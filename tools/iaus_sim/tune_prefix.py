#!/usr/bin/env python3
"""iaus_sim/tune_prefix.py -- ORDER a class's default gambit rows so every prefix is a good table (ruling Q74).

marth 2026-11-11: Rank I has 3 combat slots and the class default sets have 4-5 rows. The follower gets the TOP rows in
priority order and the rest join in the same order as slots unlock with rapport. So the order must make the 3-row
prefix, the 4-row prefix and the full set each a good table on its own.

Table mode only. For every class with more than 3 rows: every permutation of its recommended set (the lab set of
defaults/<class>.json when its verdict is keep, else today's rows) is scored on the TRAIN list as the mean of J over the
prefixes 3 .. n; the best order is then scored against the starting order on every list and two seeds, per prefix.
KEPT only when no list loses more than the noise and the mean gains.

  tools/iaus_sim/tune_prefix.py --sim <built sim binary> --train LoreRim --validate Tuxborn,Vanilla [--classes a,b]

Output: the "prefix" block of tools/iaus_sim/defaults/<class>.json (order, per-prefix evidence, verdict).
"""
import argparse, json, os, re, subprocess, sys

CACHE = os.path.expanduser('~/.cache/mfo-list-balance/')
ROLES = ['None', 'Attack', 'Destruction', 'Fire', 'Frost', 'Shock', 'Area', 'Heal', 'Summon', 'BoundWeapon', 'Ward', 'Armor', 'Paralyze', 'Fear', 'Calm', 'Reanimate', 'TurnUndead']


def run(sim, lst, args):
    return subprocess.run([sim] + ([] if lst == 'builtin' else ['--list', CACHE + lst + '.sim.json']) + args, capture_output=True, text=True).stdout


def score(sim, lst, cls, rows, k, fights, seed, pkg):
    a = ['--fights', str(fights), '--seed', str(seed), '--mode', 'table', '--opt-class', cls, '--opt', 'pk.power', '--opt-passes', '0', '--no-json', '--row-prefix', str(k), '--rows', cls + '=' + rows]
    if pkg: a += ['--set', pkg]
    m = re.search(r'start: J=([\d.\-]+)', run(sim, lst, a))
    return float(m.group(1)) if m else float('nan')


def board(rows):
    out = []
    for r in rows.split(';'):
        f = r.split('|')
        out.append(dict(condition=f[0], gambit=f[1], threshold=float(f[2]), spell_role=ROLES[int(f[3])] if int(f[3]) < len(ROLES) else f[3], cooldown_s=float(f[4])))
    return out


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--sim', required=True); ap.add_argument('--train', default='LoreRim'); ap.add_argument('--validate', default='Tuxborn,Vanilla')
    ap.add_argument('--classes', default=''); ap.add_argument('--fights', type=int, default=40)
    ap.add_argument('--packages', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'packages')); ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'defaults'))
    a = ap.parse_args()
    table = run(a.sim, 'builtin', ['--print-classes'])
    classes = [m.group(2) for m in re.finditer(r'^\s+T(\d) (\S+)\s+(melee|ranged|mage)', table, re.M)]
    want = [c for c in a.classes.split(',') if c] or classes
    lists = [a.train] + [v for v in a.validate.split(',') if v]
    for cls in want:
        pkg = ''
        pj = os.path.join(a.packages, cls + '.json')
        if os.path.exists(pj):
            rec = json.load(open(pj))
            if rec.get('verdict') == 'keep': pkg = ','.join('%s=%g' % kv for kv in rec['found'].items())
        dj = os.path.join(a.out, cls + '.json')
        drec = json.load(open(dj)) if os.path.exists(dj) else {}
        start = drec.get('lab_text') if drec.get('verdict') == 'keep' else drec.get('today_text')
        if not start: start = run(a.sim, 'builtin', ['--print-rows', cls]).strip()
        n = start.count(';') + 1
        if n <= 3:
            print('%-18s %d rows: every rank holds the whole set' % (cls, n)); continue
        out = run(a.sim, a.train, ['--fights', str(a.fights), '--mode', 'table', '--opt-class', cls, '--opt-prefix', '--no-json', '--rows', cls + '=' + start] + (['--set', pkg] if pkg else []))
        m = re.search(r'^--rows (.*)$', out, re.M)
        if not m: print(cls, 'search failed'); continue
        found = m.group(1).strip(); ev = {}; keep = found != start; gains = []
        for lst in lists:
            ev[lst] = {}
            for k in range(3, n + 1):
                b = sum(score(a.sim, lst, cls, start, k, a.fights, s, pkg) for s in (1, 7)) / 2.0
                f = sum(score(a.sim, lst, cls, found, k, a.fights, s, pkg) for s in (1, 7)) / 2.0
                ev[lst]['prefix%d' % k] = dict(start=round(b, 2), ordered=round(f, 2), delta=round(f - b, 2))
            d = sum(v['delta'] for v in ev[lst].values()) / len(ev[lst]); gains.append(d)
            if d < -0.75: keep = False
        gain = sum(gains) / len(gains)
        verdict = 'keep' if keep and gain > 0.5 else 'no robust gain: keep the starting order'
        drec['prefix'] = dict(ruling='Q74', start_text=start, ordered_text=found, ordered=board(found), evidence=ev, mean_gain=round(gain, 2), verdict=verdict,
                              slots_by_rank=[3, 5, 7, 9, 12], method='all permutations on %s, mean J over prefixes 3..%d, %d fights; validated on %s, seeds 1 and 7' % (a.train, n, a.fights, ', '.join(lists)))
        if 'class' not in drec: drec['class'] = cls
        json.dump(drec, open(dj, 'w'), indent=1)
        print('%-18s gain %+.2f  %s\n    start:   %s\n    ordered: %s' % (cls, gain, verdict, start, found)); sys.stdout.flush()


if __name__ == '__main__':
    main()
