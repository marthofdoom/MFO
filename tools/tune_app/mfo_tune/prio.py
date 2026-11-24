"""The priority levels of a tune round, from tools/iaus_sim/live/priority.py (the ONE table: edit LEVELS there, never here). Loaded by path from the
repo setting, so the app and the round runner always read the same table. marth 2026-11-23: "idle priority should be the default. But there
should be a drop down to change the current priority"."""
import importlib.util
import os


def live_dir(settings):
    return os.path.join(settings["repo"], "tools", "iaus_sim", "live")


def run_round_path(settings):
    return os.path.join(live_dir(settings), "run_round.py")


def load(settings):
    """-> the priority module, or raises ImportError / OSError with a plain reason (never swallowed: the caller shows it)."""
    p = os.path.join(live_dir(settings), "priority.py")
    spec = importlib.util.spec_from_file_location("mfo_live_priority", p)
    if spec is None or spec.loader is None:
        raise ImportError("cannot load " + p)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def can_change(st):
    """(ok, plain reason when not) for a priority.get() result."""
    if not st["unit"]:
        return False, "this run has no priority unit (it was not started by run_round.py)"
    if st["unit"] == "none":
        return False, "this run was started without systemd (nice 19): live change is off"
    if not st["active"]:
        return False, "the run is not running"
    return True, ""
