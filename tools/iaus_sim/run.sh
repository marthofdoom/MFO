#!/usr/bin/env bash
# Build and run the offline IAUS combat simulator with the host compiler (card I2s). Host-only, never in the DLL.
# Usage: tools/iaus_sim/run.sh [--list <json>] [--seed N] [--fights N] [--compare <baseline>] [--baseline <out>]
#   list JSONs come from tools/list_balance/extract.py (default dir ~/.cache/mfo-list-balance/, never committed).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/iaus_sim"
"${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -Wno-misleading-indentation -Wno-parentheses -I "$ROOT/native" \
    "$ROOT/native/iaus/Curves.cpp" "$ROOT/native/iaus/Catalogue.cpp" "$ROOT/native/iaus/Score.cpp" \
    "$ROOT/native/iaus/Decide.cpp" "$ROOT/native/iaus/Explain.cpp" "$ROOT/native/iaus/Generate.cpp" \
    "$ROOT/tools/iaus_sim/sim.cpp" -o "$OUT"
exec "$OUT" "$@"
