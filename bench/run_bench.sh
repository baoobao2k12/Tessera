#!/usr/bin/env sh
# Reproducible benchmark: fixed-depth search over the built-in bench positions
# (node count is a deterministic signature of the search; NPS measures speed),
# perft speed from the start position, and the tactical suite.
#
# usage: bench/run_bench.sh [path/to/tessera] [bench depth] [tactics movetime ms]
set -e
ENGINE=${1:-build/tessera}
DEPTH=${2:-13}
MOVETIME=${3:-1000}
DIR=$(dirname "$0")

echo "== search bench (depth $DEPTH, 1 thread, 16 MB)"
"$ENGINE" bench "$DEPTH" 1 16 2>/dev/null
echo "== perft 6 from the start position"
printf 'position startpos\ngo perft 6\nquit\n' | "$ENGINE" | tail -n 3
echo "== tactical suite ($MOVETIME ms per position)"
python3 "$DIR/tactics.py" "$ENGINE" "$DIR/tactics.epd" "$MOVETIME" | tail -n 1
