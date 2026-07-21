#!/usr/bin/env bash
# Cut a release. Builds are archived under releases/vX.Y.Z/ PERMANENTLY.
#
# Usage:
#   ./release.sh            release the version currently in VERSION
#   ./release.sh 0.1.0      bump VERSION to 0.1.0 and release it
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

if [[ $# -ge 1 ]]; then
    echo "$1" > VERSION
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

# 1. Stamp the version so the in-game log header matches the zip. INVARIANTS
#    #44: a stale binary voids every test, and this header is how that is caught.
#
#    ONLY CMakeLists.txt. Do NOT stamp native/vcpkg.json: its version-string is
#    metadata about our own port and affects nothing, but it IS part of the CI
#    cache key (hashFiles over the manifests), so touching it invalidates the
#    vcpkg cache and turns every release into a ~30 min cold rebuild instead of
#    ~2.5 min. Learned by doing it once.
sed -i "s/^project(MFO VERSION [0-9.]*/project(MFO VERSION ${VER}/" native/CMakeLists.txt
if [[ -n "$(git status --porcelain native/ 2>/dev/null)" ]]; then
    echo "Version stamp changed native/ — commit and let CI rebuild, then re-run." >&2
    git status --short native/ >&2
    exit 1
fi

# 2. ESP + SEQ, always regenerated so a zip can never carry a stale plugin.
python3 MFO_GenerateESP.py out >/dev/null
python3 tools/audit_esp.py          # PASS is a merge gate; a FAIL stops the release
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
mkdir -p "$STAGE/pkg/SKSE/Plugins" "$STAGE/pkg/SEQ"
cp out/MFO.esp             "$STAGE/pkg/"
cp out/SEQ/MFO.seq         "$STAGE/pkg/SEQ/"
cp out/SKSE/Plugins/MFO.ini "$STAGE/pkg/SKSE/Plugins/"
cp "$STAGE/dll/MFO.dll"    "$STAGE/pkg/SKSE/Plugins/"
cp THIRD-PARTY-NOTICES.md  "$STAGE/pkg/"    # ships with every build, INVARIANTS #42a

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
