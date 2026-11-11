#!/usr/bin/env bash
# Build and run the emulated save round trip of the 'MCLS' class co-save record with the host compiler (no CommonLib,
# no engine). Card C12 (MCLS v2). See test.cpp for what is checked.
# Usage: tools/mcls_byte_test/run.sh            (from the repo root, or anywhere)
#        CXX=clang++ tools/mcls_byte_test/run.sh
#        MCLS_TEST_VERBOSE=1 ...                (print every log line the codec wrote)
#        MCLS_SAN=0 ...                         (build without -fsanitize=address,undefined)
# The v1 DLL half (golden check + downgrade) needs commit f21f6c7 in the clone; without it that half is SKIPPED, loudly.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT/tools/mcls_byte_test"
OUT="${TMPDIR:-/tmp}/mcls_byte_test"
V1_SHA=f21f6c76933f113f19b4872067cbca53c6ca919a   # the last commit whose DLL wrote MCLS v1
CXX="${CXX:-g++}"
SAN=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
[[ "${MCLS_SAN:-1}" == 0 ]] && SAN=()
rm -rf "$OUT" && mkdir -p "$OUT/new" "$OUT/old"

build() {   # build <native tree> <gen dir> <output> [extra flags]
    local native="$1" gen="$2" out="$3"; shift 3
    python3 -I "$HERE/gen_rowcodec.py" "$native/Serialization.cpp" "$gen/rowcodec.inc"
    "$CXX" -std=c++23 -O1 -g -Wall -Wextra -Wno-multichar "${SAN[@]}" "$@" \
        -I "$gen" -I "$HERE/stub" -I "$native" \
        "$native/classes/State.cpp" "$native/classes/Classes.cpp" "$native/classes/ClassTable.cpp" \
        "$native/classes/Mastery.cpp" "$HERE/test.cpp" -o "$out"
}

build "$ROOT/native" "$OUT/new" "$OUT/new/test"

OLD=0
if git -C "$ROOT" cat-file -e "$V1_SHA^{commit}" 2>/dev/null; then
    git -C "$ROOT" archive "$V1_SHA" native/classes native/Serialization.cpp native/Serialization.h native/State.h \
        native/Config.h native/Vocabulary.h native/PCH.h native/CombatStyle.h | tar -x -C "$OUT/old"
    build "$OUT/old/native" "$OUT/old" "$OUT/old/test" -DMCLS_OLD_DLL
    "$OUT/old/test" --emit-v1 > "$OUT/old/v1.hex"
    if ! cmp -s "$OUT/old/v1.hex" "$HERE/golden_v1.hex"; then
        echo "[FAIL] golden_v1.hex is NOT what the v1 DLL ($V1_SHA) writes (see $OUT/old/v1.hex)"; exit 1
    fi
    echo "golden_v1.hex == the record the v1 DLL (${V1_SHA:0:7}) writes"
    OLD=1
else
    echo "SKIPPED: commit $V1_SHA is not in this clone -- golden_v1.hex is not re-derived and the downgrade is not run"
fi

"$OUT/new/test" "$HERE/golden_v1.hex" "$OUT/new/v2.hex"
if [[ $OLD == 1 ]]; then
    echo
    "$OUT/old/test" --downgrade "$OUT/new/v2.hex"
fi
