#!/usr/bin/env bash
# IAUS card I1: build and run the rank-seam equivalence check with the host compiler (no CommonLib).
# Usage: tools/rank_equiv/run.sh     (from anywhere; CXX=clang++ to switch compilers)
# Exit code = number of failed checks.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/rank_equiv"
# Iaus::RankOf lives in iaus/Decide.cpp, which needs the rest of the pure core (the same list as tools/iaus_harness).
"${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion \
    -I "$ROOT/native" \
    "$ROOT/native/iaus/Curves.cpp" "$ROOT/native/iaus/Catalogue.cpp" "$ROOT/native/iaus/Score.cpp" \
    "$ROOT/native/iaus/Decide.cpp" "$ROOT/native/iaus/Explain.cpp" \
    "$ROOT/tools/rank_equiv/test.cpp" -o "$OUT"
exec "$OUT"
