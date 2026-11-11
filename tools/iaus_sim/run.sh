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
    # The class table is the game's own: native/classes/ClassTable.cpp compiled with host stand-ins for the engine headers
    # (stub/PCH.h, stub/State.h) and the opcode constants copied out of native/Vocabulary.h (gen_vocab.py), all FIRST on the include path.
    GEN="${OUT}.gen"; mkdir -p "$GEN"
    python3 -I "$ROOT/tools/iaus_sim/gen_vocab.py" "$ROOT/native/Vocabulary.h" "$GEN/Vocabulary.h"
    "${CXX:-g++}" -std=c++23 -O2 -pthread -Wall -Wextra -Wno-misleading-indentation -Wno-parentheses ${SIM_CXXFLAGS:-} \
        -I "$GEN" -I "$ROOT/tools/iaus_sim/stub" -I "$ROOT/native" -I "$ROOT/tools" \
        "$ROOT/native/iaus/Curves.cpp" "$ROOT/native/iaus/Catalogue.cpp" "$ROOT/native/iaus/Score.cpp" \
        "$ROOT/native/iaus/Decide.cpp" "$ROOT/native/iaus/Explain.cpp" "$ROOT/native/iaus/Generate.cpp" \
        "$ROOT/native/classes/ClassTable.cpp" "$ROOT/tools/iaus_sim/RealClasses.cpp" \
        "$ROOT/tools/iaus_sim/sim.cpp" -o "$OUT"
fi
# the results file (Results.h) records where the run came from
export SIM_BRANCH="$(git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || true)"
export SIM_SHA="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || true)"
exec "$OUT" "$@"
