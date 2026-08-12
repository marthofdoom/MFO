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
python3 tools/audit_mcm.py          # #55 gate: every MCM toggle wired in all 5 places
echo

# 2. DLL from the latest successful CI run.
RUN_ID="$($GH run list --workflow=native --status=success --limit 1 --json databaseId -q '.[0].databaseId')"
if [ -z "$RUN_ID" ]; then
    echo "FAIL: no successful 'native' run to download a DLL from." >&2
    echo "      Check: $GH run list --workflow=native" >&2
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
cp "$STAGE/dll/MFO.dll" "$STAGE/pkg/SKSE/Plugins/"
[ -f out/SKSE/Plugins/MFO.ini ] && cp out/SKSE/Plugins/MFO.ini "$STAGE/pkg/SKSE/Plugins/"
[ -d out/SKSE/Plugins/MFO ] && cp -r out/SKSE/Plugins/MFO "$STAGE/pkg/SKSE/Plugins/"   # baked board fonts
cp THIRD-PARTY-NOTICES.md "$STAGE/pkg/"   # ships with every build (INVARIANTS #42a)
[ -d out/MCM ] && cp -r out/MCM "$STAGE/pkg/"   # MCM Helper config.json
[ -d out/Scripts ] && cp -r out/Scripts "$STAGE/pkg/"   # MCM Helper compiled script (MFO_MCM.pex)

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
