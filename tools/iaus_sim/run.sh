#!/usr/bin/env bash
# Build and run the offline IAUS combat simulator v2 with the host compiler. Host-only, never in the DLL.
# Usage: tools/iaus_sim/run.sh [--list <sim.json>] [--seed N] [--fights N] [--threads N] [--compare <baseline>] [--baseline <out>]
#        [--stealth off|g1|hide|g3] (default g3: every stealth / disengage road of T2 modelled) [--sneak-profile vanilla|steep|list]
#        T2 batch S (iaus-sim-v2.md section 31), every road ON by default:
#        [--t2 off] (every road off + the feet stop = the lab of section 30: 78.11 at g3, 76.69 at --stealth off for --classes beta1)
#        [--feet-stop] [--c8|--dodge|--aversion|--aim|--block|--range|--backoff|--step|--unweighted|--reanimate off|on] [--c13 built|lever]
#        [--disengage-dnd] [--mix-partial] [--aim-miss P] [--aim-scatter P] [--dodge-flight S] [--no-fill-swing] [--teach <token>]
#        Sim batch (iaus-sim-v2.md section 32): [--classes beta1 (12 classes + the 3 tree-OFF base cells) | beta1-tree (the 12)]
#        [--rating N] (fights per cell of the rating battery, default 3; 0 = off) [--rating-only] (the Rating (Tx) table over --seeds, nothing else)
#        [--class-oracle N] [--gap-scenarios N] (the per-class oracle gap; a run that writes a results file runs 4 + 2 by itself, 0 = off)
#        [--decline N] (health-decline curves and the best drink line per class) [--curve <class>:<family>.<cons>.<param>=<v>]
#        [--disengage-report N] (card G3's usefulness: the same fights without the run, with it, and with the D&D variant)
#        [--steal-lab N] (cards G13 / G14: the witness pre-check against the engine's detection formula)
#   <sim.json> comes from tools/list_balance (extract_world.py -> compile_world.py), default dir ~/.cache/mfo-list-balance/,
#   never committed. Without --list the built-in fixture set runs (no game data).
# SIM_CLITEST=1 runs the command-line assertions (backlog MFO-B710..B714, B755..B757) instead of a run and prints CLI-CHECKS PASS|FAIL.
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
        "$ROOT/native/iaus/Curves.cpp" "$ROOT/native/iaus/Catalogue.cpp" "$ROOT/native/iaus/ClassCurves.cpp" "$ROOT/native/iaus/Score.cpp" \
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
    chk "B755 a full run's headline is 24 fights whatever --fights says" 'J="$(mktemp)"; "$OUT" --classes mage --fights 1 --threads 8 --json "$J" 2>&1 | grep -q "^HEADLINE  J=.*fights=24 per scenario"; rc=$?; rm -f "$J"; [[ $rc == 0 ]]'
    chk "B755 --compare refuses a baseline made at another cell size" 'B="$(mktemp)"; R --classes mage --fights 1 --baseline "$B" >/dev/null; X="$(R --classes mage --fights 2 --compare "$B")"; Y="$(R --classes mage --fights 1 --compare "$B")"; rm -f "$B"; [[ "$X" == *"COMPARE REFUSED"* && "$Y" == *"0 metric(s) changed"* ]]'
    chk "B756 the oracle headline records its scope" 'J="$(mktemp)"; "$OUT" --classes mage --fights 1 --threads 8 --headline-oracle 1 --oracle-k 1 --oracle-h 5 --json "$J" >/dev/null 2>&1; grep -q "\"oracle_covers\":\"mage\"" "$J"; rc=$?; rm -f "$J"; [[ $rc == 0 ]]'
    chk "stealth off is the lab before the stealth model (thief, seed 1)" '[[ "$(R --t2 off --stealth off --classes thief --seeds 1 | head -1)" == "SEED 1  J=42.47 (scenarios 15.35, natural 69.58)" ]]'
    chk "T2 batch S: --t2 off is the lab of section 30 at g3 (beta1, seed 1)" '[[ "$(R --t2 off --classes beta1-tree --seeds 1 | head -1)" == "SEED 1  J=76.00 (scenarios 79.27, natural 72.73)" ]]'   # 76.96 in section 30; 76.29 with the School Circle (shipped 2026-11-12, not a T2 road); 76.00 with the Mercenary line in light armor (its card; the kit is not a T2 road either)
    chk "T2 batch S: every road off by name equals --t2 off" '[[ "$(R --feet-stop --c8 off --dodge off --aversion off --aim off --block off --range off --backoff off --step off --c13 lever --unweighted off --reanimate off --classes evoker,knight --seeds 2 | head -1)" == "$(R --t2 off --classes evoker,knight --seeds 2 | head -1)" ]]'
    chk "T2 batch S: --t2 off --roads bash is the lab of section 30 (foe casting is filled; review F1)" '[[ "$(R --t2 off --roads bash --classes knight,warrior,mercenary --seeds 1 | head -1)" == "SEED 1  J=78.21 (scenarios 73.07, natural 83.34)" ]]'   # 79.45 in section 30; 79.06 with the School Circle (the Knight scenarios' healer); 78.21 with the Mercenary in light armor
    chk "T2 batch S: the headline names its roads"     '[[ "$(R --classes mage --seeds 1 | tail -1)" == *"t2=all" && "$(R --classes mage --seeds 1 --feet-stop --block off | tail -1)" == *"t2=feet-stop,block=off" ]]'
    chk "T2 batch S: the Knight's table row raises the held guard (MFO-B827)" 'B="$(mktemp)"; R --mode table --classes knight --fights 2 --baseline "$B" >/dev/null; G="$(grep "^knight|natural|guard%=" "$B" | cut -d= -f2)"; rm -f "$B"; awk -v g="$G" "BEGIN { exit !(g > 5) }"'
    chk "T2 batch S: --disengage-report and --steal-lab print their tables" 'R --disengage-report 2 --classes evoker | grep -q "^evoker     dnd" && R --steal-lab 500 | grep -q "^dark "'
    chk "class curves: an unknown --curve class / family / param is an error" 'R --classes knight --curve nope:drink_health.need.b=0.9 | grep -q "unknown class" && R --classes knight --curve knight:nope.need.b=0.9 | grep -q "unknown family" && R --classes knight --curve knight:drink_health.need.zz=0.9 | grep -q "unknown curve param"'
    chk "class curves: --curve changes that class's score and no other class's" '[[ "$(R --classes knight --seeds 1 --fights 4 --curve knight:drink_health.need.b=0.9 | head -1)" != "$(R --classes knight --seeds 1 --fights 4 | head -1)" && "$(R --classes mage --seeds 1 --fights 4 --curve knight:drink_health.need.b=0.9 | head -1)" == "$(R --classes mage --seeds 1 --fights 4 | head -1)" ]]'
    chk "class curves: an override equal to the shared value is the baseline" '[[ "$(R --classes knight --seeds 1 --fights 4 --curve knight:scalar.drinkHealthLine=0.34 --curve knight:scalar.touchSpentSec=3 | head -1)" == "$(R --classes knight --seeds 1 --fights 4 | head -1)" ]]'
    chk "sim batch: --opt c.N next to a --curve on consideration N is refused" 'R --classes knight --curve knight:drink_health.32.b=0.5 --opt c.32.b | grep -q "refused" && ! R --classes knight --curve knight:drink_health.32.b=0.5 --opt c.31.b --opt-fights 1 --opt-passes 1 | grep -q "refused"'
    chk "sim batch: the tree-OFF base cells are real cells with their own curve rows" 'R --print-classes | grep -q "melee-off" && [[ "$(R --classes melee-off --seeds 1 --fights 4 --curve melee-off:scalar.touchSpentSec=3 | head -1)" == "$(R --classes melee-off --seeds 1 --fights 4 | head -1)" ]] && [[ "$(R --classes melee-off --seeds 1 --fights 4 --curve melee-off:drink_health.32.b=0.9 | head -1)" != "$(R --classes melee-off --seeds 1 --fights 4 | head -1)" ]] && [[ "$(R --classes melee --seeds 1 --fights 4 --curve melee-off:drink_health.32.b=0.9 | head -1)" == "$(R --classes melee --seeds 1 --fights 4 | head -1)" ]]'
    chk "sim batch: beta1 is the 12 tree-ON classes and the 3 base cells; beta1-tree is the 12" '[[ "$(R --classes beta1 --seeds 1 | tail -1)" == *"warlock,melee-off,ranged-off,mage-off "* && "$(R --classes beta1-tree --seeds 1 | tail -1)" == *"healer,warlock "* ]]'
    chk "sim batch: the rating battery plays the same fights for a tier whatever else runs" 'A="$(R --rating-only --rating 1 --classes knight | awk "\$1==\"knight\"{print \$3}")"; B="$(R --rating-only --rating 1 --classes knight,healer,archer | awk "\$1==\"knight\"{print \$3}")"; [[ -n "$A" && "$A" == "$B" ]]'
    chk "sim batch: a class report run writes the rating, the per-class gap and the ledger slugs (schema 2)" 'J="$(mktemp)"; "$OUT" --classes knight,evoker --fights 1 --threads 8 --class-oracle 1 --gap-scenarios 0 --oracle-k 1 --oracle-h 5 --rating 1 --json "$J" >/dev/null 2>&1; grep -q "^{\"schema\":2" "$J" && grep -q "\"class_gaps\":\[" "$J" && grep -q "{\"class\":\"knight\",\"gap\":" "$J" && grep -q "\"rating\":{\"fights_per_cell\":1" "$J" && grep -q "\"class\":\"knight line\".*\"classes\":\[\"knight\"\]" "$J" && python3 "'"$ROOT"'/tools/iaus_dash/dash.py" --json-check "$J" >/dev/null; rc=$?; rm -f "$J"; [[ $rc == 0 ]]'
    chk "sim batch: the Mercenary line wears light armor (its card) and a heavy cuirass never dodges" 'R --print-classes | grep -q "T2 mercenary .* light" && R --print-classes | grep -q "T2 warrior .* heavy" && [[ "$(R --classes warrior --fights 4 --rating 0 --baseline /dev/stdout | grep "^warrior|natural|dodge/f=" | cut -d= -f2)" == "0" ]]'
    chk "an unknown --sneak-profile is an error"  'R --sneak-profile nope --classes mage --seeds 1 | grep -q "unknown --sneak-profile"'
    if [[ $fail == 0 ]]; then echo "CLI-CHECKS PASS"; exit 0; else echo "CLI-CHECKS FAIL"; exit 1; fi
fi
exec "$OUT" "$@"
