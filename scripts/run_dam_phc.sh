#!/usr/bin/env bash
# Build and run the DAM-like hybrid LLC configurations on one trace:
#   dam_hybrid_lru : read_stt_write_sram + hybrid_lru   (Step 1 sanity check)
#   dam_phc        : pc_predictor        + hybrid_phc   (PHC baseline)
#
# Usage:
#   bash scripts/run_dam_phc.sh [path/to/trace.champsimtrace.xz]
# Environment:
#   CONFIGS="dam_hybrid_lru dam_phc"   which configs to run (default: both, in this order)
#   WARMUP=10000000 SIM=100000000      instruction counts
#   JOBS=<n>                           parallel build jobs (default: nproc)
#   RESULTS_DIR=<dir>                  output directory
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TRACE_PATH="${1:-$ROOT_DIR/470.lbm-1274B.champsimtrace (1).xz}"
WARMUP_INSTRUCTIONS="${WARMUP:-10000000}"
SIMULATION_INSTRUCTIONS="${SIM:-100000000}"
JOBS="${JOBS:-$(nproc)}"
read -r -a CONFIG_LIST <<< "${CONFIGS:-dam_hybrid_lru dam_phc}"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${RESULTS_DIR:-$ROOT_DIR/results/dam_phc_$TIMESTAMP}"

if [[ ! -f "$TRACE_PATH" ]]; then
  printf 'ERROR: trace file not found: %s\n' "$TRACE_PATH" >&2
  printf 'Usage: %s [path/to/trace.champsimtrace.xz]\n' "$0" >&2
  exit 1
fi
TRACE_PATH="$(realpath -- "$TRACE_PATH")"

# Preflight: Windows (CRLF) line endings break config.sh's shebang and Makefile recipes.
if head -1 "$ROOT_DIR/config.sh" | grep -q $'\r'; then
  printf 'ERROR: config.sh has CRLF line endings. Convert the checkout to LF first, e.g.:\n' >&2
  printf "  git ls-files --eol | awk -F'\\\\t' '{split(\$1,a,\" \"); if (a[2]==\"w/crlf\") print \$2}' | while IFS= read -r f; do sed -i 's/\\\\r\$//' \"\$f\"; done\n" >&2
  exit 1
fi
# Preflight: ChampSim's library dependencies come from vcpkg.
if [[ ! -d "$ROOT_DIR/vcpkg_installed" ]]; then
  printf 'ERROR: vcpkg_installed/ not found. Install dependencies once with:\n' >&2
  printf '  git submodule update --init && vcpkg/bootstrap-vcpkg.sh && vcpkg/vcpkg install\n' >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

# Read a top-level key from a JSON config
json_key() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get(sys.argv[2], ""))' "$1" "$2"; }

for name in "${CONFIG_LIST[@]}"; do
  config_path="$ROOT_DIR/configs/$name.json"
  log_path="$OUT_DIR/$name.log"
  json_path="$OUT_DIR/$name.json"

  if [[ ! -f "$config_path" ]]; then
    printf 'ERROR: config not found: %s\n' "$config_path" >&2
    exit 1
  fi

  exe_name="$(json_key "$config_path" executable_name)"
  heartbeat="$(json_key "$config_path" heartbeat_frequency)"
  exe_name="${exe_name:-champsim}"

  printf '\n===== %s | trace: %s =====\n' "$name" "$TRACE_PATH" | tee "$log_path"
  printf 'Config: %s\nExecutable: bin/%s\nHeartbeat every %s instructions\n' \
    "$config_path" "$exe_name" "${heartbeat:-default}" | tee -a "$log_path"

  printf 'Building %s (log: %s)\n' "$name" "$OUT_DIR/$name.build.log" | tee -a "$log_path"
  (
    cd "$ROOT_DIR"
    ./config.sh "$config_path"
    make -j"$JOBS"
  ) 2>&1 | tee "$OUT_DIR/$name.build.log"

  printf 'Starting simulation: warmup=%s, measured=%s instructions\n' \
    "$WARMUP_INSTRUCTIONS" "$SIMULATION_INSTRUCTIONS" | tee -a "$log_path"
  start_s=$(date +%s)
  (
    cd "$ROOT_DIR"
    stdbuf -oL -eL "./bin/$exe_name" \
      --warmup-instructions "$WARMUP_INSTRUCTIONS" \
      --simulation-instructions "$SIMULATION_INSTRUCTIONS" \
      --json "$json_path" \
      "$TRACE_PATH"
  ) 2>&1 | tee -a "$log_path"
  printf 'Wall time for %s: %ss\n' "$name" "$(( $(date +%s) - start_s ))" | tee -a "$log_path"
done

python3 - "$OUT_DIR" "$TRACE_PATH" "$WARMUP_INSTRUCTIONS" "$SIMULATION_INSTRUCTIONS" "${CONFIG_LIST[@]}" <<'PY'
import csv
import json
import os
import re
import sys

out_dir, trace_path, warmup, sim, *configs = sys.argv[1:]
access_types = ("LOAD", "RFO", "PREFETCH", "WRITE", "TRANSLATION")
rows = []

for name in configs:
    json_path = os.path.join(out_dir, f"{name}.json")
    with open(json_path, encoding="utf-8") as f:
        phases = json.load(f)
    phase = next((p for p in phases if p.get("name") == "Simulation"), None)
    if phase is None:
        raise SystemExit(f"ERROR: Simulation phase missing from {json_path}")
    roi = phase["roi"]
    cores = roi["cores"]
    llc = roi["LLC"]
    instructions = sum(int(c["instructions"]) for c in cores)
    cycles = sum(int(c["cycles"]) for c in cores)

    hits = sum(sum(int(v) for v in llc[k]["hit"]) for k in access_types)
    misses = sum(sum(int(v) for v in llc[k]["miss"]) for k in access_types)
    accesses = hits + misses
    hybrid = llc.get("hybrid LLC", {})
    row = {
        "configuration": name,
        "trace": trace_path,
        "warmup_instructions": warmup,
        "simulation_instructions": sim,
        "instructions_measured": instructions,
        "cycles_measured": cycles,
        "IPC": instructions / cycles if cycles else 0,
        "LLC_accesses": accesses,
        "LLC_hits": hits,
        "LLC_misses": misses,
        "LLC_miss_rate": misses / accesses if accesses else 0,
        "average_miss_latency_cycles": llc.get("miss latency"),
        "SRAM_reads": hybrid.get("SRAM reads"),
        "SRAM_writes": hybrid.get("SRAM writes"),
        "STTRAM_reads": hybrid.get("STTRAM reads"),
        "STTRAM_writes": hybrid.get("STTRAM writes"),
        "PHC_placement_SRAM": "",
        "PHC_placement_STT": "",
        "PHC_trainings": "",
    }

    # hybrid_phc prints its own stats (cumulative, incl. warmup) to stdout
    with open(os.path.join(out_dir, f"{name}.log"), encoding="utf-8", errors="replace") as f:
        log = f.read()
    m = re.search(r"PHC placement SRAM: (\d+) STT-RAM: (\d+)", log)
    if m:
        row["PHC_placement_SRAM"], row["PHC_placement_STT"] = m.groups()
    m = re.search(r"PHC trainings: (\d+)", log)
    if m:
        row["PHC_trainings"] = m.group(1)
    rows.append(row)

csv_path = os.path.join(out_dir, "comparison.csv")
with open(csv_path, "w", newline="", encoding="utf-8") as f:
    writer = csv.DictWriter(f, fieldnames=rows[0].keys())
    writer.writeheader()
    writer.writerows(rows)

for r in rows:
    print(f"{r['configuration']:>16}: IPC {r['IPC']:.4f}  LLC miss rate {r['LLC_miss_rate']:.4f}  "
          f"SRAM r/w {r['SRAM_reads']}/{r['SRAM_writes']}  STT r/w {r['STTRAM_reads']}/{r['STTRAM_writes']}")
print(f"\nComparison CSV written to: {csv_path}")
PY

printf '\nDone. Logs, JSON results and CSV: %s\n' "$OUT_DIR"
