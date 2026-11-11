#!/usr/bin/env bash
# Build and run the IAUS core harness with the host compiler (no CommonLib, no engine). Card I2.
# Usage: tools/iaus_harness/run.sh            (from the repo root, or anywhere)
#        CXX=clang++ tools/iaus_harness/run.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/iaus_harness"
"${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion \
    -I "$ROOT/native" \
    "$ROOT/native/iaus/Curves.cpp" "$ROOT/native/iaus/Catalogue.cpp" "$ROOT/native/iaus/Score.cpp" \
    "$ROOT/native/iaus/Decide.cpp" "$ROOT/native/iaus/Explain.cpp" "$ROOT/native/iaus/Generate.cpp" \
    "$ROOT/tools/iaus_harness/harness.cpp" -o "$OUT"
exec "$OUT"
