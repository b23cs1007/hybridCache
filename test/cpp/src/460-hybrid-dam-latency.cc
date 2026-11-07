#include <catch.hpp>

#include "../replacement/hybrid_lru/hybrid_lru.h"
#include "cache.h"
#include "defaults.hpp"
#include "mocks.hpp"

/*
 * DAM-like LLC timing (configs/dam_*.json): hit_latency 1 (tag) + medium latency.
 * Target total hit latencies: 30 cycles for SRAM read/write and STT-RAM read,
 * 120 cycles for STT-RAM write.
 *
 * sram_ways(16) makes every way SRAM, sram_ways(0) makes every way STT-RAM, so the
 * medium a line lands in is independent of the placement policy.
 */
SCENARIO("A DAM-like hybrid LLC returns hits after the per-medium latency")
{
  using namespace std::literals;
  constexpr uint64_t tag_latency = 1;
  constexpr uint64_t sram_lat = 29, stt_read_lat = 29, stt_write_lat = 119;

  auto [sram_ways, type, expected, str] =
      GENERATE(table<uint32_t, access_type, long, std::string_view>({{16u, access_type::LOAD, 30, "SRAM read"sv},
                                                                     {16u, access_type::WRITE, 30, "SRAM write"sv},
                                                                     {0u, access_type::LOAD, 30, "STT-RAM read"sv},
                                                                     {0u, access_type::WRITE, 120, "STT-RAM write"sv}}));

  GIVEN("A hybrid LLC with " + std::string{str})
  {
    do_nothing_MRC mock_ll;
    to_rq_MRP mock_ul;
    CACHE uut{champsim::cache_builder{champsim::defaults::default_llc}
                  .name("460-uut-" + std::string{str})
                  .sets(64)
                  .ways(16)
                  .upper_levels({&mock_ul.queues})
                  .lower_level(&mock_ll.queues)
                  .hit_latency(tag_latency)
                  .fill_latency(1)
                  .sram_ways(sram_ways)
                  .sram_read_latency(sram_lat)
                  .sram_write_latency(sram_lat)
                  .sttram_read_latency(stt_read_lat)
                  .sttram_write_latency(stt_write_lat)
                  .sram_read_ports(8)
                  .sram_write_ports(8)
                  .sttram_read_ports(8)
                  .sttram_write_ports(8)
                  .hybrid_policy(champsim::hybrid_llc_policy::read_stt_write_sram)
                  .replacement<hybrid_lru>()};

    std::array<champsim::operable*, 3> elements{{&uut, &mock_ll, &mock_ul}};
    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    WHEN("A line is brought in and then accessed again")
    {
      static uint64_t id = 1;
      decltype(mock_ul)::request_type seed;
      seed.address = champsim::address{0xdeadbeef};
      seed.is_translated = true;
      seed.instr_id = id++;
      seed.cpu = 0;
      seed.type = type;

      REQUIRE(mock_ul.issue(seed));
      for (auto i = 0; i < 1000; ++i)
        for (auto elem : elements)
          elem->_operate();

      auto test = seed;
      test.instr_id = id++;
      const auto initial_hits = uut.sim_stats.hits.value_or(std::pair{test.type, test.cpu}, 0);
      REQUIRE(mock_ul.issue(test));

      for (long i = 0; i < 2 * expected + 10; ++i)
        for (auto elem : elements)
          elem->_operate();

      THEN("The second access hits and returns after exactly " + std::to_string(expected) + " cycles")
      {
        REQUIRE(uut.sim_stats.hits.value_or(std::pair{test.type, test.cpu}, 0) == initial_hits + 1);
        REQUIRE_THAT(mock_ul.packets, Catch::Matchers::SizeIs(2));
        REQUIRE_THAT(mock_ul.packets.back(), champsim::test::ReturnedMatcher(expected, 0));
      }
    }
  }
}
