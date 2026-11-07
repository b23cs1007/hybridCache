#include <catch.hpp>

#include "../replacement/hybrid_lru/hybrid_lru.h"
#include "../replacement/hybrid_phc/hybrid_phc.h"
#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"

namespace
{
// 64 sets, 64-byte blocks: set index = bits [6,12), PHC partial tag = bits [12,28).
constexpr uint64_t OFFSET = 6;
constexpr uint64_t SET_BITS = 6;
champsim::address line_addr(uint64_t tag, uint64_t set) { return champsim::address{(tag << (OFFSET + SET_BITS)) | (set << OFFSET)}; }

auto phc_builder(std::string name)
{
  return champsim::cache_builder{champsim::defaults::default_llc}
      .name(std::move(name))
      .sets(64)
      .ways(16)
      .offset_bits(champsim::data::bits{OFFSET})
      .hit_latency(1)
      .fill_latency(1)
      .sram_ways(4)
      .hybrid_policy(champsim::hybrid_llc_policy::pc_predictor)
      .replacement<hybrid_phc>();
}

hybrid_phc& get_phc(CACHE& cache)
{
  auto* model = dynamic_cast<CACHE::replacement_module_model<hybrid_phc>*>(cache.repl_module_pimpl.get());
  REQUIRE(model != nullptr);
  return std::get<hybrid_phc>(model->intern_);
}
} // namespace

TEST_CASE("PHC predictor table geometry and initial state")
{
  CACHE cache{phc_builder("461-table")};
  auto& uut = get_phc(cache);

  STATIC_REQUIRE(hybrid_phc::TABLE_SIZE == 4096);
  STATIC_REQUIRE(hybrid_phc::DELTA_W == 24);
  STATIC_REQUIRE(hybrid_phc::DELTA_R == -1);
  STATIC_REQUIRE(hybrid_phc::K == 24);

  CHECK(uut.counter(champsim::address{0x400123}) == 1);
  CHECK_FALSE(uut.predicts_hot(champsim::address{0x400123}));
  CHECK_FALSE(uut.placement_to_sram(0, champsim::address{0x400123}, champsim::address{}, access_type::LOAD));

  // Index = low 12 bits of the PC
  CHECK(hybrid_phc::table_index(champsim::address{0x12345}) == 0x345);
  CHECK(hybrid_phc::table_index(champsim::address{0xfff}) == 0xfff);
  CHECK(hybrid_phc::table_index(champsim::address{0x1000}) == 0);

  // Sampled sets: set % 32 == 0
  CHECK(hybrid_phc::is_sampled_set(0));
  CHECK(hybrid_phc::is_sampled_set(32));
  CHECK_FALSE(hybrid_phc::is_sampled_set(1));
  CHECK_FALSE(hybrid_phc::is_sampled_set(31));
}

TEST_CASE("PHC sampler trains the predictor at sampler eviction")
{
  CACHE cache{phc_builder("461-train")};
  auto& uut = get_phc(cache);

  constexpr long sampled = 0;
  const champsim::address pc_a{0x401abc};
  const champsim::address pc_other{0x402000};
  const auto x = line_addr(0x77, sampled);

  auto evict_with_loads = [&](uint64_t first_tag) {
    // 16 distinct LOAD misses with another PC push x out of the 16-way sampler set
    for (uint64_t t = first_tag; t < first_tag + 16; ++t)
      uut.replacement_access_accepted(0, sampled, line_addr(t, sampled), pc_other, access_type::LOAD, false);
  };

  SECTION("A write-intensive block (cost >= K) increments its PC's counter")
  {
    uut.replacement_access_accepted(0, sampled, x, pc_a, access_type::LOAD, false); // allocate, cost 0
    REQUIRE(uut.sampler_find(sampled, x) != nullptr);
    CHECK(uut.sampler_find(sampled, x)->cost == 0);
    CHECK(uut.sampler_find(sampled, x)->has_pc);

    uut.replacement_access_accepted(0, sampled, x, {}, access_type::WRITE, true); // writeback hit, +24
    CHECK(uut.sampler_find(sampled, x)->cost == 24);

    evict_with_loads(0x1000);
    CHECK(uut.sampler_find(sampled, x) == nullptr);
    CHECK(uut.counter(pc_a) == 2);
    CHECK(uut.predicts_hot(pc_a));
    CHECK(uut.placement_to_sram(0, pc_a, {}, access_type::LOAD));
  }

  SECTION("A read-only block (cost < K) decrements its PC's counter")
  {
    uut.replacement_access_accepted(0, sampled, x, pc_a, access_type::LOAD, false);
    uut.replacement_access_accepted(0, sampled, x, pc_a, access_type::LOAD, true); // read hit, -1
    CHECK(uut.sampler_find(sampled, x)->cost == -1);

    evict_with_loads(0x1000);
    CHECK(uut.counter(pc_a) == 0);
  }

  SECTION("Writes then reads: cost 24 - 1 = 23 < K is cold")
  {
    uut.replacement_access_accepted(0, sampled, x, pc_a, access_type::RFO, false);
    uut.replacement_access_accepted(0, sampled, x, {}, access_type::WRITE, true);
    uut.replacement_access_accepted(0, sampled, x, pc_a, access_type::LOAD, true);
    CHECK(uut.sampler_find(sampled, x)->cost == 23);
    evict_with_loads(0x1000);
    CHECK(uut.counter(pc_a) == 0);
  }

  SECTION("Counters saturate at 3 and at 0")
  {
    for (int round = 0; round < 5; ++round) {
      uut.replacement_access_accepted(0, sampled, x, pc_a, access_type::LOAD, false);
      uut.replacement_access_accepted(0, sampled, x, {}, access_type::WRITE, true);
      evict_with_loads(0x1000 + 0x100 * static_cast<uint64_t>(round));
    }
    CHECK(uut.counter(pc_a) == 3);

    for (int round = 0; round < 5; ++round) {
      uut.replacement_access_accepted(0, sampled, x, pc_a, access_type::LOAD, false);
      evict_with_loads(0x8000 + 0x100 * static_cast<uint64_t>(round));
    }
    CHECK(uut.counter(pc_a) == 0);
  }

  SECTION("Cost is a saturating int8")
  {
    uut.replacement_access_accepted(0, sampled, x, pc_a, access_type::LOAD, false);
    for (int i = 0; i < 10; ++i)
      uut.replacement_access_accepted(0, sampled, x, {}, access_type::WRITE, true);
    CHECK(uut.sampler_find(sampled, x)->cost == 127);
  }

  SECTION("Writeback-allocated entries have no PC and never train, but occupy ways")
  {
    uut.replacement_access_accepted(0, sampled, x, {}, access_type::WRITE, false);
    REQUIRE(uut.sampler_find(sampled, x) != nullptr);
    CHECK_FALSE(uut.sampler_find(sampled, x)->has_pc);
    uut.replacement_access_accepted(0, sampled, x, {}, access_type::WRITE, true);

    const auto trainings_before = uut.stats().trainings;
    evict_with_loads(0x1000);
    CHECK(uut.sampler_find(sampled, x) == nullptr); // it occupied a way and was evicted
    CHECK(uut.stats().trainings == trainings_before);
    CHECK(uut.counter(champsim::address{0}) == 1);
  }

  SECTION("Plain LRU: a recently hit entry survives")
  {
    uut.replacement_access_accepted(0, sampled, x, pc_a, access_type::LOAD, false);
    for (uint64_t t = 0x1000; t < 0x1000 + 15; ++t) // fills the 15 remaining ways
      uut.replacement_access_accepted(0, sampled, line_addr(t, sampled), pc_other, access_type::LOAD, false);
    uut.replacement_access_accepted(0, sampled, x, pc_a, access_type::LOAD, true); // x becomes MRU
    uut.replacement_access_accepted(0, sampled, line_addr(0x2000, sampled), pc_other, access_type::LOAD, false);
    CHECK(uut.sampler_find(sampled, x) != nullptr);
    CHECK(uut.sampler_find(sampled, line_addr(0x1000, sampled)) == nullptr); // the LRU one went
  }

  SECTION("Non-sampled sets, prefetches and translations do not touch the sampler")
  {
    uut.replacement_access_accepted(0, 1, line_addr(0x77, 1), pc_a, access_type::LOAD, false);
    uut.replacement_access_accepted(0, sampled, x, pc_a, access_type::PREFETCH, false);
    uut.replacement_access_accepted(0, sampled, x, pc_a, access_type::TRANSLATION, false);
    CHECK(uut.stats().sampler_accesses == 0);
    CHECK(uut.sampler_find(sampled, x) == nullptr);
  }
}

TEST_CASE("PHC victim selection is LRU inside the way range passed by the cache")
{
  CACHE cache{phc_builder("461-victim")};
  auto& uut = get_phc(cache);

  constexpr long set = 3;
  std::vector<champsim::cache_block> blocks(16);
  for (long w = 0; w < 16; ++w) {
    blocks[static_cast<std::size_t>(w)].valid = true;
    uut.replacement_cache_fill(0, set, w, {}, {}, {}, access_type::LOAD);
  }
  // Way 4 is the oldest fill; touching it makes way 5 the LRU STT-RAM way.
  uut.update_replacement_state(0, set, 4, {}, {}, {}, access_type::LOAD, 1);
  CHECK(uut.find_victim(0, 0, set, blocks.data(), {}, {}, access_type::LOAD, 4, 16) == 5);
  CHECK(uut.find_victim(0, 0, set, blocks.data(), {}, {}, access_type::LOAD, 0, 4) == 0);

  // Writeback hits do not update recency
  uut.update_replacement_state(0, set, 5, {}, {}, {}, access_type::WRITE, 1);
  CHECK(uut.find_victim(0, 0, set, blocks.data(), {}, {}, access_type::LOAD, 4, 16) == 5);

  // Invalid way first
  blocks[9].valid = false;
  CHECK(uut.find_victim(0, 0, set, blocks.data(), {}, {}, access_type::LOAD, 4, 16) == 9);
  CHECK(uut.find_victim(0, 0, set, blocks.data(), {}, {}, access_type::LOAD, 0, 4) == 0);
}

namespace
{
long filled_way(const CACHE& cache, champsim::address addr)
{
  const auto set = static_cast<std::size_t>((addr.to<uint64_t>() >> OFFSET) & ((1u << SET_BITS) - 1));
  for (std::size_t w = 0; w < cache.NUM_WAY; ++w) {
    const auto& b = cache.block[set * cache.NUM_WAY + w];
    if (b.valid && (b.address.to<uint64_t>() >> OFFSET) == (addr.to<uint64_t>() >> OFFSET))
      return static_cast<long>(w);
  }
  return -1;
}
} // namespace

SCENARIO("The pc_predictor policy decides the medium once at MSHR allocation")
{
  GIVEN("A 4-SRAM / 12-STT hybrid LLC with the PHC module")
  {
    do_nothing_MRC mock_ll{5};
    to_rq_MRP mock_rq;
    to_wq_MRP mock_wq;
    to_pq_MRP mock_pq;
    CACHE uut{phc_builder("461-integration").upper_levels({&mock_rq.queues, &mock_wq.queues, &mock_pq.queues}).lower_level(&mock_ll.queues)};
    auto& phc = get_phc(uut);

    std::array<champsim::operable*, 5> elements{{&uut, &mock_ll, &mock_rq, &mock_wq, &mock_pq}};
    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    // Train pc_hot to "hot" through the sampler (set 0), leave pc_cold at its initial value.
    const champsim::address pc_hot{0x401abc};
    const champsim::address pc_cold{0x402def};
    for (int round = 0; round < 2; ++round) {
      const auto t = line_addr(0x900 + static_cast<uint64_t>(round), 0);
      phc.replacement_access_accepted(0, 0, t, pc_hot, access_type::LOAD, false);
      phc.replacement_access_accepted(0, 0, t, {}, access_type::WRITE, true);
      for (uint64_t k = 0; k < 16; ++k)
        phc.replacement_access_accepted(0, 0, line_addr(0xa00 + 0x20 * static_cast<uint64_t>(round) + k, 0), pc_cold, access_type::WRITE, false);
    }
    REQUIRE(phc.predicts_hot(pc_hot));
    REQUIRE_FALSE(phc.predicts_hot(pc_cold));

    auto run = [&] {
      for (int i = 0; i < 400; ++i)
        for (auto elem : elements)
          elem->_operate();
    };
    auto make = [](champsim::address addr, champsim::address ip, access_type type) {
      static uint64_t id = 1;
      champsim::channel::request_type pkt;
      pkt.address = addr;
      pkt.ip = ip;
      pkt.type = type;
      pkt.cpu = 0;
      pkt.is_translated = true;
      pkt.instr_id = id++;
      pkt.response_requested = (type != access_type::WRITE);
      return pkt;
    };

    // Non-sampled sets so the integration traffic does not retrain the table
    const auto a_hot = line_addr(0x11, 1);
    const auto a_cold = line_addr(0x12, 2);
    const auto a_wb = line_addr(0x13, 3);
    const auto a_pf = line_addr(0x14, 5);
    const auto a_merge = line_addr(0x15, 6);

    WHEN("Misses of different kinds are filled")
    {
      REQUIRE(mock_rq.issue(make(a_hot, pc_hot, access_type::LOAD)));
      REQUIRE(mock_rq.issue(make(a_cold, pc_cold, access_type::RFO)));
      REQUIRE(mock_wq.issue(make(a_wb, {}, access_type::WRITE)));
      REQUIRE(mock_pq.issue(make(a_pf, pc_hot, access_type::PREFETCH)));
      run();

      THEN("Hot-PC demand misses go to SRAM, cold-PC to STT-RAM, writebacks to SRAM, prefetches to STT-RAM")
      {
        CHECK(filled_way(uut, a_hot) >= 0);
        CHECK(filled_way(uut, a_hot) < 4);
        CHECK(filled_way(uut, a_cold) >= 4);
        CHECK(filled_way(uut, a_wb) >= 0);
        CHECK(filled_way(uut, a_wb) < 4);
        CHECK(filled_way(uut, a_pf) >= 4);
        // Only the two demand misses consulted the predictor
        CHECK(phc.stats().placements_sram + phc.stats().placements_stt == 2);
      }
    }

    WHEN("A hot-PC load merges into an in-flight cold-PC miss")
    {
      REQUIRE(mock_rq.issue(make(a_merge, pc_cold, access_type::LOAD)));
      REQUIRE(mock_rq.issue(make(a_merge, pc_hot, access_type::LOAD)));
      run();

      THEN("The first request's decision (STT-RAM) is kept")
      {
        CHECK(filled_way(uut, a_merge) >= 4);
        CHECK(uut.sim_stats.miss_merge.value_or(std::pair{access_type::LOAD, 0u}, 0) == 1);
        CHECK(phc.stats().placements_sram + phc.stats().placements_stt == 1);
      }
    }
  }
}

SCENARIO("Type-based policies keep their old placement")
{
  GIVEN("A 4-SRAM / 12-STT hybrid LLC with hybrid_lru and read_stt_write_sram")
  {
    do_nothing_MRC mock_ll{5};
    to_rq_MRP mock_rq;
    CACHE uut{champsim::cache_builder{champsim::defaults::default_llc}
                  .name("461-legacy")
                  .sets(64)
                  .ways(16)
                  .offset_bits(champsim::data::bits{OFFSET})
                  .upper_levels({&mock_rq.queues})
                  .lower_level(&mock_ll.queues)
                  .hit_latency(1)
                  .fill_latency(1)
                  .sram_ways(4)
                  .hybrid_policy(champsim::hybrid_llc_policy::read_stt_write_sram)
                  .replacement<hybrid_lru>()};

    std::array<champsim::operable*, 3> elements{{&uut, &mock_ll, &mock_rq}};
    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    WHEN("A LOAD and an RFO miss are filled")
    {
      champsim::channel::request_type load;
      load.address = line_addr(0x21, 1);
      load.type = access_type::LOAD;
      load.cpu = 0;
      load.is_translated = true;
      load.instr_id = 1;
      auto rfo = load;
      rfo.address = line_addr(0x22, 2);
      rfo.type = access_type::RFO;
      rfo.instr_id = 2;

      REQUIRE(mock_rq.issue(load));
      REQUIRE(mock_rq.issue(rfo));
      for (int i = 0; i < 400; ++i)
        for (auto elem : elements)
          elem->_operate();

      THEN("LOAD goes to STT-RAM and RFO goes to SRAM")
      {
        CHECK(filled_way(uut, load.address) >= 4);
        CHECK(filled_way(uut, rfo.address) >= 0);
        CHECK(filled_way(uut, rfo.address) < 4);
      }
    }
  }
}
