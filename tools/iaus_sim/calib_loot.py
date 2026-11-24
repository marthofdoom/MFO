#!/usr/bin/env python3
"""iaus_sim/calib_loot.py -- the field side of the loot gate (sim v4 batch L, iaus-sim-v4.md 14.3).

    python3 tools/iaus_sim/calib_loot.py _evidence/deck-run-1117g [more run dirs ...]

Reads MFO.log of each run and prints, per run, the rates the lab prints under the same names (sim `--loot-report`, LOOTRATE / LOOTGATE lines):
events per follower-minute of looting, and the gate rows a log can show (restart%, aborted%, legs per source). marth's method constraint
(CLAUDE.md field diagnosis 4) holds: nothing here argues from arrival counts or travel / arrival ratios alone. A leg is a DISPATCH or a RETARGET
line (what MFO asked for), an end is the `[loot-road] RELEASE` reason or the `leg ended` state Harbinger reported.

THE DENOMINATOR (follower-minutes): a log does not say when a follower is out of combat. It is estimated as, per follower, the seconds covered
by 10 s windows in which he printed a scan line (`[loot] <id> cat=`, `[lootskip] <id> cat=`, `[iaus-loot] <id>: <kind> TAKE|leave`: each prints every 10 s while he scans).
That is an ESTIMATE of scanning time and is printed with each run. To add a line to the count: one row of ROWS (name, regex).
"""
import os, re, sys
from collections import Counter, defaultdict

TS = re.compile(r'^\[(\d+):(\d+):(\d+)[.:](\d+)\]')
ROWS = [
    ('DISPATCH', re.compile(r'\[loot-road\] ([0-9A-F]+): DISPATCH road=\w+ dest=([0-9A-F]+)')),
    ('RETARGET', re.compile(r'\[loot-road\] ([0-9A-F]+): RETARGET road=\w+ dest=([0-9A-F]+)')),
    ('RELEASE why=', re.compile(r'\[loot-road\] RELEASE road=\w+ slot=\d+ why=(.+?)\s*$')),
    ('leg ended ', re.compile(r'\[loot\] ([0-9A-F]+) leg->([0-9A-F]+): CH19 leg ended ([A-Z\- ]+?) \(')),
    ('GATED', re.compile(r'\[loot\] ([0-9A-F]+) target ([0-9A-F]+) GATED -- Movement Blocked')),
    ('GATED-BEHIND', re.compile(r'\[loot\] ([0-9A-F]+) GATED-BEHIND')),
    ('GATED re-admitted', re.compile(r'\[loot\] GATED ([0-9A-F]+) re-admitted')),
    ('ACTOR-BLOCKED (REORDER)', re.compile(r'\[loot\] ([0-9A-F]+) target ([0-9A-F]+) ACTOR-BLOCKED --')),
    ('bump (first block, stuck evidence only)', re.compile(r'a bump / repath, not gated \(first block')),
    ('REORDER (no ground closed, stuck evidence)', re.compile(r'NO ground closed since his previous block')),
    ('ARRIVED but drifted away (leg restarted)', re.compile(r'Harbinger ARRIVED but he is')),
    ('DEADLINE EXPIRED', re.compile(r'DEADLINE EXPIRED')),
    ('unreachable (no progress)', re.compile(r'\[loot\] ([0-9A-F]+) unreachable ([0-9A-F]+) \(no progress')),
    ('stuck himself', re.compile(r'is stuck himself')),
    ('left the stuck point', re.compile(r'left the stuck point')),
    ('idle: cleared blocklisted bodies', re.compile(r'idle \d+ ticks -- cleared')),
    ('arrived -- looted', re.compile(r': arrived -- looted')),
    ('arrived -- nothing to take', re.compile(r': arrived -- nothing to take')),
    ('arrived: guard sees it (BARRED)', re.compile(r'arrived at [0-9A-F]+ -- guard sees')),
    ('arrived: pending (guard or landing)', re.compile(r'arrived at [0-9A-F]+ -- (guard check pending|a take of this visit is still landing)')),
    ('EMPTIED before arrival', re.compile(r'EMPTIED before arrival')),
    ('blocklisted: ', re.compile(r'blocklisted ~\d+s \(transient skip\) -- (.+?)\s*$')),
]
ANY = re.compile(r'\[(loot|lootskip|iaus-loot)\] ([0-9A-F]{6,8})(?: cat=|: \w+ (?:TAKE|leave) )')   # a follower's own scan lines only (other [loot] lines begin with a ref's id)


def run(path):
    log = os.path.join(path, 'MFO.log') if os.path.isdir(path) else path
    ev = Counter(); windows = defaultdict(set); legs = []; dests = Counter(); arrived = 0; t = 0.0
    for line in open(log, encoding='utf-8', errors='replace'):
        m = TS.match(line)
        if m: t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
        a = ANY.search(line)
        if a: windows[a.group(2)].add(int(t // 10))
        for name, rx in ROWS:
            m = rx.search(line)
            if not m: continue
            if name in ('DISPATCH', 'RETARGET'): legs.append((m.group(1), m.group(2))); ev[name] += 1
            elif name == 'RELEASE why=': ev[name + m.group(1)] += 1
            elif name == 'leg ended ': ev[name + m.group(3).strip()] += 1; arrived += m.group(3).strip() == 'ARRIVED'
            elif name == 'blocklisted: ': ev[name + re.sub(r'\s*\(.*', '', m.group(1))[:48]] += 1
            else: ev[name] += 1
            break
    fmin = sum(len(w) for w in windows.values()) * 10 / 60.0
    seen = Counter(); restarted = 0
    for f, d in legs:
        if seen[(f, d)]: restarted += 1
        seen[(f, d)] += 1
    n = len(legs); looted = ev['arrived -- looted']
    print('FIELD %s  followers %d  follower-min (ESTIMATE, scanning) %.1f  legs %d' % (os.path.basename(path.rstrip('/')), len(windows), fmin, n))
    for k in sorted(ev): print('FIELDRATE %-22s %-58s %7d  %8.3f /follower-min' % (os.path.basename(path.rstrip('/')), k, ev[k], ev[k] / fmin if fmin else 0.0))
    g = [('restart%', 100.0 * restarted / n if n else 0.0), ('aborted%', 100.0 * (n - arrived) / n if n else 0.0),
         ('legs/src', n / looted if looted else 0.0), ('legs/min', n / fmin if fmin else 0.0)]
    for k, v in g: print('FIELDGATE %-22s %-20s %9.3f' % (os.path.basename(path.rstrip('/')), k, v))
    print('  (aborted%: legs asked for minus Harbinger ARRIVED ends. A leg MFO took by its own distance test before Harbinger ended it counts as not arrived here, so the field number is an UPPER bound.)')


if __name__ == '__main__':
    if len(sys.argv) < 2: sys.exit(__doc__)
    for p in sys.argv[1:]: run(p)
