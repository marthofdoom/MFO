#!/usr/bin/env bash
# Build a simulator VARIANT in which ONE compile-time IAUS constant has another value, without touching native/.
# HOST-ONLY. For the edge-number rule (marth 11-12: every IAUS constant that decides an edge case is found by a sim search):
# the constants below are `constexpr` in native/iaus, so --set cannot reach them. This script copies native/iaus into a
# scratch include directory, rewrites the one initializer there, and compiles the simulator with that directory FIRST on the
# include path. The game's sources are never edited; a value this finds is a recommendation with its numbers, to be applied
# to native/ by its own reviewed change.
#
# Usage: tools/iaus_sim/const_probe.sh <name> <value> <out-binary>
#   <name>: kGuardLineFoes | kFleeHealthLine | kPressCount | kCloseSlack | kMinimumFrac | kRangeVeto | kPostureDist | kDrinkHealthLine
# Then run the variant like the simulator itself (it takes the same arguments), e.g. with tools/iaus_sim/edge_search.py gates --bin.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NAME="${1:?name}"; VALUE="${2:?value}"; OUT="${3:?out binary}"
P="${OUT}.patch"; GEN="${OUT}.gen"; rm -rf "$P"; mkdir -p "$P/iaus" "$GEN"
cp "$ROOT"/native/iaus/*.h "$ROOT"/native/iaus/*.cpp "$P/iaus/"
case "$NAME" in
    kGuardLineFoes)   F=Core.h ;;
    kDrinkHealthLine) F=Core.h ;;
    kFleeHealthLine)  F=Disengage.h ;;
    kPressCount|kCloseSlack|kMinimumFrac) F=Reposition.h ;;
    kRangeVeto)       F=Generate.cpp ;;
    kPostureDist)     F=Catalogue.cpp ;;
    *) echo "unknown constant '$NAME'" >&2; exit 2 ;;
esac
# exactly one initializer `NAME = <literal>` must be rewritten, or the probe would silently measure the shipped value
BEFORE="$(grep -c -E "constexpr [A-Za-z_:0-9 ]*\b${NAME} = [-0-9.]+f?;" "$P/iaus/$F" || true)"
if [[ "$BEFORE" != 1 ]]; then echo "expected exactly one 'constexpr ... $NAME = <number>;' in native/iaus/$F, found $BEFORE" >&2; exit 2; fi
sed -i -E "s/(constexpr [A-Za-z_:0-9 ]*\b${NAME} = )[-0-9.]+(f?);/\1${VALUE}\2;/" "$P/iaus/$F"
grep -n -E "\b${NAME} = " "$P/iaus/$F" | head -1 >&2
python3 -I "$ROOT/tools/iaus_sim/gen_vocab.py" "$ROOT/native/Vocabulary.h" "$GEN/Vocabulary.h"
"${CXX:-g++}" -std=c++23 -O2 -pthread -Wall -Wextra -Wno-misleading-indentation -Wno-parentheses \
    -I "$P" -I "$GEN" -I "$ROOT/tools/iaus_sim/stub" -I "$ROOT/native" -I "$ROOT/tools" \
    "$P/iaus/Curves.cpp" "$P/iaus/Catalogue.cpp" "$P/iaus/ClassCurves.cpp" "$P/iaus/Score.cpp" \
    "$P/iaus/Decide.cpp" "$P/iaus/Explain.cpp" "$P/iaus/Generate.cpp" \
    "$ROOT/native/classes/ClassTable.cpp" "$ROOT/tools/iaus_sim/RealClasses.cpp" \
    "$ROOT/tools/iaus_sim/sim.cpp" -o "$OUT"
