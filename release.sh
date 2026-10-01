#!/usr/bin/env bash
# Cut a release. Builds are archived under releases/vX.Y.Z/ PERMANENTLY.
#
# Usage (also ./release.sh --help):
#   ./release.sh 0.2.0              PHASE 1: stamp + commit + push, then wait for CI
#   ./release.sh --run <run-id>     PHASE 2: release the version in VERSION, taking
#                                   the DLL from exactly that green 'native' CI run
#   ./release.sh --run <run-id> --dry-run
#                                   phase 2's checks only (stamp, run, artifact);
#                                   writes, packages and tags nothing
#   ./release.sh                    lists the last green 'native' runs and stops
#
# Release folders and tags are IMMUTABLE — bump VERSION for every build you
# want to keep. Nothing is ever overwritten or deleted. This is the ONLY way a
# build reaches the game: MRO lost a session to a hand-copied DLL where the
# running game and the archive disagreed about what was live.
#
# The DLL comes from a GREEN CI run the operator names with --run, never a
# local build — there is no local MSVC by design, so CI is the only compiler
# that ever sees this code. The run is never guessed: with parallel branch
# builds "the newest green run" is often another branch's DLL.
set -euo pipefail
cd "$(dirname "$0")"

GH="${GH:-$HOME/.local/bin/gh}"
ARTIFACT="MFO-dll"
DLL_NAME="MFO.dll"

usage() {
    cat <<EOF
Usage:
  ./release.sh <X.Y.Z>                   PHASE 1: stamp VERSION, native/CMakeLists.txt and
                                         the MCM version readout, commit, push. Wait for CI.
  ./release.sh --run <run-id>            PHASE 2: verify, package and tag the version in
                                         VERSION, with the DLL from that 'native' CI run.
  ./release.sh --run <run-id> --dry-run  PHASE 2 checks only. Nothing is written or tagged.
  ./release.sh                           list the last green 'native' runs, then stop.
  ./release.sh --help                    this text.

--run <run-id> (or --run=<run-id>) is REQUIRED for phase 2. The run must be a
'native' workflow run that completed with 'success', whose commit has the SAME
native/ tree as HEAD, and that still holds the ${ARTIFACT} artifact. Anything
else stops the release. Pick the id from the list printed by a bare ./release.sh
(or: gh run list --workflow=native --status=success).
EOF
}

RUN_SEL=""
DRY_RUN=0
POS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --run)     [[ $# -ge 2 && -n "$2" ]] || { echo "ERROR: --run needs a run id." >&2; exit 1; }
                   RUN_SEL="$2"; shift 2 ;;
        --run=*)   RUN_SEL="${1#--run=}"; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        -*)        echo "ERROR: unknown option '$1'." >&2; usage >&2; exit 1 ;;
        *)         POS+=("$1"); shift ;;
    esac
done
if [[ ${#POS[@]} -gt 1 ]]; then
    echo "ERROR: too many arguments: ${POS[*]}" >&2; usage >&2; exit 1
fi
if [[ ${#POS[@]} -eq 1 && ( -n "$RUN_SEL" || "$DRY_RUN" -eq 1 ) ]]; then
    echo "ERROR: --run / --dry-run belong to phase 2; phase 1 (./release.sh ${POS[0]}) takes no options." >&2
    exit 1
fi

# Print the last green 'native' runs so the operator can pick one for --run.
# The mark says which of them built the native/ tree HEAD has.
list_green_runs() {
    local head_tree
    head_tree="$(git rev-parse HEAD:native)"
    echo "Last green 'native' runs, newest first (HEAD $(git rev-parse --short HEAD), native tree ${head_tree:0:8}):" >&2
    printf '  %-12s %-9s %-20s  %-40s %s\n' RUN SHA CREATED BRANCH "" >&2
    $GH run list --workflow=native --status=success --limit 8 \
        --json databaseId,headBranch,headSha,createdAt \
        -q '.[] | [(.databaseId|tostring), .headSha, .createdAt, .headBranch] | join("|")' |
    while IFS='|' read -r id sha created branch; do
        local tree mark=""
        tree="$(git rev-parse -q --verify "${sha}:native" 2>/dev/null || true)"
        if [[ -z "$tree" ]]; then
            mark="(commit not fetched here)"
        elif [[ "$tree" == "$head_tree" ]]; then
            mark="<- native/ matches HEAD"
        fi
        printf '  %-12s %-9s %-20s  %-40s %s\n' "$id" "${sha:0:8}" "$created" "$branch" "$mark" >&2
    done
}

# Resolve --run into RUN_ID + RUN_SHA, refusing anything that is not a green
# 'native' run of exactly HEAD's native/ tree with the DLL artifact still there.
select_run() {
    if [[ -z "$RUN_SEL" ]]; then
        echo "ERROR: phase 2 needs the CI run to take the DLL from:  ./release.sh --run <run-id>" >&2
        echo "       It is never picked automatically: with parallel branch builds the newest" >&2
        echo "       green run is often another branch's DLL." >&2
        list_green_runs
        exit 1
    fi
    if [[ ! "$RUN_SEL" =~ ^[0-9]+$ ]]; then
        echo "ERROR: --run takes a numeric run id, got '${RUN_SEL}'." >&2
        exit 1
    fi
    local info wf status concl branch created
    if ! info="$($GH run view "$RUN_SEL" --json workflowName,status,conclusion,headBranch,headSha,createdAt \
            -q '[.workflowName, .status, .conclusion, .headBranch, .headSha, .createdAt] | join("|")')"; then
        echo "ERROR: gh could not read run ${RUN_SEL}." >&2
        exit 1
    fi
    IFS='|' read -r wf status concl branch RUN_SHA created <<<"$info"
    echo "CI run:   ${RUN_SEL}  branch ${branch}  sha ${RUN_SHA}  (${wf}, ${status}/${concl:-none}, ${created})"
    if [[ "$wf" != "native" ]]; then
        echo "ERROR: run ${RUN_SEL} is workflow '${wf}', not 'native'." >&2
        exit 1
    fi
    if [[ "$status" != "completed" || "$concl" != "success" ]]; then
        echo "ERROR: run ${RUN_SEL} is ${status}/${concl:-none}, not completed/success." >&2
        exit 1
    fi
    # Compare the native/ TREE, not the commit sha. The DLL is a function of
    # native/ alone, so a docs- or script-only commit since the run is harmless
    # — but any drift in native/ means the artifact is not this code.
    # Comparing shas instead would force a pointless rebuild every time this very
    # file changed.
    local head_tree run_tree
    head_tree="$(git rev-parse HEAD:native)"
    run_tree="$(git rev-parse -q --verify "${RUN_SHA}:native" 2>/dev/null || echo unknown)"
    if [[ "$run_tree" != "$head_tree" ]]; then
        echo "ERROR: run ${RUN_SEL} did not build HEAD's native/ tree." >&2
        echo "       run ${RUN_SEL} built ${RUN_SHA:0:8} on ${branch} (native tree ${run_tree:0:8})" >&2
        echo "       HEAD is $(git rev-parse --short HEAD) (native tree ${head_tree:0:8})" >&2
        [[ "$run_tree" == unknown ]] && echo "       (${RUN_SHA:0:8} is not in this clone: git fetch origin, then retry)" >&2
        echo "       Pick a run that built this code, or you will ship a DLL that is not it." >&2
        exit 1
    fi
    local names
    if ! names="$($GH api "repos/{owner}/{repo}/actions/runs/${RUN_SEL}/artifacts?per_page=100" \
            -q '.artifacts[] | select(.expired | not) | .name')"; then
        echo "ERROR: gh could not list the artifacts of run ${RUN_SEL}." >&2
        exit 1
    fi
    if ! grep -qxF "$ARTIFACT" <<<"$names"; then
        echo "ERROR: run ${RUN_SEL} has no unexpired '${ARTIFACT}' artifact (has: ${names//$'\n'/, })." >&2
        exit 1
    fi
    RUN_ID="$RUN_SEL"
    echo "CI run ${RUN_ID} verified: green, native/ tree matches HEAD, ${ARTIFACT} present."
}

# TWO-PHASE, deliberately. Stamping the version touches native/CMakeLists.txt,
# which changes the native/ tree -- and the DLL we ship must come from a CI run
# that built exactly that tree. So the bump has to be committed and BUILT before
# the release can be cut. Trying to do both in one pass means either shipping a
# DLL whose version header does not match the zip, or skipping the check that
# catches exactly that.
#
#   ./release.sh 0.2.0          -> phase 1: stamp, commit, push. Wait for CI.
#   ./release.sh --run <id>     -> phase 2: verify + package + tag.
if [[ ${#POS[@]} -eq 1 ]]; then
    NEWVER="${POS[0]}"
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
    echo "When it is green:  ./release.sh --run <run-id>   (a bare ./release.sh lists the green runs)"
    exit 0
fi

# Phase 2 without --run: list the green runs to pick from and stop, before any
# other check, so the list is always one bare ./release.sh away.
[[ -n "$RUN_SEL" ]] || select_run

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

# 1b. The CI run the DLL comes from, named by the operator (--run). Checked
#     BEFORE anything below regenerates or writes a file, so a wrong or missing
#     run stops the release with nothing touched.
select_run
if [[ "$DRY_RUN" -eq 1 ]]; then
    echo
    echo "DRY RUN: every phase 2 check passed for v${VER} with run ${RUN_ID}."
    echo "         Nothing was generated, downloaded, packaged or tagged."
    exit 0
fi

# 2. ESP + SEQ, always regenerated so a zip can never carry a stale plugin.
python3 MFO_GenerateESP.py out >/dev/null
python3 tools/audit_esp.py          # PASS is a merge gate; a FAIL stops the release
python3 tools/audit_mcm.py          # #55 gate: every MCM toggle wired in all 5 places; FAIL stops the release
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

# 3. DLL from the CI run chosen and verified in 1b, provenance recorded below.
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
if ! $GH run download "$RUN_ID" -n "$ARTIFACT" -D "$STAGE/dll"; then
    echo "ERROR: could not download artifact ${ARTIFACT} of run ${RUN_ID}." >&2
    exit 1
fi
if [[ ! -f "$STAGE/dll/$DLL_NAME" ]]; then
    echo "ERROR: ${DLL_NAME} is not in artifact ${ARTIFACT} of run ${RUN_ID}." >&2
    exit 1
fi

# 4. Stage in Data/ layout — zip root IS the virtual Data folder, so MO2
#    installs it with zero manual placement.
mkdir -p "$STAGE/pkg/SKSE/Plugins" "$STAGE/pkg/SEQ" \
         "$STAGE/pkg/MCM/Config/MFO" "$STAGE/pkg/MCM/Settings" "$STAGE/pkg/Scripts"
cp out/MFO.esp             "$STAGE/pkg/"
cp out/SEQ/MFO.seq         "$STAGE/pkg/SEQ/"
cp out/SKSE/Plugins/MFO.ini "$STAGE/pkg/SKSE/Plugins/"
cp -r out/SKSE/Plugins/MFO   "$STAGE/pkg/SKSE/Plugins/"   # board fonts + fallback mfo_items.json
cp "$STAGE/dll/MFO.dll"    "$STAGE/pkg/SKSE/Plugins/"
# MCM Helper config + its binding script. WITHOUT BOTH the MCM never appears:
# config.json alone registers the config but renders nothing, and the ESP's
# MFO_MCMQuest attaches MFO_MCM (an MCM_ConfigBase subclass) whose .pex MUST
# ship or the quest silently fails to become an MCM (2026-07-28 root cause --
# every zip before this one shipped neither file).
cp out/MCM/Config/MFO/config.json "$STAGE/pkg/MCM/Config/MFO/"
# THE DEFAULTS FILE -- how MCM Helper REGISTERS every ModSetting at plugin-load
# (its LoadDefaults() reads MCM/Config/<mod>/settings.ini). This is the #55 fix:
# without it, registration depended entirely on the mutable user store, which
# MO2 shadows with a STALE overwrite copy from earlier play -- so a NEW toggle
# added in an update never registered on an existing install and drew as an
# empty/unresponsive checkbox. Config/ is author-shipped and never written to,
# so MO2 can't shadow it; every control now binds on any save, no relaunch.
# (Primary source: SkyUI/Precision/TDM/TrueHUD all ship this; MFO/MEO/MAO were
# the only omitters. audit_mcm.py gates that this stays in sync with config.json.)
cp out/MCM/Config/MFO/settings.ini "$STAGE/pkg/MCM/Config/MFO/"
# Initial MCM Helper USER store -- still shipped for fresh installs so values are
# complete on first launch (defaults file registers; user store overrides).
cp out/MCM/Settings/MFO.ini       "$STAGE/pkg/MCM/Settings/"
# The translation template (i18n, 86e3gmxmg). The engine loads every
# Interface/Translations/*_<LANGUAGE>.txt, and MFO reads its $MFO_ keys from it.
# Stale-template gate: it is generated from native/i18n/Strings_keys.h.
python3 tools/i18n/gen_template.py --check || { echo "ERROR: out/Interface/Translations/MFO_ENGLISH.txt is stale (run tools/i18n/gen_template.py)." >&2; exit 1; }
mkdir -p "$STAGE/pkg/Interface/Translations"
cp out/Interface/Translations/MFO_ENGLISH.txt "$STAGE/pkg/Interface/Translations/"
cp out/Scripts/MFO_MCM.pex        "$STAGE/pkg/Scripts/"
# #21 econ bridge: MFO_Trade.pex MUST ship or MFO_TradeQuest's VMAD references a
# missing script and the bridge is silently dead. Compiled fresh from
# Source/Scripts/MFO_Trade.psc by tools/compile.sh (checked below).
cp out/Scripts/MFO_Trade.pex      "$STAGE/pkg/Scripts/"
cp THIRD-PARTY-NOTICES.md  "$STAGE/pkg/"    # ships with every build, INVARIANTS #42a
cp OFL.txt                 "$STAGE/pkg/"    # SIL OFL 1.1 for the board fonts (RC#1); OFL 2 requires it travel
# SYNTHESIS ONBOARDING (#19): the FULL item catalog (mfo_items.json) is load-
# order-specific, so the user generates it with the MFO Synthesis patcher. A
# MINIMAL fallback ships regardless (out/SKSE/Plugins/MFO/mfo_items.json via the
# cp -r above): just the known vanilla creature-weapon exclusions -- giant clubs
# et al. are UN-flagged records the DLL heuristics can't see -- so a no-Synthesis
# install still refuses them (the catalog supplements, never replaces, the
# heuristics). The patcher's own output supersedes it in MO2 priority order.
# Ship the one-import .synth (adds the public git patcher to
# their pipeline) + the how-to, so a downloader can wire it up without hunting.
cp assets/MFO.synth        "$STAGE/pkg/"                          # import in Synthesis -> patcher added
cp installer/README.md     "$STAGE/pkg/MFO-Synthesis-README.md"   # how to build the catalog

ZIP="MFO-v${VER}.zip"
rm -f "$ZIP"
(cd "$STAGE/pkg" && zip -rq "$OLDPWD/$ZIP" .)

mkdir -p "$DEST"
cp "$ZIP" "$DEST/"
{
    echo "MFO v${VER}"
    echo "commit:   $(git rev-parse HEAD)"
    echo "ci run:   $RUN_ID (${RUN_SHA})"
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
echo "REMINDER: update Docs/STATUS.md — bump the version, move this release to"
echo "          'shipped', and refresh field-test status / open issues. Keep the"
echo "          living handoff current or the next session inherits a stale map."
