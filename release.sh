#!/usr/bin/env bash
# Cut a release. Builds are archived under releases/vX.Y.Z/ PERMANENTLY.
#
# Usage:
#   ./release.sh            release the version currently in VERSION
#   ./release.sh 0.2.0      PHASE 1: stamp + commit + push, then wait for CI
#
# Release folders and tags are IMMUTABLE — bump VERSION for every build you
# want to keep. Nothing is ever overwritten or deleted. This is the ONLY way a
# build reaches the game: MRO lost a session to a hand-copied DLL where the
# running game and the archive disagreed about what was live.
#
# The DLL comes from the latest GREEN CI run, never a local build — there is
# no local MSVC by design, so CI is the only compiler that ever sees this code.
set -euo pipefail
cd "$(dirname "$0")"

GH="${GH:-$HOME/.local/bin/gh}"

# TWO-PHASE, deliberately. Stamping the version touches native/CMakeLists.txt,
# which changes the native/ tree -- and the DLL we ship must come from a CI run
# that built exactly that tree. So the bump has to be committed and BUILT before
# the release can be cut. Trying to do both in one pass means either shipping a
# DLL whose version header does not match the zip, or skipping the check that
# catches exactly that.
#
#   ./release.sh 0.2.0   -> phase 1: stamp, commit, push. Wait for CI.
#   ./release.sh         -> phase 2: verify + package + tag.
if [[ $# -ge 1 ]]; then
    NEWVER="$1"
    if [[ -n "$(git status --porcelain 2>/dev/null)" ]]; then
        echo "ERROR: commit your work before bumping the version." >&2
        git status --short >&2
        exit 1
    fi
    echo "$NEWVER" > VERSION
    sed -i "s/^project(MFO VERSION [0-9.]*/project(MFO VERSION ${NEWVER}/" native/CMakeLists.txt
    # Stamp the MCM Debug-page "Version" readout (MEO/MAO style). The `"value"`
    # key is unique to that text element in config.json, so this hits nothing
    # else. Keeps the in-game readout matching the built DLL for free.
    sed -i "s/\"value\": \"v[0-9.]*\"/\"value\": \"v${NEWVER}\"/" out/MCM/Config/MFO/config.json
    git add VERSION native/CMakeLists.txt out/MCM/Config/MFO/config.json
    git commit -q -m "Bump version to ${NEWVER}"
    git push -q origin main
    echo "Stamped ${NEWVER} and pushed. CI is rebuilding native/."
    echo "When it is green:  ./release.sh"
    exit 0
fi

VER="$(cat VERSION)"
DEST="releases/v${VER}"

if [[ -e "$DEST" ]]; then
    echo "ERROR: $DEST already exists. Releases are immutable — bump VERSION." >&2
    exit 1
fi

# Every release ships with its changelog entry. Enforced mechanically because
# in MRO the convention was silently skipped once and backfilled after the fact.
if ! grep -q "^## v${VER}" CHANGELOG.md 2>/dev/null; then
    echo "ERROR: CHANGELOG.md has no '## v${VER}' entry. Write the changelog first." >&2
    exit 1
fi

# Refuse to ship uncommitted work — otherwise the tag does not describe the zip.
if [[ -n "$(git status --porcelain 2>/dev/null)" ]]; then
    echo "ERROR: working tree is dirty. Commit first, or the tag lies about what shipped." >&2
    git status --short >&2
    exit 1
fi

echo "== MFO v${VER} =="

# 1. VERIFY the stamp. The log header is how a stale binary gets caught
#    (INVARIANTS #44), so it must match the zip. Phase 1 does the stamping;
#    here we only check it happened and was built.
#
#    Note: only CMakeLists.txt carries the version. native/vcpkg.json's
#    version-string is metadata that affects nothing at build time but IS part
#    of the CI cache key, so stamping it would invalidate the vcpkg cache on
#    every release. Learned by doing it once.
STAMPED="$(grep -oP '^project\(MFO VERSION \K[0-9.]+' native/CMakeLists.txt)"
if [[ "$STAMPED" != "$VER" ]]; then
    echo "ERROR: VERSION says ${VER} but native/CMakeLists.txt says ${STAMPED}." >&2
    echo "       Run:  ./release.sh ${VER}    (stamps, commits, pushes; then wait for CI)" >&2
    exit 1
fi

# 2. ESP + SEQ, always regenerated so a zip can never carry a stale plugin.
python3 MFO_GenerateESP.py out >/dev/null
python3 tools/audit_esp.py          # PASS is a merge gate; a FAIL stops the release
echo

# 2b. Papyrus, always recompiled (Wine + Nemesis compiler) so the shipped .pex
# can never lag Source/Scripts. If the compiler is unreachable, fall back to the
# committed out/Scripts/*.pex -- but a MISSING MFO_Trade.pex is fatal (its VMAD
# would reference a dead script). #21 econ bridge.
if [[ -x "/mnt/gaming/Steam/steamapps/common/Proton Hotfix/files/bin/wine" ]]; then
    tools/compile.sh all || { echo "ERROR: Papyrus compile failed." >&2; exit 1; }
else
    echo "WARN: Wine compiler absent -- shipping committed out/Scripts/*.pex"
fi
[[ -f out/Scripts/MFO_Trade.pex ]] || { echo "ERROR: out/Scripts/MFO_Trade.pex missing." >&2; exit 1; }
echo

# 3. DLL from the latest green CI run, with its provenance recorded.
RUN_ID="$($GH run list --workflow=native --status=success --limit 1 --json databaseId -q '.[0].databaseId')"
if [[ -z "$RUN_ID" ]]; then
    echo "ERROR: no successful 'native' run to take a DLL from." >&2
    exit 1
fi
# Compare the native/ TREE, not the commit sha. The DLL is a function of
# native/ alone, so a docs- or script-only commit since the last green run is
# harmless — but any drift in native/ means the artifact is not this code.
# Comparing shas instead would force a pointless rebuild every time this very
# file changed.
RUN_SHA="$($GH run view "$RUN_ID" --json headSha -q '.headSha')"
HEAD_TREE="$(git rev-parse HEAD:native)"
RUN_TREE="$(git rev-parse "${RUN_SHA}:native" 2>/dev/null || echo unknown)"
if [[ "$RUN_TREE" != "$HEAD_TREE" ]]; then
    echo "ERROR: native/ has changed since the last green CI run." >&2
    echo "       run $RUN_ID built ${RUN_SHA:0:8} (native tree ${RUN_TREE:0:8})" >&2
    echo "       HEAD is $(git rev-parse --short HEAD) (native tree ${HEAD_TREE:0:8})" >&2
    echo "       Push and wait for CI, or you will ship a DLL that is not this code." >&2
    exit 1
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
$GH run download "$RUN_ID" -n MFO-dll -D "$STAGE/dll"

# 4. Stage in Data/ layout — zip root IS the virtual Data folder, so MO2
#    installs it with zero manual placement.
mkdir -p "$STAGE/pkg/SKSE/Plugins" "$STAGE/pkg/SEQ" \
         "$STAGE/pkg/MCM/Config/MFO" "$STAGE/pkg/MCM/Settings" "$STAGE/pkg/Scripts"
cp out/MFO.esp             "$STAGE/pkg/"
cp out/SEQ/MFO.seq         "$STAGE/pkg/SEQ/"
cp out/SKSE/Plugins/MFO.ini "$STAGE/pkg/SKSE/Plugins/"
cp -r out/SKSE/Plugins/MFO   "$STAGE/pkg/SKSE/Plugins/"   # baked board fonts (MEO parity)
cp "$STAGE/dll/MFO.dll"    "$STAGE/pkg/SKSE/Plugins/"
# MCM Helper config + its binding script. WITHOUT BOTH the MCM never appears:
# config.json alone registers the config but renders nothing, and the ESP's
# MFO_MCMQuest attaches MFO_MCM (an MCM_ConfigBase subclass) whose .pex MUST
# ship or the quest silently fails to become an MCM (2026-07-28 root cause --
# every zip before this one shipped neither file).
cp out/MCM/Config/MFO/config.json "$STAGE/pkg/MCM/Config/MFO/"
# Initial MCM Helper settings store -- WITHOUT it every ModSetting control reads
# back -1 (no value to bind to), so the whole MCM shows -1 (2026-07-28).
cp out/MCM/Settings/MFO.ini       "$STAGE/pkg/MCM/Settings/"
cp out/Scripts/MFO_MCM.pex        "$STAGE/pkg/Scripts/"
# #21 econ bridge: MFO_Trade.pex MUST ship or MFO_TradeQuest's VMAD references a
# missing script and the bridge is silently dead. Compiled fresh from
# Source/Scripts/MFO_Trade.psc by tools/compile.sh (checked below).
cp out/Scripts/MFO_Trade.pex      "$STAGE/pkg/Scripts/"
cp THIRD-PARTY-NOTICES.md  "$STAGE/pkg/"    # ships with every build, INVARIANTS #42a
cp OFL.txt                 "$STAGE/pkg/"    # SIL OFL 1.1 for the board fonts (RC#1); OFL 2 requires it travel

ZIP="MFO-v${VER}.zip"
rm -f "$ZIP"
(cd "$STAGE/pkg" && zip -rq "$OLDPWD/$ZIP" .)

mkdir -p "$DEST"
cp "$ZIP" "$DEST/"
{
    echo "MFO v${VER}"
    echo "commit:   $(git rev-parse HEAD)"
    echo "ci run:   $RUN_ID"
    echo "dll:      $(sha256sum "$STAGE/pkg/SKSE/Plugins/MFO.dll" | cut -d' ' -f1)"
    echo "esp:      $(sha256sum "$STAGE/pkg/MFO.esp" | cut -d' ' -f1)"
    echo "built:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$DEST/MANIFEST.txt"

git tag "v${VER}"    # fails if it exists — tags are immutable too

echo
cat "$DEST/MANIFEST.txt"
echo
echo "Released -> $DEST/$ZIP"
echo "Push the tag when ready:  git push origin v${VER}"
