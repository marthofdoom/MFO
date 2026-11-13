#!/usr/bin/env bash
# Installs the KDE menu entry (~/.local/share/applications/mfo-tuning-lab.desktop) pointing at this checkout's run.sh, and prepares the venv.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$APPS"
cat > "$APPS/mfo-tuning-lab.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=MFO Tuning Lab
GenericName=IAUS tuning simulator front end
Comment=Run, watch and deploy IAUS tuning passes
Exec=$HERE/run.sh
Icon=applications-science
Terminal=false
Categories=Development;Science;
StartupWMClass=mfo-tuning-lab
EOF
chmod +x "$HERE/run.sh" "$HERE/main.py"
command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" 2>/dev/null || true
VENV="${MFO_TUNE_VENV:-$HOME/.local/share/mfo-tune-app/venv}"
if ! "$VENV/bin/python" -c "import PySide6" 2>/dev/null; then
    python3 -m venv "$VENV" && "$VENV/bin/pip" install -q -r "$HERE/requirements.txt"
fi
echo "installed: $APPS/mfo-tuning-lab.desktop"
