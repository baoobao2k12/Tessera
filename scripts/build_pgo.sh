#!/usr/bin/env sh
# Profile-guided optimisation build (GCC or Clang).
#   1. build an instrumented binary, 2. run the bench to collect a profile,
#   3. rebuild using the profile. The result is build-pgo/tessera.
#
# usage: scripts/build_pgo.sh [extra cmake arguments, e.g. -DCMAKE_CXX_COMPILER=clang++]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PROFILE="$ROOT/build-pgo/profile"
rm -rf "$ROOT/build-pgo-gen" "$PROFILE"
mkdir -p "$PROFILE"

cmake -S "$ROOT" -B "$ROOT/build-pgo-gen" -DCMAKE_BUILD_TYPE=Release -DTESSERA_PGO=generate \
      -DTESSERA_PGO_DIR="$PROFILE" "$@"
cmake --build "$ROOT/build-pgo-gen" --target tessera -j
"$ROOT/build-pgo-gen/tessera" bench 11 1 16 > /dev/null 2>&1

# Clang writes raw profiles that must be merged
if ls "$PROFILE"/*.profraw > /dev/null 2>&1; then
    MERGE=$(command -v llvm-profdata || ls /usr/bin/llvm-profdata-* 2>/dev/null | head -n 1)
    "$MERGE" merge -output="$PROFILE/tessera.profdata" "$PROFILE"/*.profraw
fi

cmake -S "$ROOT" -B "$ROOT/build-pgo" -DCMAKE_BUILD_TYPE=Release -DTESSERA_PGO=use \
      -DTESSERA_PGO_DIR="$PROFILE" "$@"
cmake --build "$ROOT/build-pgo" --target tessera -j
echo "PGO build ready: $ROOT/build-pgo/tessera"
