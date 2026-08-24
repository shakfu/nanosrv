#!/usr/bin/env bash
# Unified benchmark: comparison + busy sweep + HTML report generation.
# Usage: bash scripts/bench.sh
#        make bench

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
PORT=9876
DURATION=10s
THREADS=4
CONNECTIONS=100
URL="http://127.0.0.1:${PORT}/"

# Load generator. wrk is preferred when installed, but it is an external
# dependency that the published numbers silently assumed; scripts/loadgen.c is
# a bundled fallback so `make bench` works on a bare machine with only a C
# compiler. Both are driven at the same connection count and duration.
DURATION_SECS="${DURATION%s}"
LOADGEN_BIN="build/loadgen"

if command -v wrk >/dev/null 2>&1; then
    LOAD_TOOL="wrk"
else
    LOAD_TOOL="loadgen"
    if [ ! -x "$LOADGEN_BIN" ] || [ "${SCRIPT_DIR:-scripts}/loadgen.c" -nt "$LOADGEN_BIN" ]; then
        mkdir -p build
        echo "wrk not found -- building the bundled load generator ($LOADGEN_BIN)"
        "${CC:-cc}" -O2 -D_GNU_SOURCE -o "$LOADGEN_BIN" \
            "$(cd "$(dirname "$0")" && pwd)/loadgen.c" -lpthread
    fi
fi

WRK="wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION} --latency ${URL}"

# Format microseconds the way wrk does, so both tools produce the same table.
fmt_us() {
    awk -v us="$1" 'BEGIN {
        if (us >= 1000000) printf "%.2fs", us / 1000000
        else if (us >= 1000) printf "%.2fms", us / 1000
        else printf "%.2fus", us
    }'
}

BUILD_DIR=build/cmake
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Binary paths (all executables output to build/)
MONGOOSE_SERVER="./build/mongoose-server"
MUNGO_SERVER="./build/mungo-server"
NANOSRV_SERVER="./build/nanosrv-server"
NANOSRV_SHARDED="./build/nanosrv-sharded"
REPORT=build/bench-report.html

BUSY_VALUES=(0 10 50 100 500)

BOLD='\033[1m'
CYAN='\033[0;36m'
GREEN='\033[0;32m'
NC='\033[0m'

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
wait_for_server() {
    for _ in $(seq 1 30); do
        if curl -s -o /dev/null "http://127.0.0.1:${PORT}/" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "Server did not start in time" >&2
    return 1
}

kill_server() {
    if [ -n "${SERVER_PID:-}" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        unset SERVER_PID
    fi
    sleep 0.3
}

trap kill_server EXIT

# Run a benchmark, set globals: _RPS, _LAT, _P99
run_one() {
    local label="$1"; shift
    "$@" &
    SERVER_PID=$!
    sleep 0.5
    wait_for_server

    if [ "$LOAD_TOOL" = "wrk" ]; then
        OUTPUT=$(${WRK} 2>&1) || true
        _RPS=$(echo "$OUTPUT" | awk '/Requests\/sec/{print $2}')
        _LAT=$(echo "$OUTPUT" | awk '/Thread Stats/{found=1} found && /Latency/{print $2; exit}')
        _P99=$(echo "$OUTPUT" | awk '/99%/{print $2}')
    else
        OUTPUT=$("$LOADGEN_BIN" 127.0.0.1 "$PORT" "$CONNECTIONS" "$DURATION_SECS" 2>&1) || true
        _RPS=$(echo "$OUTPUT" | sed -n 's/.*"rps": \([0-9.]*\).*/\1/p')
        _LAT=$(fmt_us "$(echo "$OUTPUT" | sed -n 's/.*"mean_latency_us": \([0-9.]*\).*/\1/p')")
        _P99=$(fmt_us "$(echo "$OUTPUT" | sed -n 's/.*"p99_latency_us": \([0-9.]*\).*/\1/p')")
    fi

    kill_server
}

header() {
    echo ""
    echo -e "${BOLD}${CYAN}=== $1 ===${NC}"
    echo ""
}

# ---------------------------------------------------------------------------
# Phase 1: Comparison benchmark (all servers, trivial handler)
# ---------------------------------------------------------------------------
declare -a COMP_NAMES COMP_RPS COMP_LAT COMP_P99

echo -e "${BOLD}Benchmark Configuration${NC}"
echo "  Duration:    ${DURATION}"
echo "  Threads:     ${THREADS}"
echo "  Connections: ${CONNECTIONS}"
echo "  Port:        ${PORT}"
echo "  Load tool:   ${LOAD_TOOL}"
echo "  Handler:     identical across all servers (formats method + URI)"

run_comparison() {
    local name="$1"; shift
    header "$name"
    echo -n "  Running wrk ... "
    run_one "$name" "$@"
    echo "${_RPS} req/s (avg ${_LAT}, p99 ${_P99})"
    COMP_NAMES+=("$name")
    COMP_RPS+=("$_RPS")
    COMP_LAT+=("$_LAT")
    COMP_P99+=("$_P99")
}

run_comparison "mongoose 7.21 (C, single-thread)" \
    ${MONGOOSE_SERVER} --port "$PORT"

run_comparison "mungo-server (C, single-thread)" \
    ${MUNGO_SERVER} --port "$PORT"

run_comparison "nanosrv-server (C++, single-thread)" \
    ${NANOSRV_SERVER} --port "$PORT"

run_comparison "nanosrv-sharded (C++, multi-thread)" \
    ${NANOSRV_SHARDED} --port "$PORT"

run_comparison "nanosrv Python Manager (single-thread)" \
    uv run python "${SCRIPT_DIR}/bench_pynanosrv_server.py" "$PORT"

run_comparison "nanosrv Python ShardedManager (multi-thread)" \
    uv run python "${SCRIPT_DIR}/bench_pynanosrv_sharded.py" "$PORT"

# Optional 7th row: the same sharded Python server on a free-threaded
# interpreter, where the Python handler parallelises instead of contending for
# the GIL. Set NANOSRV_FT_PYTHON to a 3.13t+ interpreter that has nanosrv
# installed (see scripts/bench_freethreading.py for a fuller sweep).
FT_PYTHON="${NANOSRV_FT_PYTHON:-$(command -v python3.13t || true)}"
if [ -n "$FT_PYTHON" ] && "$FT_PYTHON" -c "import nanosrv" >/dev/null 2>&1; then
    run_comparison "nanosrv Python ShardedManager (free-threaded)" \
        "$FT_PYTHON" "${SCRIPT_DIR}/bench_pynanosrv_sharded.py" "$PORT"
fi

# Terminal summary
header "COMPARISON SUMMARY"
printf "${BOLD}%-48s | %-12s | %-10s | %-10s${NC}\n" "Server" "Req/sec" "Avg Lat" "p99 Lat"
echo "-------------------------------------------------+--------------+------------+-----------"
for i in "${!COMP_NAMES[@]}"; do
    printf "%-48s | %-12s | %-10s | %-10s\n" \
        "${COMP_NAMES[$i]}" "${COMP_RPS[$i]}" "${COMP_LAT[$i]}" "${COMP_P99[$i]}"
done

# ---------------------------------------------------------------------------
# Phase 2: Busy sweep (nanosrv-server vs nanosrv-sharded)
# ---------------------------------------------------------------------------
declare -a BUSY_SINGLE_RPS BUSY_SINGLE_LAT BUSY_SHARDED_RPS BUSY_SHARDED_LAT BUSY_SPEEDUP

header "SHARDING BUSY SWEEP"

for busy in "${BUSY_VALUES[@]}"; do
    echo -e "${CYAN}--- --busy ${busy}us ---${NC}"

    echo -n "  nanosrv-server ...  "
    run_one "single" ${NANOSRV_SERVER} --port "$PORT" --busy "$busy"
    BUSY_SINGLE_RPS+=("$_RPS")
    BUSY_SINGLE_LAT+=("$_LAT")
    echo "${_RPS} req/s (${_LAT})"

    echo -n "  nanosrv-sharded ... "
    run_one "sharded" ${NANOSRV_SHARDED} --port "$PORT" --busy "$busy"
    BUSY_SHARDED_RPS+=("$_RPS")
    BUSY_SHARDED_LAT+=("$_LAT")
    echo "${_RPS} req/s (${_LAT})"

    _idx=$(( ${#BUSY_SINGLE_RPS[@]} - 1 ))
    SPEEDUP=$(awk "BEGIN {printf \"%.2f\", ${BUSY_SHARDED_RPS[$_idx]} / ${BUSY_SINGLE_RPS[$_idx]}}")
    BUSY_SPEEDUP+=("${SPEEDUP}x")
    echo ""
done

# Terminal summary
header "BUSY SWEEP SUMMARY"
printf "${BOLD}%-10s | %-13s | %-11s | %-14s | %-12s | %-8s${NC}\n" \
    "Busy (us)" "Single req/s" "Single lat" "Sharded req/s" "Sharded lat" "Speedup"
echo "-----------+---------------+-------------+----------------+--------------+---------"
for i in "${!BUSY_VALUES[@]}"; do
    printf "%-10s | %-13s | %-11s | %-14s | %-12s | %-8s\n" \
        "${BUSY_VALUES[$i]}" "${BUSY_SINGLE_RPS[$i]}" "${BUSY_SINGLE_LAT[$i]}" \
        "${BUSY_SHARDED_RPS[$i]}" "${BUSY_SHARDED_LAT[$i]}" "${BUSY_SPEEDUP[$i]}"
done

# ---------------------------------------------------------------------------
# Phase 3: Generate HTML report
# ---------------------------------------------------------------------------
header "GENERATING REPORT"

# Compute max RPS for chart scaling
MAX_COMP_RPS=0
for rps in "${COMP_RPS[@]}"; do
    MAX_COMP_RPS=$(awk "BEGIN {print ($rps > $MAX_COMP_RPS) ? $rps : $MAX_COMP_RPS}")
done

MAX_BUSY_RPS=0
for rps in "${BUSY_SINGLE_RPS[@]}" "${BUSY_SHARDED_RPS[@]}"; do
    MAX_BUSY_RPS=$(awk "BEGIN {print ($rps > $MAX_BUSY_RPS) ? $rps : $MAX_BUSY_RPS}")
done

CHART_W=480
BAR_H=32
GAP=6
NCOMP=${#COMP_NAMES[@]}
LABEL_H=16
COMP_SVG_H=$(( NCOMP * (LABEL_H + BAR_H + GAP) + 10 ))

NBUSY=${#BUSY_VALUES[@]}
GROUP_H=$(( 2 * BAR_H + GAP ))
BUSY_SVG_H=$(( NBUSY * (GROUP_H + GAP * 3) + 30 ))

SYSTEM_INFO=$(uname -smr)
BENCH_DATE=$(date "+%Y-%m-%d %H:%M")
NCPU=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo "?")

# Colors
C_BAR="#3b82f6"
C_SINGLE="#3b82f6"
C_SHARDED="#22c55e"

cat > "$REPORT" << 'HTMLHEAD'
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>nanosrv Benchmark Report</title>
<style>
:root {
    --bg: #ffffff; --fg: #1a1a1a; --bg2: #f8f9fa; --border: #dee2e6;
    --accent: #3b82f6; --green: #22c55e; --muted: #6b7280;
}
@media (prefers-color-scheme: dark) {
    :root {
        --bg: #111827; --fg: #f3f4f6; --bg2: #1f2937; --border: #374151;
        --accent: #60a5fa; --green: #4ade80; --muted: #9ca3af;
    }
}
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    background: var(--bg); color: var(--fg);
    max-width: 900px; margin: 0 auto; padding: 2rem 1.5rem;
    line-height: 1.5;
}
h1 { font-size: 1.75rem; margin-bottom: 0.25rem; }
h2 { font-size: 1.25rem; margin: 2.5rem 0 1rem; border-bottom: 2px solid var(--border); padding-bottom: 0.5rem; }
.meta { color: var(--muted); font-size: 0.85rem; margin-bottom: 2rem; }
.meta span { margin-right: 1.5rem; }
table { width: 100%; border-collapse: collapse; margin: 1rem 0; font-size: 0.9rem; }
th, td { text-align: left; padding: 0.5rem 0.75rem; border-bottom: 1px solid var(--border); }
th { background: var(--bg2); font-weight: 600; }
tr:hover { background: var(--bg2); }
td.num { text-align: right; font-variant-numeric: tabular-nums; font-family: "SF Mono", Menlo, monospace; }
svg { display: block; margin: 1rem 0 1.5rem; }
.bar-label { font-size: 12px; fill: var(--fg); font-family: -apple-system, sans-serif; }
.bar-value { font-size: 11px; fill: var(--muted); font-family: "SF Mono", Menlo, monospace; }
.legend { display: flex; gap: 1.5rem; font-size: 0.85rem; margin: 0.5rem 0 1rem; }
.legend-item { display: flex; align-items: center; gap: 0.4rem; }
.legend-swatch { width: 14px; height: 14px; border-radius: 3px; }
.speedup { font-weight: 600; }
.speedup.fast { color: var(--green); }
.speedup.slow { color: var(--muted); }
footer { margin-top: 3rem; padding-top: 1rem; border-top: 1px solid var(--border); font-size: 0.8rem; color: var(--muted); }
</style>
</head>
<body>
<h1>nanosrv Benchmark Report</h1>
HTMLHEAD

# Meta line
cat >> "$REPORT" << EOF
<div class="meta">
<span>${BENCH_DATE}</span>
<span>${SYSTEM_INFO}</span>
<span>${NCPU} CPUs</span>
<span>wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION}</span>
</div>
EOF

# --- Section 1: Comparison ---
cat >> "$REPORT" << 'EOF'
<h2>Server Comparison (trivial handler)</h2>
EOF

# SVG bar chart (labels above bars)
{
echo "<svg width=\"700\" height=\"${COMP_SVG_H}\" xmlns=\"http://www.w3.org/2000/svg\">"
Y=2
for i in "${!COMP_NAMES[@]}"; do
    BAR_W=$(awk "BEGIN {printf \"%d\", (${COMP_RPS[$i]} / $MAX_COMP_RPS) * $CHART_W}")
    echo "  <text x=\"0\" y=\"$((Y + LABEL_H - 2))\" class=\"bar-label\">${COMP_NAMES[$i]}</text>"
    Y=$((Y + LABEL_H))
    echo "  <rect x=\"0\" y=\"$Y\" width=\"$BAR_W\" height=\"$BAR_H\" rx=\"4\" fill=\"${C_BAR}\" opacity=\"0.85\"/>"
    echo "  <text x=\"$((BAR_W + 8))\" y=\"$((Y + BAR_H / 2 + 4))\" class=\"bar-value\">${COMP_RPS[$i]} req/s</text>"
    Y=$((Y + BAR_H + GAP))
done
echo "</svg>"
} >> "$REPORT"

# Table
{
echo "<table>"
echo "<tr><th>Server</th><th>Req/sec</th><th>Avg Latency</th><th>p99 Latency</th></tr>"
for i in "${!COMP_NAMES[@]}"; do
    echo "<tr><td>${COMP_NAMES[$i]}</td><td class=\"num\">${COMP_RPS[$i]}</td><td class=\"num\">${COMP_LAT[$i]}</td><td class=\"num\">${COMP_P99[$i]}</td></tr>"
done
echo "</table>"
} >> "$REPORT"

# --- Section 2: Busy sweep ---
cat >> "$REPORT" << 'EOF'
<h2>Sharding Speedup (busy handler)</h2>
<p style="color: var(--muted); font-size: 0.9rem; margin-bottom: 0.5rem;">
Each handler spins for the given number of microseconds before replying, simulating CPU-bound work.
nanosrv-sharded uses 8 worker threads with accept-and-hand-off.
</p>
<div class="legend">
<div class="legend-item"><div class="legend-swatch" style="background:var(--accent)"></div> Single-threaded</div>
<div class="legend-item"><div class="legend-swatch" style="background:var(--green)"></div> Sharded (8 workers)</div>
</div>
EOF

# SVG grouped bar chart
{
echo "<svg width=\"700\" height=\"${BUSY_SVG_H}\" xmlns=\"http://www.w3.org/2000/svg\">"
Y=5
LABEL_W=80
for i in "${!BUSY_VALUES[@]}"; do
    LABEL="${BUSY_VALUES[$i]}us"
    S_W=$(awk "BEGIN {v=${BUSY_SINGLE_RPS[$i]}/$MAX_BUSY_RPS*$CHART_W; printf \"%d\", (v<1&&v>0)?1:v}")
    H_W=$(awk "BEGIN {v=${BUSY_SHARDED_RPS[$i]}/$MAX_BUSY_RPS*$CHART_W; printf \"%d\", (v<1&&v>0)?1:v}")

    echo "  <text x=\"0\" y=\"$((Y + BAR_H + GAP / 2 + 2))\" class=\"bar-label\">${LABEL}</text>"

    echo "  <rect x=\"$LABEL_W\" y=\"$Y\" width=\"$S_W\" height=\"$BAR_H\" rx=\"4\" fill=\"${C_SINGLE}\" opacity=\"0.85\"/>"
    echo "  <text x=\"$((LABEL_W + S_W + 8))\" y=\"$((Y + BAR_H / 2 + 4))\" class=\"bar-value\">${BUSY_SINGLE_RPS[$i]}</text>"

    Y2=$((Y + BAR_H + GAP))
    echo "  <rect x=\"$LABEL_W\" y=\"$Y2\" width=\"$H_W\" height=\"$BAR_H\" rx=\"4\" fill=\"${C_SHARDED}\" opacity=\"0.85\"/>"
    echo "  <text x=\"$((LABEL_W + H_W + 8))\" y=\"$((Y2 + BAR_H / 2 + 4))\" class=\"bar-value\">${BUSY_SHARDED_RPS[$i]}</text>"

    Y=$((Y2 + BAR_H + GAP * 3))
done
echo "</svg>"
} >> "$REPORT"

# Table
{
echo "<table>"
echo "<tr><th>Busy (us)</th><th>Single req/s</th><th>Single lat</th><th>Sharded req/s</th><th>Sharded lat</th><th>Speedup</th></tr>"
for i in "${!BUSY_VALUES[@]}"; do
    SP_NUM=$(echo "${BUSY_SPEEDUP[$i]}" | sed 's/x//')
    SP_CLASS=$(awk "BEGIN {print ($SP_NUM > 1.1) ? \"fast\" : \"slow\"}")
    echo "<tr><td class=\"num\">${BUSY_VALUES[$i]}</td><td class=\"num\">${BUSY_SINGLE_RPS[$i]}</td><td class=\"num\">${BUSY_SINGLE_LAT[$i]}</td><td class=\"num\">${BUSY_SHARDED_RPS[$i]}</td><td class=\"num\">${BUSY_SHARDED_LAT[$i]}</td><td class=\"num speedup ${SP_CLASS}\">${BUSY_SPEEDUP[$i]}</td></tr>"
done
echo "</table>"
} >> "$REPORT"

# Footer
cat >> "$REPORT" << 'EOF'
<footer>Generated by <code>make bench</code> / <code>scripts/bench.sh</code></footer>
</body>
</html>
EOF

echo "Report: ${REPORT}"
echo ""

# Open on macOS
if command -v open &>/dev/null; then
    open "$REPORT"
fi
