#!/usr/bin/env bash
# Build and run the FLWR mfoEnabled-byte round-trip test with the host compiler (no CommonLib, no engine). Card I5.
# Usage: tools/flwr_byte_test/run.sh            (from the repo root, or anywhere)
#        CXX=clang++ tools/flwr_byte_test/run.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/flwr_byte_test"
"${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion -Wsign-compare \
    -I "$ROOT/native" "$ROOT/tools/flwr_byte_test/test.cpp" -o "$OUT"
exec "$OUT"
