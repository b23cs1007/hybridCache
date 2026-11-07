#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TRACE_PATH="${1:-$ROOT_DIR/470.lbm-1274B.champsimtrace (1).xz}"
WARMUP_INSTRUCTIONS=10000000
SIMULATION_INSTRUCTIONS=100000000
JOBS="${JOBS:-$(nproc)}"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${RESULTS_DIR:-$ROOT_DIR/results/hybrid_llc_$TIMESTAMP}"

if [[ ! -f "$TRACE_PATH" ]]; then
  printf 'ERROR: trace file not found: %s\n' "$TRACE_PATH" >&2
  printf 'Usage: %s [path/to/trace.champsimtrace.xz]\n' "$0" >&2
  exit 1
fi
TRACE_PATH="$(realpath -- "$TRACE_PATH")"

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

configs=(
  "hybrid_sram_only:16"
  "hybrid_sttram_only:0"
  "hybrid_1to3:4"
)

for entry in "${configs[@]}"; do
  name="${entry%%:*}"
  config_path="$ROOT_DIR/configs/$name.json"
  log_path="$OUT_DIR/$name.log"
  json_path="$OUT_DIR/$name.json"

  printf '\n===== %s | SRAM ways: %s | trace: %s =====\n' "$name" "${entry##*:}" "$TRACE_PATH" \
    | tee "$log_path"
  printf 'Building %s\n' "$name" | tee -a "$log_path"
  (
    cd "$ROOT_DIR"
    ./config.sh "$config_path"
    make -j"$JOBS"
  ) 2>&1 | tee "$OUT_DIR/$name.build.log"

  printf 'Starting simulation: warmup=%s instructions, measured=%s instructions\n' \
    "$WARMUP_INSTRUCTIONS" "$SIMULATION_INSTRUCTIONS" | tee -a "$log_path"
  printf 'ChampSim heartbeats report retired instructions, cycles, heartbeat IPC, and cumulative IPC.\n' \
    | tee -a "$log_path"
  (
    cd "$ROOT_DIR"
    stdbuf -oL -eL ./bin/champsim \
      --warmup-instructions "$WARMUP_INSTRUCTIONS" \
      --simulation-instructions "$SIMULATION_INSTRUCTIONS" \
      --json "$json_path" \
      "$TRACE_PATH"
  ) 2>&1 | tee -a "$log_path"
done

python3 - "$OUT_DIR" "$TRACE_PATH" <<'PY'
import csv
import json
import os
import sys

out_dir, trace_path = sys.argv[1:]
configs = (
    ("hybrid_sram_only", 16),
    ("hybrid_sttram_only", 0),
    ("hybrid_1to3", 4),
)
access_types = ("LOAD", "RFO", "PREFETCH", "WRITE", "TRANSLATION")
rows = []

for name, sram_ways in configs:
    json_path = os.path.join(out_dir, f"{name}.json")
    with open(json_path, encoding="utf-8") as result_file:
        phases = json.load(result_file)

    phase = next((item for item in phases if item.get("name") == "Simulation"), None)
    if phase is None:
        raise SystemExit(f"ERROR: Simulation phase missing from {json_path}")
    roi = phase["roi"]
    cores = roi["cores"]
    llc = roi["LLC"]
    instructions = sum(int(core["instructions"]) for core in cores)
    cycles = sum(int(core["cycles"]) for core in cores)
    if cycles <= 0:
        raise SystemExit(f"ERROR: invalid cycle count in {json_path}: {cycles}")

    hits = sum(sum(int(value) for value in llc[key]["hit"]) for key in access_types)
    misses = sum(sum(int(value) for value in llc[key]["miss"]) for key in access_types)
    accesses = hits + misses
    hybrid = llc["hybrid LLC"]
    rows.append({
        "configuration": name,
        "SRAM_ways": sram_ways,
        "trace": trace_path,
        "warmup_instructions": 10000000,
        "simulation_instructions": 100000000,
        "instructions_measured": instructions,
        "cycles_measured": cycles,
        "IPC": instructions / cycles,
        "LLC_accesses": accesses,
        "LLC_hits": hits,
        "LLC_misses": misses,
        "LLC_miss_rate": misses / accesses if accesses else 0,
        "average_miss_latency_cycles": llc["miss latency"],
        "SRAM_reads": hybrid["SRAM reads"],
        "SRAM_writes": hybrid["SRAM writes"],
        "STTRAM_reads": hybrid["STTRAM reads"],
        "STTRAM_writes": hybrid["STTRAM writes"],
    })

csv_path = os.path.join(out_dir, "comparison.csv")
with open(csv_path, "w", newline="", encoding="utf-8") as csv_file:
    writer = csv.DictWriter(csv_file, fieldnames=rows[0].keys())
    writer.writeheader()
    writer.writerows(rows)

print(f"\nComparison CSV written to: {csv_path}")
PY

printf '\nAll three simulations completed. Logs, JSON results, and CSV: %s\n' "$OUT_DIR"
