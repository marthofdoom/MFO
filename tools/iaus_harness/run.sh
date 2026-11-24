#!/usr/bin/env bash
# Build and run the IAUS core harness with the host compiler (no CommonLib, no engine). Card I2.
# Usage: tools/iaus_harness/run.sh            (from the repo root, or anywhere)
#        CXX=clang++ tools/iaus_harness/run.sh
# The movement rows alone (MSVC replays the same golden file; --write rewrites it after a deliberate change of a t.spot* / spot curve / generator):
#        g++ -std=c++23 -O2 -DMOVE_STANDALONE -I native native/iaus/{Curves,Catalogue,ClassCurves,Score,Decide,EnvQuery,PathSearch,Spots,MovePass}.cpp \
#            tools/iaus_harness/move_rows.cpp -o move_rows && ./move_rows tools/iaus_harness/golden_move.txt [--write]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/iaus_harness"
"${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion \
    -I "$ROOT/native" \
    "$ROOT/native/iaus/Curves.cpp" "$ROOT/native/iaus/Catalogue.cpp" "$ROOT/native/iaus/ClassCurves.cpp" "$ROOT/native/iaus/Score.cpp" \
    "$ROOT/native/iaus/Decide.cpp" "$ROOT/native/iaus/Explain.cpp" "$ROOT/native/iaus/Generate.cpp" \
    "$ROOT/native/iaus/EnvQuery.cpp" "$ROOT/native/iaus/PathSearch.cpp" "$ROOT/native/iaus/Spots.cpp" "$ROOT/native/iaus/MovePass.cpp" \
    -DMOVE_GOLDEN="\"$ROOT/tools/iaus_harness/golden_move.txt\"" "$ROOT/tools/iaus_harness/move_rows.cpp" \
    "$ROOT/tools/iaus_harness/harness.cpp" -o "$OUT"
exec "$OUT"
