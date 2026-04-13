#!/usr/bin/env bash
# run_perf_profile.sh — Profile bench_e2e using perf stat and perf record.
#
# Required env: labnew or any Linux env with perf installed.
# No root required for perf stat; perf record requires perf_event_paranoid <= 1.
#
# Usage (positional):
#   bash run_perf_profile.sh <BENCH_BIN> <BENCH_ARGS> [OUT_DIR]
#
# Usage (env vars):
#   BENCH_BIN=./bench_e2e \
#   BENCH_ARGS="--index-dir /path/to/idx --nprobe 200 --queries 1000" \
#   OUT_DIR=/tmp/perf_out \
#   bash run_perf_profile.sh

set -euo pipefail

# ---------------------------------------------------------------------------
# Parse arguments (positional or env vars)
# ---------------------------------------------------------------------------

BENCH_BIN="${1:-${BENCH_BIN:-}}"
BENCH_ARGS="${2:-${BENCH_ARGS:-}}"
OUT_DIR="${3:-${OUT_DIR:-/tmp/perf_profile_$(date +%Y%m%dT%H%M%S)}}"

# ---------------------------------------------------------------------------
# Validate
# ---------------------------------------------------------------------------

if [[ -z "$BENCH_BIN" ]]; then
    echo "Usage: $0 <BENCH_BIN> <BENCH_ARGS> [OUT_DIR]"
    echo "  or set BENCH_BIN / BENCH_ARGS / OUT_DIR env vars."
    exit 1
fi

if [[ ! -x "$BENCH_BIN" ]]; then
    echo "Error: BENCH_BIN='$BENCH_BIN' does not exist or is not executable."
    exit 1
fi

mkdir -p "$OUT_DIR"
echo "=== perf profiling: $BENCH_BIN ==="
echo "  BENCH_ARGS : $BENCH_ARGS"
echo "  OUT_DIR    : $OUT_DIR"
echo ""

# ---------------------------------------------------------------------------
# Step 1: perf stat — hardware counters (no special permissions required)
# ---------------------------------------------------------------------------

PERF_STAT_OUT="$OUT_DIR/perf_stat.txt"

if command -v perf &>/dev/null; then
    echo "[1/3] Running perf stat ..."
    PERF_EVENTS="cycles,instructions,cache-misses,cache-references,branch-misses,LLC-load-misses,LLC-store-misses"
    # shellcheck disable=SC2086
    perf stat -e "$PERF_EVENTS" \
        -o "$PERF_STAT_OUT" \
        -- "$BENCH_BIN" $BENCH_ARGS 2>&1
    echo "  -> $PERF_STAT_OUT"
else
    echo "[1/3] WARNING: 'perf' not found in PATH. Skipping perf stat."
    PERF_STAT_OUT=""
fi

# ---------------------------------------------------------------------------
# Step 2: perf record — call graph (requires perf_event_paranoid <= 1)
# ---------------------------------------------------------------------------

PERF_DATA="$OUT_DIR/perf.data"
PERF_REPORT_OUT="$OUT_DIR/perf_report.txt"
PERF_RECORD_DONE=0

if command -v perf &>/dev/null; then
    PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "99")
    if [[ "$PARANOID" -le 1 ]]; then
        echo "[2/3] Running perf record (paranoid=$PARANOID) ..."
        # shellcheck disable=SC2086
        perf record -g --call-graph dwarf \
            -o "$PERF_DATA" \
            -- "$BENCH_BIN" $BENCH_ARGS 2>&1
        echo "  -> $PERF_DATA"

        echo "  Generating perf report ..."
        perf report --stdio -i "$PERF_DATA" > "$PERF_REPORT_OUT" 2>&1
        echo "  -> $PERF_REPORT_OUT"
        PERF_RECORD_DONE=1
    else
        echo "[2/3] NOTE: perf_event_paranoid=$PARANOID > 1."
        echo "      perf record requires paranoid <= 1 or CAP_PERFMON."
        echo "      To enable: sudo sysctl kernel.perf_event_paranoid=1"
        echo "      Skipping perf record."
    fi
else
    echo "[2/3] Skipping perf record (perf not found)."
fi

# ---------------------------------------------------------------------------
# Step 3: FlameGraph (optional, requires flamegraph.pl in PATH or ../FlameGraph/)
# ---------------------------------------------------------------------------

FLAMEGRAPH_SVG="$OUT_DIR/flamegraph.svg"
FLAMEGRAPH_DONE=0

if [[ "$PERF_RECORD_DONE" -eq 1 ]]; then
    # Search for flamegraph.pl
    FG_PL=""
    if command -v flamegraph.pl &>/dev/null; then
        FG_PL="flamegraph.pl"
    elif command -v flamegraph &>/dev/null; then
        FG_PL="flamegraph"
    fi

    SC_PL=""
    if command -v stackcollapse-perf.pl &>/dev/null; then
        SC_PL="stackcollapse-perf.pl"
    fi

    # Also check relative FlameGraph directory
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    for candidate in "$SCRIPT_DIR/../../FlameGraph" "$SCRIPT_DIR/../FlameGraph" "$HOME/FlameGraph"; do
        if [[ -f "$candidate/flamegraph.pl" && -z "$FG_PL" ]]; then
            FG_PL="$candidate/flamegraph.pl"
            SC_PL="$candidate/stackcollapse-perf.pl"
            break
        fi
    done

    if [[ -n "$FG_PL" && -n "$SC_PL" ]]; then
        echo "[3/3] Generating FlameGraph ..."
        perf script -i "$PERF_DATA" | "$SC_PL" | "$FG_PL" > "$FLAMEGRAPH_SVG" 2>&1
        echo "  -> $FLAMEGRAPH_SVG"
        FLAMEGRAPH_DONE=1
    else
        echo "[3/3] FlameGraph tools not found. To install:"
        echo "      git clone https://github.com/brendangregg/FlameGraph ~/FlameGraph"
        echo "      export PATH=\$PATH:~/FlameGraph"
    fi
else
    echo "[3/3] Skipping FlameGraph (no perf.data)."
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo ""
echo "=== Profiling Complete ==="
echo ""
echo "Generated files:"
[[ -n "$PERF_STAT_OUT" && -f "$PERF_STAT_OUT" ]] && echo "  perf stat    : $PERF_STAT_OUT"
[[ "$PERF_RECORD_DONE" -eq 1 ]] && echo "  perf data    : $PERF_DATA"
[[ "$PERF_RECORD_DONE" -eq 1 ]] && echo "  perf report  : $PERF_REPORT_OUT"
[[ "$FLAMEGRAPH_DONE"  -eq 1 ]] && echo "  flamegraph   : $FLAMEGRAPH_SVG"
echo ""
echo "Next steps:"
if [[ -n "$PERF_STAT_OUT" && -f "$PERF_STAT_OUT" ]]; then
    echo "  View hardware counters : less $PERF_STAT_OUT"
    echo "  Key metrics to watch   : IPC (instructions/cycles), cache-miss rate, LLC-load-misses"
fi
if [[ "$PERF_RECORD_DONE" -eq 1 ]]; then
    echo "  View hotspot functions : less $PERF_REPORT_OUT"
    echo "  Interactive perf TUI   : perf report -i $PERF_DATA"
fi
if [[ "$FLAMEGRAPH_DONE" -eq 1 ]]; then
    echo "  Open flamegraph        : xdg-open $FLAMEGRAPH_SVG  (or open in browser)"
fi
echo ""
echo "Interpretation hints:"
echo "  - High cache-misses in ProbeCluster → data layout optimization (cluster block stride)"
echo "  - High IPC but slow → memory bound, check LLC-load-misses"
echo "  - Low IPC (<1.0) → branch-heavy or dependency stalls in FastScan loop"
