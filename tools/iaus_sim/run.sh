#!/usr/bin/env bash
# Build and run the offline IAUS combat simulator v2 with the host compiler. Host-only, never in the DLL.
# Usage: tools/iaus_sim/run.sh [--list <sim.json>] [--seed N] [--fights N] [--threads N] [--compare <baseline>] [--baseline <out>]
#   <sim.json> comes from tools/list_balance (extract_world.py -> compile_world.py), default dir ~/.cache/mfo-list-balance/,
#   never committed. Without --list the built-in fixture set runs (no game data).
# SIM_NOBUILD=1 skips the rebuild; SIM_CXXFLAGS adds flags (e.g. -g -fsanitize=address,undefined).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${SIM_OUT:-${TMPDIR:-/tmp}/iaus_sim_v2}"
if [[ -z "${SIM_NOBUILD:-}" ]]; then
    "${CXX:-g++}" -std=c++23 -O2 -pthread -Wall -Wextra -Wno-misleading-indentation -Wno-parentheses ${SIM_CXXFLAGS:-} \
        -I "$ROOT/native" -I "$ROOT/tools" \
        "$ROOT/native/iaus/Curves.cpp" "$ROOT/native/iaus/Catalogue.cpp" "$ROOT/native/iaus/Score.cpp" \
        "$ROOT/native/iaus/Decide.cpp" "$ROOT/native/iaus/Explain.cpp" "$ROOT/native/iaus/Generate.cpp" \
        "$ROOT/tools/iaus_sim/sim.cpp" -o "$OUT"
fi
exec "$OUT" "$@"
