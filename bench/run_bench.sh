#!/usr/bin/env bash
# bench/run_bench.sh — GSSK benchmark runner
# Usage: ./bench/run_bench.sh [--regression <budget_ms>]
#   --regression <budget_ms>  exit 1 if the slowest scenario exceeds budget_ms

set -euo pipefail

GSSK_BIN="${GSSK_BIN:-./bin/gssk}"
BENCH_DIR="$(dirname "$0")"
REGRESSION=0
BUDGET_MS=500

while [[ $# -gt 0 ]]; do
  case "$1" in
    --regression) REGRESSION=1; BUDGET_MS="${2:-500}"; shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

if [[ ! -x "$GSSK_BIN" ]]; then
  echo "ERROR: $GSSK_BIN not found or not executable. Run 'make' first." >&2
  exit 1
fi

# Scenarios: label  model_path
declare -a LABELS MODELS
declare -i IDX=0

add_scenario() {
  LABELS[$IDX]="$1"
  MODELS[$IDX]="$2"
  IDX+=1
}

# Built-in examples
add_scenario "decay_model      " "examples/decay_model.json"
add_scenario "oscillator_model " "examples/oscillator_model.json"
add_scenario "supply_chain_30  " "examples/supply_chain_30.json"
add_scenario "household_model  " "examples/household_model.json"

# Generated bench models (built by make bench-gen)
for f in "$BENCH_DIR"/bench_*.json; do
  [[ -f "$f" ]] || continue
  name=$(basename "$f" .json)
  add_scenario "$(printf '%-17s' "$name")" "$f"
done

# Temporary output file
TMP=$(mktemp /tmp/gssk_bench_XXXXXX.csv)
trap 'rm -f "$TMP"' EXIT

printf "%-20s  %8s  %8s\n" "Scenario" "Steps" "ms"
printf '%0.s-' {1..42}; echo

WORST_MS=0
WORST_LABEL=""

for i in "${!LABELS[@]}"; do
  label="${LABELS[$i]}"
  model="${MODELS[$i]}"

  if [[ ! -f "$model" ]]; then
    printf "%-20s  %8s  %8s\n" "$label" "SKIP" "(not found)"
    continue
  fi

  # Time the run
  START_NS=$(date +%s%N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1e9))')
  "$GSSK_BIN" "$model" "$TMP" > /dev/null 2>&1
  END_NS=$(date +%s%N 2>/dev/null || python3 -c 'import time; print(int(time.time()*1e9))')

  ELAPSED_MS=$(( (END_NS - START_NS) / 1000000 ))
  STEPS=$(( $(wc -l < "$TMP") - 1 ))   # subtract header row

  printf "%-20s  %8d  %8d\n" "$label" "$STEPS" "$ELAPSED_MS"

  if (( ELAPSED_MS > WORST_MS )); then
    WORST_MS=$ELAPSED_MS
    WORST_LABEL="$label"
  fi
done

echo
echo "Slowest: ${WORST_LABEL} — ${WORST_MS} ms"

if (( REGRESSION == 1 )); then
  if (( WORST_MS > BUDGET_MS )); then
    echo "FAIL: ${WORST_MS} ms exceeds budget ${BUDGET_MS} ms" >&2
    exit 1
  else
    echo "OK: within budget (${BUDGET_MS} ms)"
  fi
fi
