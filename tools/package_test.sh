#!/usr/bin/env bash
# Build an MO2-installable test zip from the CI DLL + the generated ESP.
#
# The zip root IS the virtual Data/ folder, so MO2 installs it with zero
# manual placement.
#
# This is NOT the release path. Releases go through an immutable tagged
# release script; this is for iterating on a test profile.
set -euo pipefail

cd "$(dirname "$0")/.."
GH="${GH:-$HOME/.local/bin/gh}"
VER="$(cat VERSION)"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

echo "MFO ${VER} — test package"

# 1. ESP + SEQ, always regenerated so the zip can never contain a stale plugin.
python3 MFO_GenerateESP.py out >/dev/null
python3 tools/audit_esp.py            # PASS is a merge gate; fail here stops the package
python3 tools/audit_mcm.py          # #55 gate: every MFO MCM toggle wired in all 5 places
# The addon's own GlobalValue MCM config (economy tab, ships in the ESL).
python3 tools/audit_mcm.py out/MCM/Config/MFO_Progression/config.json
echo

# 2. DLL from the successful CI run FOR THIS EXACT COMMIT.
#    It used to take `--limit 1` of ANY successful native run on ANY branch, so
#    packaging could ship a DLL built from a completely different branch than the
#    tree being packaged -- the zip and the binary would silently disagree, which
#    is the same class of failure the release script's stamp check exists to stop.
#    Pin it to HEAD and fail loudly rather than guessing.
HEAD_SHA="$(git rev-parse HEAD)"
RUN_ID="$($GH run list --workflow=native --status=success --commit "$HEAD_SHA" --limit 1 --json databaseId -q '.[0].databaseId')"
if [ -z "$RUN_ID" ]; then
    echo "FAIL: no successful 'native' run for THIS commit (${HEAD_SHA})." >&2
    echo "      A green run on another commit is NOT a substitute -- the DLL would" >&2
    echo "      not match this tree." >&2
    echo "      Check:  $GH run list --workflow=native --commit ${HEAD_SHA}" >&2
    echo "      If this commit is docs-only, native/ was not rebuilt and no run" >&2
    echo "      exists (the workflow has a paths: filter). Confirm the last green" >&2
    echo "      commit is native-identical before packaging from it:" >&2
    echo "        git diff --quiet <green-sha> ${HEAD_SHA} -- native/ && echo IDENTICAL" >&2
    exit 1
fi
echo "Downloading MFO.dll from run ${RUN_ID}"
$GH run download "$RUN_ID" -n MFO-dll -D "$STAGE/dll"

# 3. Stage in Data/ layout.
mkdir -p "$STAGE/pkg/SKSE/Plugins" "$STAGE/pkg/SEQ"
cp out/MFO.esp        "$STAGE/pkg/"
# The OPTIONAL progression addon rides the TEST zip so the allocator is
# exercisable on the deck (enable its plugin too, or it stays undetected —
# that un-detected state is itself a valid test). Release packaging ships it
# separately; this is the iteration path only.
cp out/MFO_Progression.esl "$STAGE/pkg/"
cp out/SEQ/MFO.seq    "$STAGE/pkg/SEQ/"
# The addon's OWN MCM quest SEQ (its economy tab) -- rides with the ESL.
cp out/SEQ/MFO_Progression.seq "$STAGE/pkg/SEQ/"
cp "$STAGE/dll/MFO.dll" "$STAGE/pkg/SKSE/Plugins/"
[ -f out/SKSE/Plugins/MFO.ini ] && cp out/SKSE/Plugins/MFO.ini "$STAGE/pkg/SKSE/Plugins/"
[ -d out/SKSE/Plugins/MFO ] && cp -r out/SKSE/Plugins/MFO "$STAGE/pkg/SKSE/Plugins/"   # baked board fonts
cp THIRD-PARTY-NOTICES.md "$STAGE/pkg/"   # ships with every build (INVARIANTS #42a)
[ -d out/MCM ] && cp -r out/MCM "$STAGE/pkg/"   # MCM Helper configs (MFO + MFO_Progression)
[ -d out/Scripts ] && cp -r out/Scripts "$STAGE/pkg/"   # MCM compiled scripts (MFO_MCM.pex + MFOP_MCM.pex)

OUT="$PWD/MFO-test-v${VER}.zip"
rm -f "$OUT"
(cd "$STAGE/pkg" && zip -rq "$OUT" .)

echo
echo "Written: $OUT"
echo "  MFO.dll  $(sha256sum "$STAGE/pkg/SKSE/Plugins/MFO.dll" | cut -c1-16)…"
echo "  MFO.esp  $(sha256sum "$STAGE/pkg/MFO.esp" | cut -c1-16)…"
echo
echo "Install in MO2, then CHECK BOTH BOXES — left-pane mod AND right-pane plugin."
echo "Verify the version header in MFO.log before believing any test result."
