#include "hybrid_phc.h"

#include <algorithm>
#include <cassert>
#include <fmt/core.h>

#include "util/bits.h"
#include "util/to_underlying.h"

hybrid_phc::hybrid_phc(CACHE* cache_)
    : replacement(cache_), NUM_SET(static_cast<long>(cache_->NUM_SET)), NUM_WAY(static_cast<long>(cache_->NUM_WAY)),
      TAG_SHIFT(static_cast<unsigned>(champsim::to_underlying(cache_->OFFSET_BITS)) + static_cast<unsigned>(champsim::lg2(cache_->NUM_SET))),
      sampler(static_cast<std::size_t>(((NUM_SET + SAMPLE_PERIOD - 1) / SAMPLE_PERIOD) * NUM_WAY)),
      last_used_cycles(static_cast<std::size_t>(NUM_SET * NUM_WAY), 0)
{
  table.fill(COUNTER_INIT);

  // Give every sampler way a distinct LRU rank so the rank array is a permutation.
  for (std::size_t i = 0; i < sampler.size(); ++i)
    sampler[i].lru = static_cast<uint32_t>(i % static_cast<std::size_t>(NUM_WAY));
}

// ---------------------------------------------------------------------------
// Predictor table
// ---------------------------------------------------------------------------

std::size_t hybrid_phc::table_index(champsim::address ip)
{
  // champsim::address -> integer via to<uint64_t>(); keep the low 12 bits.
  return static_cast<std::size_t>(ip.to<uint64_t>() & (TABLE_SIZE - 1));
}

uint8_t hybrid_phc::counter(champsim::address ip) const { return table.at(table_index(ip)); }

bool hybrid_phc::predicts_hot(champsim::address ip) const { return counter(ip) >= HOT_THRESHOLD; }

bool hybrid_phc::placement_to_sram(uint32_t, champsim::address ip, champsim::address, access_type)
{
  const bool hot = predicts_hot(ip);
  if (hot)
    ++phc_stats.placements_sram;
  else
    ++phc_stats.placements_stt;
  return hot;
}

void hybrid_phc::train(const sampler_entry& victim)
{
  assert(victim.valid && victim.has_pc);
  auto& ctr = table.at(victim.pc);
  const bool actual_hot = static_cast<int>(victim.cost) >= K;

  ++phc_stats.trainings;
  if (actual_hot) {
    ++phc_stats.counter_increments;
    if (ctr < COUNTER_MAX)
      ++ctr;
  } else {
    ++phc_stats.counter_decrements;
    if (ctr > 0)
      --ctr;
  }

  if (victim.predicted_hot)
    ++(actual_hot ? phc_stats.pred_hot_actual_hot : phc_stats.pred_hot_actual_cold);
  else
    ++(actual_hot ? phc_stats.pred_cold_actual_hot : phc_stats.pred_cold_actual_cold);
}

// ---------------------------------------------------------------------------
// Sampler
// ---------------------------------------------------------------------------

bool hybrid_phc::is_sampled_set(long set) { return (set % SAMPLE_PERIOD) == 0; }

uint16_t hybrid_phc::partial_tag(champsim::address full_addr) const
{
  const auto line_above_index = full_addr.to<uint64_t>() >> TAG_SHIFT;
  return static_cast<uint16_t>(line_above_index & ((uint64_t{1} << PARTIAL_TAG_BITS) - 1));
}

std::size_t hybrid_phc::sampler_base(long set) const { return static_cast<std::size_t>((set / SAMPLE_PERIOD) * NUM_WAY); }

int8_t hybrid_phc::saturating_add(int8_t cost, int delta)
{
  return static_cast<int8_t>(std::clamp(static_cast<int>(cost) + delta, -128, 127));
}

void hybrid_phc::sampler_touch(std::size_t base, std::size_t way)
{
  const auto old_rank = sampler[base + way].lru;
  for (long w = 0; w < NUM_WAY; ++w) {
    auto& e = sampler[base + static_cast<std::size_t>(w)];
    if (e.lru < old_rank)
      ++e.lru;
  }
  sampler[base + way].lru = 0;
}

const hybrid_phc::sampler_entry* hybrid_phc::sampler_find(long set, champsim::address full_addr) const
{
  if (!is_sampled_set(set))
    return nullptr;
  const auto base = sampler_base(set);
  const auto tag = partial_tag(full_addr);
  for (long w = 0; w < NUM_WAY; ++w) {
    const auto& e = sampler[base + static_cast<std::size_t>(w)];
    if (e.valid && e.tag == tag)
      return &e;
  }
  return nullptr;
}

void hybrid_phc::replacement_access_accepted(uint32_t, long set, champsim::address full_addr, champsim::address ip, access_type type, bool)
{
  // PREFETCH and TRANSLATION are ignored (see header).
  const bool is_read = (type == access_type::LOAD || type == access_type::RFO);
  const bool is_write = (type == access_type::WRITE);
  if (!is_read && !is_write)
    return;
  if (!is_sampled_set(set))
    return;

  ++phc_stats.sampler_accesses;
  const auto base = sampler_base(set);
  const auto tag = partial_tag(full_addr);
  auto set_begin = std::next(std::begin(sampler), static_cast<std::ptrdiff_t>(base));
  auto set_end = std::next(set_begin, NUM_WAY);

  // Sampler hit: update LRU and accumulate cost.
  auto hit_it = std::find_if(set_begin, set_end, [tag](const auto& e) { return e.valid && e.tag == tag; });
  if (hit_it != set_end) {
    ++phc_stats.sampler_hits;
    sampler_touch(base, static_cast<std::size_t>(std::distance(set_begin, hit_it)));
    hit_it->cost = saturating_add(hit_it->cost, is_write ? DELTA_W : DELTA_R);
    return;
  }

  // Sampler miss: invalid entry first, else the LRU entry.
  ++phc_stats.sampler_misses;
  auto victim = std::find_if_not(set_begin, set_end, [](const auto& e) { return e.valid; });
  if (victim == set_end)
    victim = std::max_element(set_begin, set_end, [](const auto& a, const auto& b) { return a.lru < b.lru; });

  if (victim->valid && victim->has_pc)
    train(*victim);

  // Allocate. The allocating access itself starts the entry at cost 0 (PHC Fig. 3).
  victim->valid = true;
  victim->tag = tag;
  victim->cost = 0;
  victim->has_pc = is_read; // only LOAD/RFO carry a usable trigger PC
  victim->pc = is_read ? static_cast<uint16_t>(table_index(ip)) : uint16_t{0};
  victim->predicted_hot = is_read && predicts_hot(ip);
  if (!is_read)
    ++phc_stats.sampler_allocs_no_pc;
  sampler_touch(base, static_cast<std::size_t>(std::distance(set_begin, victim)));
}

// ---------------------------------------------------------------------------
// Real-cache LRU inside the chosen portion
// ---------------------------------------------------------------------------

long hybrid_phc::find_victim(uint32_t, uint64_t, long set, const champsim::cache_block* current_set, champsim::address, champsim::address, access_type,
                             long first_way, long last_way)
{
  assert(0 <= first_way && first_way < last_way && last_way <= NUM_WAY);

  // Invalid way first
  for (long w = first_way; w < last_way; ++w) {
    if (!current_set[w].valid)
      return w;
  }

  auto begin = std::next(std::begin(last_used_cycles), set * NUM_WAY + first_way);
  auto end = std::next(std::begin(last_used_cycles), set * NUM_WAY + last_way);
  return first_way + std::distance(begin, std::min_element(begin, end));
}

void hybrid_phc::replacement_cache_fill(uint32_t, long set, long way, champsim::address, champsim::address, champsim::address, access_type)
{
  if (way < NUM_WAY)
    last_used_cycles.at(static_cast<std::size_t>(set * NUM_WAY + way)) = ++cycle;
}

void hybrid_phc::update_replacement_state(uint32_t, long set, long way, champsim::address, champsim::address, champsim::address, access_type type, uint8_t hit)
{
  // Same as hybrid_lru: writeback hits do not update recency.
  if (hit && type != access_type::WRITE && way < NUM_WAY)
    last_used_cycles.at(static_cast<std::size_t>(set * NUM_WAY + way)) = ++cycle;
}

void hybrid_phc::replacement_final_stats()
{
  const auto& s = phc_stats;
  const auto hot_counters = std::count_if(std::begin(table), std::end(table), [](auto c) { return c >= HOT_THRESHOLD; });

  fmt::print("PHC (DELTA_W={} DELTA_R={} K={}) [cumulative, includes warmup]\n", DELTA_W, DELTA_R, K);
  fmt::print("PHC placement SRAM: {} STT-RAM: {}\n", s.placements_sram, s.placements_stt);
  fmt::print("PHC sampler accesses: {} hits: {} misses: {} no-PC allocations: {}\n", s.sampler_accesses, s.sampler_hits, s.sampler_misses,
             s.sampler_allocs_no_pc);
  fmt::print("PHC trainings: {} increments: {} decrements: {}\n", s.trainings, s.counter_increments, s.counter_decrements);
  fmt::print("PHC prediction vs. outcome: hot/hot: {} hot/cold: {} cold/hot: {} cold/cold: {}\n", s.pred_hot_actual_hot, s.pred_hot_actual_cold,
             s.pred_cold_actual_hot, s.pred_cold_actual_cold);
  fmt::print("PHC table entries predicting hot: {} / {}\n", hot_counters, TABLE_SIZE);
}
