#!/usr/bin/env python3
"""iaus_sim/live/watch.py -- what a tune round is doing now. READ-ONLY: it never starts, stops or changes a round.

usage: watch.py <round dir> [--interval S (default 30)] [--once] [--stall-min M (default 45)]

One block per look: the round's state and unit, its priority level and what the unit has now, the stage it is in, per class how many
knobs were judged and how many values were accepted in its last search, the last progress lines, how many results the cache holds, and
how long ago the log last moved. STALLED is printed when the unit is running and the log has not moved for --stall-min minutes (a long
oracle pass at idle priority on a busy machine can be quiet that long: it is a prompt to look, not a verdict). It ends when the round
is done, failed, refused or stopped, with exit code 0 / 1 / 2 / 3.
"""
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import priority as P  # noqa: E402


def read_json(p):
    try:
        return json.load(open(p))
    except (OSError, ValueError):
        return {}


def look(out, stall_min):
    st = read_json(os.path.join(out, "round-status.json"))
    pr = P.get(out)
    lines = []
    logp = os.path.join(out, "progress.log")
    log = open(logp, errors="replace").read().splitlines() if os.path.exists(logp) else []
    age = time.time() - os.path.getmtime(logp) if log else None
    state = st.get("state", "never started")
    if state == "running" and not pr["active"] and pr["unit"] != "none":
        state = "running per round-status.json, but its unit is gone (killed without a signal to run_round.py?): resume with the same command"
    lines.append("ROUND %s  state: %s  unit: %s  priority: %s%s" % (out, state, pr["unit"] or "-", pr["level"],
                 ("  live: CPUWeight=%s IOWeight=%s" % (pr["live"].get("CPUWeight", "?"), pr["live"].get("IOWeight", "?"))) if pr["live"] else "  (LIVE CHANGE OFF: no systemd)" if pr["unit"] == "none" else ""))
    start = max([i for i, ln in enumerate(log) if "  START" in ln] or [0])
    seg = log[start:]
    stage_done = [m.group(1) for ln in seg for m in [re.search(r"  STAGE (\S+) done", ln)] if m]
    cur = next((m.group(1).rstrip(":") for ln in reversed(seg) for m in [re.match(r"^\S+ \S+  (\S+) ", ln)] if m and m.group(1) not in ("STAGE", "START", "DONE:")), "-")
    lines.append("since the last START: stages done: %s; working on: %s" % (", ".join(stage_done) or "none", "finished" if any("  DONE:" in ln for ln in seg) else cur))
    cls = {}
    for ln in seg:
        m = re.search(r"  classes (\S+): (\d+) knobs, (\d+) live", ln)
        if m:
            cls[m.group(1)] = {"knobs": int(m.group(2)), "live": int(m.group(3)), "judged": 0, "accepted": 0}
            continue
        m = re.search(r"  classes (\S+) pass \d+ ", ln)
        if m and m.group(1) in cls:
            cls[m.group(1)]["judged"] += 1
            cls[m.group(1)]["accepted"] += "  ACCEPTED" in ln
    if cls:
        lines.append("classes: " + "  ".join("%s %d/%d live, %d accepted" % (c, v["judged"], v["live"], v["accepted"]) for c, v in cls.items()))
    cp = os.path.join(out, "cache.jsonl")
    if os.path.exists(cp):
        lines.append("cache: %.1f MB of finished results" % (os.path.getsize(cp) / 1e6))
    if age is not None:
        lines.append("progress.log last moved %.0f min ago%s" % (age / 60.0, "   STALLED? (no line for over %d min while the unit runs)" % stall_min if pr["active"] and age > stall_min * 60 else ""))
    lines += ["  | " + ln[:200] for ln in log[-4:]]
    code = {"done": 0, "failed": 1, "refused": 2, "stopped": 3}.get(st.get("state"))
    return "\n".join(lines), code


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__)
        return 2
    opt = lambda name, d: float(argv[argv.index(name) + 1]) if name in argv else d
    interval, stall = opt("--interval", 30.0), opt("--stall-min", 45.0)
    while True:
        text, code = look(args[0], stall)
        print(time.strftime("%H:%M:%S ") + text, flush=True)
        if "--once" in argv or code is not None:
            return code or 0
        time.sleep(interval)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
