#ifndef REPLACEMENT_HYBRID_PHC_H
#define REPLACEMENT_HYBRID_PHC_H

/*
 * PHC write-intensity predictor for a hybrid SRAM/STT-RAM LLC (baseline).
 *
 *   Ahn, Yoo, Choi, "Prediction Hybrid Cache: An Energy-Efficient STT-RAM Cache
 *   Architecture", IEEE Transactions on Computers, 2016.
 *
 * What is implemented (single core, no per-core state):
 *   - Predictor: 4,096 tagless, direct-mapped 2-bit saturating counters indexed by
 *     the low 12 bits of the trigger PC. Initial value 1. 0-1 = cold, 2-3 = hot.
 *     A hot prediction places the block in SRAM, cold in STT-RAM.
 *   - Sampler: tag-only array over the sets with set % 32 == 0, NUM_WAY entries per
 *     sampled set, plain LRU, independent of the real cache. Each entry holds
 *     valid, 16-bit partial tag, LRU rank, 12-bit PC, has_pc, int8 saturating cost,
 *     and a stats-only "predicted hot at allocation" bit.
 *   - Training: when a valid entry with a PC is evicted from the sampler,
 *     cost >= K increments that PC's counter, otherwise decrements it.
 *   - Victim: plain LRU restricted to the way range chosen by the cache (the range
 *     is passed in by CACHE::handle_fill from the stored placement decision).
 *
 * What is NOT implemented (on purpose): dead-block prediction, DVS, DM, migration,
 * threshold selection, set-dueling.
 *
 * Sampler access filter: only LOAD, RFO and WRITE (writeback) accesses update the
 * sampler. PREFETCH and TRANSLATION accesses are ignored: prefetches carry no
 * trigger PC and are not part of the write-intensity cost, and translations are
 * page-walk reads with no program PC. Entries allocated by writebacks (WRITE
 * sampler misses) have no PC and never train, but they still occupy sampler ways.
 *
 * Compile-time constants (override with -D on the command line, e.g. in module.options
 * or CXXFLAGS):
 *   PHC_DELTA_W  cost added per write (writeback) hit     default +24
 *   PHC_DELTA_R  cost added per read (LOAD/RFO) hit       default -1  (variants: +1, 0)
 *   PHC_K        write-intensity threshold (cost >= K)    default 24
 */

#include <array>
#include <cstdint>
#include <vector>

#include "cache.h"
#include "modules.h"

#ifndef PHC_DELTA_W
#define PHC_DELTA_W 24
#endif

#ifndef PHC_DELTA_R
#define PHC_DELTA_R (-1)
#endif

#ifndef PHC_K
#define PHC_K 24
#endif

class hybrid_phc : public champsim::modules::replacement
{
public:
  static constexpr unsigned TABLE_INDEX_BITS = 12;
  static constexpr std::size_t TABLE_SIZE = std::size_t{1} << TABLE_INDEX_BITS;
  static constexpr uint8_t COUNTER_MAX = 3;
  static constexpr uint8_t COUNTER_INIT = 1;
  static constexpr uint8_t HOT_THRESHOLD = 2;

  static constexpr long SAMPLE_PERIOD = 32; // simple-static selection: set % 32 == 0
  static constexpr unsigned PARTIAL_TAG_BITS = 16;

  static constexpr int DELTA_W = PHC_DELTA_W;
  static constexpr int DELTA_R = PHC_DELTA_R;
  static constexpr int K = PHC_K;

  static_assert(DELTA_W >= -128 && DELTA_W <= 127, "PHC_DELTA_W must fit the int8 cost");
  static_assert(DELTA_R >= -128 && DELTA_R <= 127, "PHC_DELTA_R must fit the int8 cost");
  static_assert(K >= -128 && K <= 127, "PHC_K must be comparable with an int8 cost");

  struct sampler_entry {
    bool valid = false;
    uint16_t tag = 0;      // low 16 bits of the line address above the set-index bits
    uint32_t lru = 0;      // rank: 0 = MRU, NUM_WAY-1 = LRU
    uint16_t pc = 0;       // low 12 bits of the trigger PC
    bool has_pc = false;   // false for writeback-allocated entries
    int8_t cost = 0;       // saturating
    bool predicted_hot = false; // stats only: table prediction at allocation time
  };

  struct stats_type {
    uint64_t placements_sram = 0; // placement_to_sram() calls predicting hot
    uint64_t placements_stt = 0;  // placement_to_sram() calls predicting cold

    uint64_t sampler_accesses = 0;
    uint64_t sampler_hits = 0;
    uint64_t sampler_misses = 0;
    uint64_t sampler_allocs_no_pc = 0; // allocated by writebacks

    uint64_t trainings = 0;
    uint64_t counter_increments = 0; // cost >= K (attempted, including saturated)
    uint64_t counter_decrements = 0; // cost < K (attempted, including saturated)

    // Prediction at sampler allocation vs. observed outcome at sampler eviction
    uint64_t pred_hot_actual_hot = 0;
    uint64_t pred_hot_actual_cold = 0;
    uint64_t pred_cold_actual_hot = 0;
    uint64_t pred_cold_actual_cold = 0;
  };

  explicit hybrid_phc(CACHE* cache);

  // --- hybrid placement hook (CACHE::handle_miss, LOAD/RFO MSHR allocation) ---
  bool placement_to_sram(uint32_t triggering_cpu, champsim::address ip, champsim::address full_addr, access_type type);

  // --- sampler hook: once per accepted request ---
  void replacement_access_accepted(uint32_t triggering_cpu, long set, champsim::address full_addr, champsim::address ip, access_type type, bool hit);

  // --- victim selection in the chosen portion ---
  long find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set, const champsim::cache_block* current_set, champsim::address ip,
                   champsim::address full_addr, access_type type, long first_way, long last_way);
  void replacement_cache_fill(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip, champsim::address victim_addr,
                              access_type type);
  void update_replacement_state(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip, champsim::address victim_addr,
                                access_type type, uint8_t hit);
  void replacement_final_stats();

  // --- introspection (tests / debugging) ---
  [[nodiscard]] static std::size_t table_index(champsim::address ip);
  [[nodiscard]] uint8_t counter(champsim::address ip) const;
  [[nodiscard]] bool predicts_hot(champsim::address ip) const;
  [[nodiscard]] static bool is_sampled_set(long set);
  [[nodiscard]] uint16_t partial_tag(champsim::address full_addr) const;
  [[nodiscard]] const sampler_entry* sampler_find(long set, champsim::address full_addr) const;
  [[nodiscard]] const stats_type& stats() const { return phc_stats; }

private:
  long NUM_SET;
  long NUM_WAY;
  unsigned TAG_SHIFT; // OFFSET_BITS + log2(NUM_SET)

  std::array<uint8_t, TABLE_SIZE> table{};
  std::vector<sampler_entry> sampler; // (NUM_SET / 32) sampled sets x NUM_WAY entries

  // Real-cache LRU (recency timestamps per block)
  std::vector<uint64_t> last_used_cycles;
  uint64_t cycle = 0;

  stats_type phc_stats{};

  [[nodiscard]] std::size_t sampler_base(long set) const;
  void sampler_touch(std::size_t base, std::size_t way);
  void train(const sampler_entry& victim);
  static int8_t saturating_add(int8_t cost, int delta);
};

#endif
