# Hybrid LLC experiment configurations

These three configurations use the same 2 MiB, 16-way LLC. They differ only
in the number of SRAM ways per set:

| File | SRAM ways | STT-RAM ways |
| --- | ---: | ---: |
| `hybrid_sram_only.json` | 16 | 0 |
| `hybrid_sttram_only.json` | 0 | 16 |
| `hybrid_1to3.json` | 4 | 12 |

They model 20-cycle SRAM reads/writes, 20-cycle STT-RAM reads, and 100-cycle
STT-RAM writes. The one-cycle `hit_latency` and `fill_latency` model tag
lookup; the configured technology latency models the array access after that
lookup. Each medium has one independent read port and one independent write
port.

The default static placement policy, `read_stt_write_sram`, sends LOAD and
PREFETCH fills to STT-RAM and RFO/WRITE fills to SRAM. `hybrid_lru` chooses a
victim only within the destination medium. It does not migrate blocks.

Build one configuration at a time:

```sh
./config.sh configs/hybrid_1to3.json
make
```

Use identical traces, warmup, simulation length, core count, prefetcher, DRAM,
and random seed for all three experiments.

## Run and compare all three configurations on WSL

From the repository root in WSL, run:

```sh
bash scripts/run_hybrid_experiments.sh
```

By default, the script uses `470.lbm-1274B.champsimtrace (1).xz` in the
repository and runs a 10,000,000-instruction warmup followed by
100,000,000 measured instructions for each configuration. To use a different
trace, pass its path as the first argument:

```sh
bash scripts/run_hybrid_experiments.sh /path/to/trace.champsimtrace.xz
```

The simulator's periodic heartbeats are retained in each per-configuration
`.log` file and report retired instructions, cycles, and IPC. Build logs and
per-run JSON statistics are saved alongside them. The script writes
`comparison.csv` with measured-phase IPC, LLC hits/misses and miss rate,
average miss latency, and SRAM/STT-RAM access counts. Set `JOBS` to override
the parallel build job count, or `RESULTS_DIR` to choose an output directory.

For best trace I/O and build performance, keep the repository and trace on the
WSL Linux filesystem rather than under `/mnt/c`.

## DAM-like configuration + PHC write-intensity predictor (baseline)

| File | LLC policy | LLC replacement |
| --- | --- | --- |
| `dam_hybrid_lru.json` | `read_stt_write_sram` (sanity check) | `hybrid_lru` |
| `dam_phc.json` | `pc_predictor` | `hybrid_phc` |

Both are complete configurations (`./config.sh configs/dam_phc.json && make`)
and build separate executables (`bin/champsim_dam_hybrid_lru`,
`bin/champsim_dam_phc`).

System: 1 core, 4-wide fetch/decode/dispatch/execute/retire, ROB 128,
2660 MHz core and LLC. L1I 128x4 (32 KB, 4 cycles), L1D 64x8 (32 KB, 4 cycles),
L2 512x8 (256 KB, 8 cycles), all LRU. LLC 8192x16 (8 MB), 4 SRAM ways + 12
STT-RAM ways per set.

LLC timing: a hit pays `hit_latency` (tag, 1) + the medium latency, so the medium
latencies are 29/29/29/119 for total hit latencies of 30 (SRAM read/write,
STT-RAM read) and 120 (STT-RAM write). `test/cpp/src/460-hybrid-dam-latency.cc`
checks these totals. Read/write ports are 8 per technology (placeholder; DAM is
banked).

Differences from DAM: the hierarchy stays non-inclusive; ports are a flat
per-technology pool instead of banks; a miss fill reserves a write port and the
data is returned to the upper level after the medium write latency (existing
behavior of this code base).

### Placement decision (`hybrid_policy: pc_predictor`)

The medium is decided once per miss and stored in the MSHR / fill entry
(`fill_type::to_sram`); `finish_packet()` and `handle_fill()` use the stored
flag.

- LOAD/RFO miss allocating an MSHR: the replacement module's
  `placement_to_sram(cpu, ip, addr, type)` hook decides.
- Requests merging into an in-flight miss keep the first request's decision.
- No usable PC, fixed rule (configurable, LLC JSON keys):
  `hybrid_writeback_to_sram` (default `true`), `hybrid_prefetch_to_sram`
  (default `false`), `hybrid_translation_to_sram` (default `false`). These do
  not read or train the predictor.

The type-based policies (`read_stt_write_sram`, `read_sram_writeback_stt`,
`demand_sram_prefetch_stt`) behave exactly as before: their stored flag is
recomputed from the post-merge type, which is what the old code used.

### `replacement/hybrid_phc`

- Table: 4,096 2-bit counters, init 1, index = low 12 bits of the trigger PC,
  hot (SRAM) iff counter >= 2.
- Sampler: sets with `set % 32 == 0`, 16 entries each, plain LRU; entry =
  valid, 16-bit partial tag, LRU rank, 12-bit PC, has_pc, int8 saturating cost,
  stats-only predicted-hot bit.
- Updated once per accepted LLC request (new optional hook
  `replacement_access_accepted`, called on a hit, a successful miss/merge or a
  writeback miss, never on a tag-check retry). Only LOAD, RFO and WRITE update
  it; PREFETCH and TRANSLATION are ignored. Trains during warmup as well.
- Sampler hit: WRITE adds `DELTA_W` (+24), LOAD/RFO add `DELTA_R` (-1). Sampler
  miss: evict invalid/LRU entry, train its PC if it has one (cost >= K -> ++,
  else --), allocate with cost 0. Writeback-allocated entries have no PC.
- Constants: override with `-DPHC_DELTA_W=..`, `-DPHC_DELTA_R=1` / `0`,
  `-DPHC_K=..` (e.g. in `module.options`).
- Victim: plain LRU, invalid way first, inside the way range passed by
  `CACHE::handle_fill` (range-aware `find_victim`); WRITE hits do not update
  recency.
- Not implemented: dead-block prediction, DVS, DM, migration, threshold
  selection, set-dueling.
