"""Persisted settings (~/.config/mfo-tune-app/settings.json). Paths only; no secrets."""
import json
import os

CONF_DIR = os.environ.get("MFO_TUNE_CONF") or os.path.expanduser("~/.config/mfo-tune-app")
CONF_FILE = os.path.join(CONF_DIR, "settings.json")

# repo default: the checkout this app lives in (tools/tune_app/mfo_tune/settings.py -> repo root)
_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, "..", "..", ".."))

DEFAULTS = {
    "repo": _REPO,
    "sim_bin": "",               # empty: the Launch / Sim run tabs prompt for it (Browse… or Build…)
    "runs_root": os.path.expanduser("~/mfo-tune-runs"),
    "extra_run_roots": ["/tmp/claude-1000/-mnt-gaming-modlists-Projects-marth-follower-overhaul/a935cdde-4a9b-4a50-983f-339ce7b41062/scratchpad/tune"],   # more directories scanned for run dirs (each dir with progress.log or summary.json)
    "lorerim_tuning": "/mnt/gaming/modlists/LoreRim/mods/MFO/SKSE/Plugins/MFO_Tuning",
    "lorerim_log": "/mnt/gaming/modlists/LoreRim/overwrite/SKSE/Plugins/MFO.log",
    "deck_host": "deck@marthdeck",
    "deck_tuning": "/home/deck/Games/Tuxbornrc1/mods/MFO/SKSE/Plugins/MFO_Tuning",
    "deck_log": "/home/deck/Games/Tuxbornrc1/overwrite/SKSE/Plugins/MFO.log",
    "taskset": "",               # empty = all cores (no taskset)
    "nice": 10,
    "jobs": os.cpu_count() or 4,
    "scratch_files": os.path.expanduser("~/mfo-tune-runs/tuning-files"),
}


def load():
    s = dict(DEFAULTS)
    try:
        with open(CONF_FILE) as f:
            s.update(json.load(f))
    except (OSError, ValueError):
        pass
    return s


def save(s):
    os.makedirs(CONF_DIR, exist_ok=True)
    tmp = CONF_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(s, f, indent=1)
    os.replace(tmp, CONF_FILE)
