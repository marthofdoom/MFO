"""The priority levels of a tune round, from tools/iaus_sim/live/priority.py (the ONE table: edit LEVELS there, never here). Loaded by path from the
repo setting, so the app and the round runner always read the same table. marth 2026-11-23: "idle priority should be the default. But there
should be a drop down to change the current priority"."""
import fcntl
import hashlib
import importlib.util
import json
import os

_MODS = {}   # (path, mtime) -> loaded module: the table is read on every refresh, not re-executed on every refresh


def live_dir(settings):
    return os.path.join(settings["repo"], "tools", "iaus_sim", "live")


def run_round_path(settings):
    return os.path.join(live_dir(settings), "run_round.py")


def load(settings):
    """-> the priority module, or raises ImportError / OSError with a plain reason (never swallowed: the caller shows it)."""
    return _load_path(os.path.join(live_dir(settings), "priority.py"), "mfo_live_priority")


def _load_path(p, name):
    key = (p, os.path.getmtime(p))
    if key not in _MODS:
        spec = importlib.util.spec_from_file_location(name, p)
        if spec is None or spec.loader is None:
            raise ImportError("cannot load " + p)
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        _MODS[key] = m
    return _MODS[key]


def unit_name(settings, out):
    """The unit run_round.py would give this out dir (its own unit_name(), so the two never drift). run_round.py has no import-time side effects."""
    import sys
    live = live_dir(settings)
    if live not in sys.path:
        sys.path.insert(0, live)             # run_round.py does `import priority`
    return _load_path(run_round_path(settings), "mfo_live_run_round").unit_name(out)


def can_change(st, expected_unit=None):
    """(ok, plain reason when not) for a priority.get() result. expected_unit = run_round.unit_name(out): the unit recorded in <out>/unit must be it,
    or the file belongs to some other scope and a live change would touch a process that is not this round."""
    if not st["unit"]:
        return False, "this run has no priority unit (it was not started by run_round.py)"
    if st["unit"] == "none":
        return False, "this run was started without systemd (nice 19): live change is off"
    if expected_unit and st["unit"] != expected_unit:
        return False, "the unit recorded for this run (%s) is not the unit run_round.py names for it (%s): live change is off" % (st["unit"], expected_unit)
    if not st["active"]:
        return False, "the run is not running"
    return True, ""


def live_level(pm, st):
    """The level whose weights the running unit has now ('' when none matches or the unit is not running)."""
    lw = st.get("live") or {}
    for name in pm.ORDER:
        cpu, io = pm.LEVELS[name]
        if str(lw.get("CPUWeight")) == str(cpu) and str(lw.get("IOWeight")) == str(io):
            return name
    return ""


def read_file_level(out):
    """The level remembered in <out>/priority ('' when none)."""
    try:
        with open(os.path.join(out, "priority")) as f:
            return f.read().strip()
    except OSError:
        return ""


def read_json(out, name):
    try:
        with open(os.path.join(out, name)) as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def bin_sha(path):
    """run_round.py's bin_sha: sha256 of the binary, first 16 hex digits."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:16]


def resume_conflicts(out, drv_args, binary):
    """What run_round.py would REFUSE on this resume: -> (args_msg, bin_msg), '' each when fine. Mirrors its checks on <out>/round.json
    (driver_args compared as a list, bin_sha as the 16-digit prefix)."""
    rec = read_json(out, "round.json")
    am = bm = ""
    old = rec.get("driver_args")
    if old is not None and old != list(drv_args):
        am = "this run dir was started with other driver arguments.\n  then: %s\n  now:  %s" % (" ".join(old), " ".join(drv_args))
    old_sha = rec.get("bin_sha")
    if old_sha and binary and os.path.isfile(binary):
        now = bin_sha(binary)
        if now != old_sha:
            bm = "this run dir was started with another simulator binary (sha256 %s, now %s); its cached results cannot be reused." % (old_sha, now)
    return am, bm


def lock_held(out):
    """True while a run_round.py holds <out>/round.lock (it holds it from its first line to its exit, building and hashing included)."""
    try:
        fd = os.open(os.path.join(out, "round.lock"), os.O_RDONLY)
    except OSError:
        return False
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
        except OSError:
            return True
        fcntl.flock(fd, fcntl.LOCK_UN)
        return False
    finally:
        os.close(fd)
