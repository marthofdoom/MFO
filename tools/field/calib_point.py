#!/usr/bin/env python3
"""Read the G0b hold exerciser's lines out of MFO.log. Usage: calib_point.py MFO.log [...]
One row per leg, then per (test, radius) totals: share inside, arrival, drift, attacks, casts. Nothing is written."""
import re
import sys
from collections import defaultdict

LEG = re.compile(r"\[hold\] RESULT (\S+) leg (\d+)/(\d+) '([^']*)' R=(\d+): (.*?) \| ")
NUM = re.compile(r"start (\d+)u, arrived (-?[\d.]+)s after walking (\d+)u, inside (\d+)% of ([\d.]+)s, escapes (\d+), "
                 r"dist avg (-?\d+) max (\d+) last (\d+), drift (\d+) u/s \| attacks (\d+) shots (\d+) casts (\d+)")


def main(paths):
    rows, other, light = [], [], 0
    for p in paths:
        for line in open(p, encoding="utf-8", errors="replace"):
            if "[probe-light]" in line:
                light += 1
            m = LEG.search(line)
            if not m:
                if "QUIET COMPARE" in line and "[hold] RESULT" in line:
                    other.append(line[line.index("[hold]"):].strip())
                continue
            n = NUM.search(line)
            end = line.rsplit("| end: ", 1)[-1].strip() if n else line.rsplit("| ", 1)[-1].strip()
            rows.append((m, n, end))
    print(f"{'test':24}{'leg':6}{'what':20}{'R':>5}  {'verdict':26}{'arr s':>7}{'in %':>6}{'esc':>5}{'drift':>7}{'atk':>5}{'shot':>5}{'cast':>5}  end")
    tot = defaultdict(list)
    for m, n, end in rows:
        g = n.groups() if n else None
        print(f"{m[1]:24}{m[2] + '/' + m[3]:6}{m[4][:19]:20}{m[5]:>5}  {m[6][:25]:26}" +
              (f"{g[1]:>7}{g[3]:>6}{g[5]:>5}{g[9]:>7}{g[10]:>5}{g[11]:>5}{g[12]:>5}" if g else " " * 40) + f"  {end[:60]}")
        if g and not m[6].startswith("SKIP") and float(g[1]) >= 0:
            tot[(m[1], m[4], int(m[5]))].append([float(x) for x in g])
    print("\nper test and radius (measured legs that arrived):")
    for (t, w, r), v in sorted(tot.items()):
        a = [sum(c) / len(v) for c in zip(*v)]
        print(f"  {t:24}{w[:19]:20} R {r:4}: n={len(v)} inside {a[3]:.0f}%  arrive {a[1]:.1f}s over {a[0]:.0f}u  drift {a[9]:.0f} u/s  atk {a[10]:.1f} shot {a[11]:.1f} cast {a[12]:.1f}")
    for o in other:
        print(o)
    print(f"[probe-light] lines: {light}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
