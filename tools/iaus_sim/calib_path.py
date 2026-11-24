#!/usr/bin/env python3
"""Field rates behind the odd-pathing numbers of sim v4 (Docs/research/2026-11/iaus-sim-v4.md 2.5, 2.6).

    python3 tools/iaus_sim/calib_path.py <run-dir> [<run-dir> ...]      # folders of saved field logs (MFO.log, APMF.log), never in git

Counts, per session and per follower-minute, the log lines each metric is calibrated on, and prints them beside the metric's name so
they can be read against `sim --world v4 --path-report N`. HONEST LIMIT: no log line prints a position, so these are RATES to check
the lab's range against, not constants to fit. The dev-build dump (8.6) is what turns a field case into a replay.

To add a line: one row in ROWS (metric, what it shows, the regular expression). Nothing else changes."""
import os, re, sys

ROWS = [
    ('stuck%',          'stall-probe lines (moved u in s)',            r'\[stall-probe\]'),
    ('stuck%',          'travel legs that ended STUCK',                r'\[travel\].*STUCK'),
    ('stuck%',          'loot stuck-at-origin evidence',               r'\[loot\].*stuck'),
    ('reblock/f',       'travel legs BLOCKED at the same spot',        r'\[travel\].*(blocked ends at the same spot|BLOCKED)'),
    ('reblock/f',       'loot GATED records',                          r'\[loot[^\]]*\].*GATED'),
    ('reblock/f',       'Movement Blocked',                            r'Movement Blocked'),
    ('osc/min',         'offense approach CLAIMED',                    r'\[offense-approach\].*CLAIMED'),
    ('osc/min',         'offense approach released',                   r'\[offense-approach\].*(released|RELEASED)'),
    ('osc/min',         'loot road DISPATCH',                          r'\[loot-road\].*DISPATCH'),
    ('osc/min',         'loot road RELEASE',                           r'\[loot-road\].*RELEASE'),
    ('unreach%',        'offense approach ENGINE DROPPED',             r'\[offense-approach\].*ENGINE DROPPED'),
    ('unreach%',        'loot skipped on navdist',                     r'\[lootskip\].*navdist'),
    ('detour',          'lootskip lines with navdist and dist',        r'\[lootskip\].*navdist.*dist'),
    ('holdUnreach.s/f', 'own-ray OCCLUDED verdicts',                   r'\[los\].*OCCLUDED'),
    ('holdUnreach.s/f', 'target pins',                                 r'\[target-pin\]'),
    ('cast starts',     'offense claims NEVER FIRED',                  r'NEVER FIRED'),
    ('cast starts',     'no Harbinger caster seat',                    r'has NO Harbinger caster seat'),
]
STAMP = re.compile(r'(\d\d):(\d\d):(\d\d)')
FOLLOWER = re.compile(r'\b(0x)?([0-9A-Fa-f]{8})\b')


def read(run):
    counts = [0] * len(ROWS); first = last = None; who = set(); detour = []
    for root, _, files in os.walk(run):
        for fn in files:
            if not fn.lower().endswith('.log'): continue
            try: fh = open(os.path.join(root, fn), errors='replace')
            except OSError: continue
            with fh:
                for line in fh:
                    m = STAMP.search(line[:40])
                    if m:
                        t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)); first = t if first is None else min(first, t); last = t if last is None else max(last, t)
                    for i, (_, _, rx) in enumerate(ROWS):
                        if re.search(rx, line):
                            counts[i] += 1
                            f = FOLLOWER.search(line)
                            if f: who.add(f.group(2).upper())
                    if '[lootskip]' in line:
                        a = re.search(r'navdist[= ]+([0-9.]+)', line); b = re.search(r'distF?[= ]+([0-9.]+)', line)
                        if a and b and float(b.group(1)) > 50: detour.append(float(a.group(1)) / float(b.group(1)))
    minutes = max(1.0, ((last - first) % 86400) / 60.0) if first is not None else 1.0
    return counts, minutes, detour


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ('-h', '--help'): print(__doc__); return 0
    for run in sys.argv[1:]:
        if not os.path.isdir(run): print('not a folder: %s' % run); return 2
        counts, minutes, detour = read(run)
        print('\nFIELD PATH RATES  %s  (%.0f min of log)' % (run, minutes))
        for (metric, what, _), n in zip(ROWS, counts): print('  %-16s %-44s %6d  %7.2f / min' % (metric, what, n, n / minutes))
        if detour:
            detour.sort(); print('  %-16s %-44s n=%d  mean %.2f  p90 %.2f' % ('detour', 'navdist over dist (lootskip)', len(detour), sum(detour) / len(detour), detour[int(0.9 * (len(detour) - 1))]))
    return 0


if __name__ == '__main__': sys.exit(main())
