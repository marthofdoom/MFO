#!/usr/bin/env bash
# Build and run the offline IAUS combat simulator v2 with the host compiler. Host-only, never in the DLL.
# Usage: tools/iaus_sim/run.sh [--list <sim.json>] [--seed N] [--fights N] [--threads N] [--compare <baseline>] [--baseline <out>]
#   <sim.json> comes from tools/list_balance (extract_world.py -> compile_world.py), default dir ~/.cache/mfo-list-balance/,
#   never committed. Without --list the built-in fixture set runs (no game data).
# SIM_CLITEST=1 runs the command-line assertions (backlog MFO-B710..B714) instead of a run and prints CLI-CHECKS PASS|FAIL.
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
if [[ -n "${SIM_CLITEST:-}" ]]; then
    fail=0; chk() { if ! eval "$2"; then echo "CLI-CHECK FAILED: $1"; fail=1; fi; }
    R() { "$OUT" --no-json --threads 8 "$@" 2>&1 || true; }
    chk "B710 --fights is honoured and printed" '[[ "$(R --seeds 1,2 --fights 6 --classes mage | tail -1)" == *"fights=6 per scenario"* ]]'
    chk "B710 default is 24 and printed"        '[[ "$(R --seeds 1 --classes mage | tail -1)" == *"fights=24 per scenario"* ]]'
    chk "B711 unknown --classes slug is an error" '! R --classes mage,mgae --seeds 1 | grep -q HEADLINE && R --classes mgae --seeds 1 | grep -q "unknown class"'
    chk "B711 unknown --opt-class is an error"  'R --opt-class mgae --opt x | grep -q "unknown class"'
    chk "B712 JSON headline records its scope"  'J="$(mktemp)"; "$OUT" --classes mage --threads 8 --json "$J" >/dev/null 2>&1; grep -q "\"covers\":\"mage\",\"fights\":24" "$J"; rc=$?; rm -f "$J"; [[ $rc == 0 ]]'
    chk "B713 --opt-prefix honours --seeds"     '[[ "$(R --opt-class mage --opt-prefix --opt-fights 2 --seeds 1,2 | sed -n 2p)" != "$(R --opt-class mage --opt-prefix --opt-fights 2 --seed 1 | sed -n 2p)" ]]'
    chk "B714 --seeds n=1 prints se=n/a"        '[[ "$(R --seeds 1 --classes mage | tail -1)" == *"se=n/a"* ]]'
    chk "B714 --seeds , is an error"            '! R --seeds , | grep -q HEADLINE'
    if [[ $fail == 0 ]]; then echo "CLI-CHECKS PASS"; exit 0; else echo "CLI-CHECKS FAIL"; exit 1; fi
fi
exec "$OUT" "$@"
