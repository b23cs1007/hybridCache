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
