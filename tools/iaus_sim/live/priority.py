#!/usr/bin/env python3
"""iaus_sim/live/priority.py -- the priority of a tune round, readable and CHANGEABLE WHILE IT RUNS (marth 2026-11-23: "idle priority
should be the default. But there should be a drop down to change the current priority").

usage: priority.py <out>            print the round's level, its unit and what the unit has now
       priority.py <out> <level>    set it: remembered in <out>/priority, applied to the running unit at once
       priority.py --levels         print the level table

A round (run_round.py) runs the driver and every simulator it starts inside ONE systemd user scope. The level is two cgroup weights of
that scope, so a change reaches every process of the round, live, with no restart. Nothing here sets nice, chrt or ionice on a process.

TO ADD OR CHANGE A LEVEL, EDIT `LEVELS` BELOW. It is the only table: run_round.py launches with it, the tune app's drop-down lists it
(import this module: LEVELS, DEFAULT, get(out), set_level(out, level)), and this command line reads it.
"""
import json
import os
import shutil
import subprocess
import sys

# level -> (CPUWeight, IOWeight). CPUWeight "idle" is the kernel's SCHED_IDLE for the whole cgroup (cpu.idle = 1): the round runs only on
# CPU time nothing else wants. 100 is systemd's default weight of every other unit; IOWeight 1 is the lowest.
LEVELS = {
    "idle":   ("idle", 1),
    "low":    (20, 20),
    "normal": (100, 100),
    "high":   (400, 400),
}
DEFAULT = "idle"
ORDER = ["idle", "low", "normal", "high"]   # the drop-down's order


def props(level):
    """The systemd properties of a level, as `-p` values: ["CPUWeight=idle", "IOWeight=1"]."""
    if level not in LEVELS:
        raise ValueError("unknown priority level %r (levels: %s)" % (level, ", ".join(ORDER)))
    cpu, io = LEVELS[level]
    return ["CPUWeight=%s" % cpu, "IOWeight=%s" % io]


def have_systemd():
    """True when a systemd user manager answers (the scope road). False: the nice 19 fallback, with NO live change."""
    if not shutil.which("systemd-run") or not shutil.which("systemctl"):
        return False
    try:
        return subprocess.run(["systemctl", "--user", "show-environment"], capture_output=True, timeout=10).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def read_file(out, name):
    try:
        return open(os.path.join(out, name)).read().strip()
    except OSError:
        return ""


def unit_of(out):
    """The round's unit name (<out>/unit), "" when the round never started, "none" when it ran without systemd (nice fallback)."""
    return read_file(out, "unit")


def unit_active(unit):
    if not unit or unit == "none":
        return False
    try:
        r = subprocess.run(["systemctl", "--user", "is-active", unit], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return False
    return r.stdout.strip() in ("active", "activating")


def live_weights(unit):
    """What the running unit has now: {"CPUWeight": "idle" | "<n>", "IOWeight": "<n>"} ({} when it is not running)."""
    if not unit_active(unit):
        return {}
    r = subprocess.run(["systemctl", "--user", "show", unit, "-p", "CPUWeight", "-p", "IOWeight"], capture_output=True, text=True, timeout=10)
    out = dict(line.split("=", 1) for line in r.stdout.splitlines() if "=" in line)
    if out.get("CPUWeight") == "0":   # systemd prints the idle weight as 0
        out["CPUWeight"] = "idle"
    return out


def get(out):
    """{"level", "unit", "active", "live", "can_change"}: the remembered level (DEFAULT when none was ever set), and the unit's state."""
    level = read_file(out, "priority")
    unit = unit_of(out)
    active = unit_active(unit)
    return {"level": level if level in LEVELS else DEFAULT, "unit": unit, "active": active, "live": live_weights(unit) if active else {},
            "can_change": unit != "none"}


def set_level(out, level):
    """Remember the level in <out>/priority (the next start or resume uses it) and apply it to the running unit. -> get(out) plus
    "applied": True when a running unit took it. Raises ValueError on an unknown level, RuntimeError when systemd refuses."""
    p = props(level)
    os.makedirs(out, exist_ok=True)
    tmp = os.path.join(out, "priority.tmp")
    with open(tmp, "w") as f:
        f.write(level + "\n")
    os.replace(tmp, os.path.join(out, "priority"))
    unit, applied = unit_of(out), False
    if unit_active(unit):
        r = subprocess.run(["systemctl", "--user", "set-property", "--runtime", unit] + p, capture_output=True, text=True, timeout=20)
        if r.returncode != 0:
            raise RuntimeError("systemctl set-property %s %s failed: %s" % (unit, " ".join(p), (r.stderr or r.stdout).strip()))
        applied = True
    st = get(out)
    st["applied"] = applied
    return st


def main(argv):
    if len(argv) == 2 and argv[1] == "--levels":
        for name in ORDER:
            print("%-7s CPUWeight=%s IOWeight=%s%s" % (name, LEVELS[name][0], LEVELS[name][1], "   (default)" if name == DEFAULT else ""))
        return 0
    if len(argv) not in (2, 3) or argv[1].startswith("-"):
        print(__doc__)
        return 2
    out = argv[1]
    if len(argv) == 3:
        try:
            st = set_level(out, argv[2])
        except (ValueError, RuntimeError) as e:
            print("priority: %s" % e, file=sys.stderr)
            return 2
        if st["unit"] == "none":
            print("priority: LIVE CHANGE IS OFF for this round: it runs without systemd (nice 19). The level is remembered for the next start.", file=sys.stderr)
    else:
        st = get(out)
    print(json.dumps(st))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
