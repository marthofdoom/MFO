#!/usr/bin/env bash
# Cut an immutable, exe-free (Synthesis-only) MFO release.
#
# The Nexus download ships NO binary tool — the install-time patcher is a
# Synthesis patcher added from GitHub (see releases/*/MFO.synth), so the
# release is: the mod zip (DLL + ESP + SEQ + MCM + runtime assets) plus the
# static MFO.synth onboarding file. The DLL is pulled from a green `native` CI
# run (CommonLibSSE-NG only builds on Windows/MSVC — there is no local MSVC by
# design, so CI is the only compiler that ever sees this code).
#
# Usage: tools/release_synthesis.sh <version> "desc" [--run <native-run-id>]
#        e.g. tools/release_synthesis.sh v0.9.0 "first Synthesis-only cut"
set -euo pipefail
cd "$(dirname "$0")/.."

GH="${GH:-$HOME/.local/bin/gh}"

VER="${1:?usage: tools/release_synthesis.sh <version> [description] [--run <id>]}"; shift
DESC=""; RUN_ID=""
if [[ $# -gt 0 && "$1" != --* ]]; then DESC="$1"; shift; fi
while [[ $# -gt 0 ]]; do
    case "$1" in
        --run) RUN_ID="${2:?--run needs an id}"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

DEST="releases/$VER"; ZIP="$DEST/MFO-$VER.zip"
[[ -e "$DEST" ]] && { echo "ERROR: $DEST already exists. Releases are immutable; bump or clear it." >&2; exit 1; }

# Resolve the CI run that built the DLL.
if [[ -z "$RUN_ID" ]]; then
    RUN_ID=$("$GH" run list --workflow=native.yml --status success --limit 1 --json databaseId -q '.[0].databaseId')
    [[ -n "$RUN_ID" ]] || { echo "ERROR: no successful 'native' run found." >&2; exit 1; }
fi
echo "== native DLL from run $RUN_ID =="

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/SKSE/Plugins" "$STAGE/SEQ" \
         "$STAGE/MCM/Config/MFO" "$STAGE/MCM/Settings" "$STAGE/Scripts"
"$GH" run download "$RUN_ID" -n MFO-dll -D "$STAGE/SKSE/Plugins"
[[ -f "$STAGE/SKSE/Plugins/MFO.dll" ]] || { echo "ERROR: artifact had no MFO.dll" >&2; exit 1; }
echo "DLL: $(stat -c '%s bytes' "$STAGE/SKSE/Plugins/MFO.dll")"

# Regenerate all DLL-adjacent content fresh (complete standalone mod every
# time), gated by the ESP audit — a FAIL stops the release.
echo "== regenerate ESP + SEQ =="
python3 MFO_GenerateESP.py out >/dev/null
python3 tools/audit_esp.py

cp out/MFO.esp              "$STAGE/"
cp out/SEQ/MFO.seq          "$STAGE/SEQ/"
cp out/SKSE/Plugins/MFO.ini "$STAGE/SKSE/Plugins/"
cp -r out/SKSE/Plugins/MFO  "$STAGE/SKSE/Plugins/"    # baked board fonts (MEO parity)
# MCM Helper config + its binding script + initial settings store. All THREE or
# the MCM never appears / every control reads -1 (2026-07-28 root cause).
cp out/MCM/Config/MFO/config.json "$STAGE/MCM/Config/MFO/"
cp out/MCM/Settings/MFO.ini       "$STAGE/MCM/Settings/"
cp out/Scripts/MFO_MCM.pex        "$STAGE/Scripts/"
cp THIRD-PARTY-NOTICES.md   "$STAGE/"    # ships with every build, INVARIANTS #42a

mkdir -p "$STAGE/fomod"
cat > "$STAGE/fomod/info.xml" <<EOF
<fomod>
  <Name>marth's Follower Overhaul</Name>
  <Author>marth</Author>
  <Version>$VER</Version>
  <Website>https://github.com/marthofdoom/MFO</Website>
  <Description>Follower gambits: loot, supply, and behavior. Build the item catalog for your load order with the MFO Synthesis patcher (see MFO.synth / installer/README.md).</Description>
</fomod>
EOF

# Completeness gate: refuse an incomplete release.
for req in "SKSE/Plugins/MFO.dll" "SKSE/Plugins/MFO.ini" \
           "SKSE/Plugins/MFO/fonts/head.ttf" "SKSE/Plugins/MFO/fonts/body.ttf" \
           "MFO.esp" "SEQ/MFO.seq" \
           "MCM/Config/MFO/config.json" "MCM/Settings/MFO.ini" \
           "Scripts/MFO_MCM.pex" "fomod/info.xml" "THIRD-PARTY-NOTICES.md"; do
    [[ -f "$STAGE/$req" ]] || { echo "ERROR: release incomplete — missing $req" >&2; exit 1; }
done

mkdir -p "$DEST"
( cd "$STAGE" && zip -qr - . ) > "$ZIP"
cp assets/MFO.synth "$DEST/MFO.synth"          # static Synthesis onboarding file
printf '%s\n' "$VER" > "$DEST/VERSION"
{
    [[ -n "$DESC" ]] && printf '%s\n' "$DESC"
    printf 'built from native run %s\n' "$RUN_ID"
    printf 'dll: %s\n' "$(sha256sum "$STAGE/SKSE/Plugins/MFO.dll" | cut -d' ' -f1)"
    printf 'esp: %s\n' "$(sha256sum "$STAGE/MFO.esp" | cut -d' ' -f1)"
} > "$DEST/NOTES.txt"

echo "== manifest (MO2 installs this as Data/) =="; unzip -l "$ZIP"
echo; echo "Wrote $ZIP  +  $DEST/MFO.synth"
echo "Tag with:  git tag -a $VER -m \"${DESC:-$VER}\" && git push origin $VER"
