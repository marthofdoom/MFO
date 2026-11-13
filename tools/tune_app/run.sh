#!/usr/bin/env bash
# MFO Tuning Lab launcher: creates the venv on first start (pinned requirements), then runs the app.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV="${MFO_TUNE_VENV:-$HOME/.local/share/mfo-tune-app/venv}"
if [[ ! -x "$VENV/bin/python" ]] || ! "$VENV/bin/python" -c "import PySide6" 2>/dev/null; then
    echo "first start: creating $VENV and installing $HERE/requirements.txt" >&2
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install -q -r "$HERE/requirements.txt"
fi
exec "$VENV/bin/python" "$HERE/main.py" "$@"
