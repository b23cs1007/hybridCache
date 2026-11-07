#include "hybrid_lru.h"

#include <algorithm>
#include <cassert>

hybrid_lru::hybrid_lru(CACHE* cache_) : replacement(cache_), cache(cache_), NUM_WAY(cache_->NUM_WAY),
                                        last_used_cycles(static_cast<std::size_t>(cache_->NUM_SET * cache_->NUM_WAY), 0)
{
}

long hybrid_lru::find_victim(uint32_t, uint64_t, long set, const champsim::cache_block*, champsim::address, champsim::address, access_type type)
{
  const auto [first_way, last_way] = cache->hybrid_way_range(type);
  assert(first_way < last_way);
  auto begin = std::next(std::begin(last_used_cycles), set * NUM_WAY + first_way);
  auto end = std::next(std::begin(last_used_cycles), set * NUM_WAY + last_way);
  auto victim = std::min_element(begin, end);
  return first_way + std::distance(begin, victim);
}

void hybrid_lru::replacement_cache_fill(uint32_t, long set, long way, champsim::address, champsim::address, champsim::address, access_type)
{
  last_used_cycles.at(static_cast<std::size_t>(set * NUM_WAY + way)) = cycle++;
}

void hybrid_lru::update_replacement_state(uint32_t, long set, long way, champsim::address, champsim::address, champsim::address, access_type type, uint8_t hit)
{
  if (hit && type != access_type::WRITE)
    last_used_cycles.at(static_cast<std::size_t>(set * NUM_WAY + way)) = cycle++;
}
