#!/usr/bin/env python3
"""iaus_sim/tune_defaults.py -- curate a class's DEFAULT GAMBIT ROWS (card C12) in the lab, with its package.

Table mode only (IAUS off): the rows and the package are one system. For every class: a hill-climb over row sets on
the TRAIN list (reorder, retune a threshold, drop, add or replace a row from the rows the board lets that class pick),
then the found set is scored against today's on every list and two seeds. KEPT only when it does not lose elsewhere.

Rules the search keeps: only vocabulary opcodes (native/Vocabulary.h); the class gate decides what it may pick (Q59);
every kind of action today's defaults hold stays (the class stays itself); power-attack rows do not multiply; at most
--row-cap rows.

  tools/iaus_sim/tune_defaults.py --sim <built sim binary> --train LoreRim --validate Tuxborn,Vanilla [--classes a,b]

Output: tools/iaus_sim/defaults/<class>.json (rows in board terms, evidence, verdict).
"""
import argparse, json, os, re, subprocess, sys

CACHE = os.path.expanduser('~/.cache/mfo-list-balance/')
ROLES = ['None', 'Attack', 'Destruction', 'Fire', 'Frost', 'Shock', 'Area', 'Heal', 'Summon', 'BoundWeapon', 'Ward', 'Armor', 'Paralyze', 'Fear', 'Calm', 'Reanimate', 'TurnUndead']


def run(sim, lst, args):
    return subprocess.run([sim] + ([] if lst == 'builtin' else ['--list', CACHE + lst + '.sim.json']) + args, capture_output=True, text=True).stdout


def score(sim, lst, cls, rows, fights, seed, pkg):
    a = ['--fights', str(fights), '--seed', str(seed), '--mode', 'table', '--opt-class', cls, '--opt', 'pk.power', '--opt-passes', '0', '--no-json']
    if rows: a += ['--rows', cls + '=' + rows]
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
    ap.add_argument('--classes', default=''); ap.add_argument('--fights', type=int, default=16); ap.add_argument('--row-cap', type=int, default=5); ap.add_argument('--evals', type=int, default=400)
    ap.add_argument('--packages', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'packages')); ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'defaults'))
    a = ap.parse_args(); os.makedirs(a.out, exist_ok=True)
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
        today = run(a.sim, 'builtin', ['--print-rows', cls]).strip()
        args = ['--fights', str(a.fights), '--mode', 'table', '--opt-class', cls, '--opt-rows', '--row-cap', str(max(a.row_cap, today.count(';') + 1)), '--row-evals', str(a.evals), '--no-json'] + (['--set', pkg] if pkg else [])
        out = run(a.sim, a.train, args)
        m = re.search(r'^--rows (.*)$', out, re.M)
        if not m: print(cls, 'search failed'); continue
        found = m.group(1).strip(); ev = {}; keep = found != today
        for lst in lists:
            b = sum(score(a.sim, lst, cls, today, a.fights * 2, s, pkg) for s in (1, 7)) / 2.0
            n = sum(score(a.sim, lst, cls, found, a.fights * 2, s, pkg) for s in (1, 7)) / 2.0
            ev[lst] = dict(today=round(b, 2), lab=round(n, 2), delta=round(n - b, 2))
            if n - b < -0.75: keep = False
        gain = sum(v['delta'] for v in ev.values()) / len(lists)
        verdict = 'keep' if keep and gain > 0.75 else 'no robust gain: keep today\'s rows'
        rec = dict(schema=1, id='defaults-%s-v1' % cls, **{'class': cls}, train=a.train, validate=lists[1:], mode='table', package=pkg or 'current',
                   today=board(today), lab=board(found), today_text=today, lab_text=found, evidence=ev, mean_gain=round(gain, 2), verdict=verdict,
                   rules='vocabulary opcodes only; class gate decides the picks (Q59); every action kind of today\'s rows kept; power-attack rows do not multiply; row cap %d' % max(a.row_cap, today.count(';') + 1))
        json.dump(rec, open(os.path.join(a.out, cls + '.json'), 'w'), indent=1)
        print('%-18s gain %+.2f  %s\n    today: %s\n    lab:   %s' % (cls, gain, verdict, today, found)); sys.stdout.flush()


if __name__ == '__main__':
    main()
