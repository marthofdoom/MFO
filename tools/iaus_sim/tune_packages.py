#!/usr/bin/env python3
"""iaus_sim/tune_packages.py -- search a class combat package (card C8) in the lab and validate it across lists.

For every class: a coordinate search over the package (stance styles, ch.18 power share, ch.23 leash) in TABLE mode
(IAUS off, the class's default gambit rows), on the TRAIN list; then the found values are scored on every list in
table mode and in IAUS mode, against today's package. A value is KEPT only when it does not lose on the other lists.

  tools/iaus_sim/tune_packages.py --sim <built sim binary> --train LoreRim --validate Tuxborn,Vanilla [--classes a,b] [--fights 16]

Output: tools/iaus_sim/packages/<class>.json (values, evidence per list and mode, the robustness verdict).
The CSTY fields move behaviour through the sim's engine-AI MODEL (doc section 4 row 22): the DIRECTION of a finding is
what the lab can stand behind; the numbers need the in-game probe before they become generator rows.
"""
import argparse, json, os, re, subprocess, sys

CACHE = os.path.expanduser('~/.cache/mfo-list-balance/')
# today's package (MFO_GenerateESP.py make_csty; class offsets are applied by the sim): the search starts here
BASE = {'st.0.off': 1.0, 'st.0.def': 0.5, 'st.0.fallback': 0.42, 'st.0.powBlock': 1.0, 'st.0.powStag': 1.0, 'st.0.bash': 1.0,
        'st.1.off': 1.0, 'st.1.def': 0.5, 'st.1.fallback': 0.65, 'st.1.avoid': 0.5,
        'st.2.off': 1.0, 'st.2.def': 0.5, 'st.2.fallback': 0.50, 'st.2.avoid': 0.2,
        'pk.power': 1.0, 'pk.leash': 0.0}
STANCES = {'Melee': ('st.0.', 'pk.'), 'Ranged': ('st.1.', 'st.0.', 'pk.'), 'Mage': ('st.2.', 'st.0.', 'pk.'), 'Hybrid': ('st.0.', 'st.1.', 'st.2.', 'pk.')}


def run(sim, lst, args):
    cmd = [sim] + ([] if lst == 'builtin' else ['--list', CACHE + lst + '.sim.json']) + args
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def score(sim, lst, cls, sets, mode, fights, seed):
    out = run(sim, lst, ['--fights', str(fights), '--seed', str(seed), '--mode', mode, '--opt-class', cls, '--opt', 'pk.power', '--opt-passes', '0', '--no-json'] + (['--set', sets] if sets else []))
    m = re.search(r'start: J=([\d.\-]+)', out)
    return float(m.group(1)) if m else float('nan')


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--sim', required=True); ap.add_argument('--train', default='LoreRim'); ap.add_argument('--validate', default='Tuxborn,Vanilla')
    ap.add_argument('--classes', default=''); ap.add_argument('--fights', type=int, default=16); ap.add_argument('--passes', type=int, default=2); ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'packages'))
    a = ap.parse_args(); os.makedirs(a.out, exist_ok=True)
    table = run(a.sim, 'builtin', ['--print-classes'])
    classes = [(m.group(2), int(m.group(1)), m.group(3)) for m in re.finditer(r'^\s+T(\d) (\S+)\s+(melee|ranged|mage)', table, re.M)]
    lines = {}   # class -> line (the print shows the family; hybrids read as their root)
    for name, tier, fam in classes: lines[name] = {'melee': 'Melee', 'ranged': 'Ranged', 'mage': 'Mage'}[fam]
    want = [c for c in a.classes.split(',') if c] or [c[0] for c in classes]
    lists = [a.train] + [v for v in a.validate.split(',') if v]
    for cls in want:
        pref = STANCES[lines.get(cls, 'Melee')]
        params = [k for k in BASE if k.startswith(pref)]
        start = ','.join('%s=%g' % (k, BASE[k]) for k in params)
        out = run(a.sim, a.train, ['--fights', str(a.fights), '--mode', 'table', '--opt-class', cls, '--set', start, '--opt', ','.join(params), '--opt-passes', str(a.passes), '--no-json'])
        m = re.search(r'^--set (.*)$', out, re.M)
        if not m: print(cls, 'search failed'); continue
        found = dict((kv.split('=')[0], float(kv.split('=')[1])) for kv in m.group(1).split(','))
        changed = {k: v for k, v in found.items() if abs(v - BASE[k]) > 1e-6}
        sets = ','.join('%s=%g' % (k, v) for k, v in found.items())
        ev = {}; keep = True
        for lst in lists:
            for mode in ('table', 'iaus'):
                b = sum(score(a.sim, lst, cls, start, mode, a.fights * 2, s) for s in (1, 7)) / 2.0
                n = sum(score(a.sim, lst, cls, sets, mode, a.fights * 2, s) for s in (1, 7)) / 2.0
                ev['%s/%s' % (lst, mode)] = dict(today=round(b, 2), package=round(n, 2), delta=round(n - b, 2))
                if n - b < -0.75: keep = False
        gain = sum(v['delta'] for k, v in ev.items() if k.endswith('/table')) / len(lists)
        verdict = 'keep' if keep and gain > 0.5 and changed else 'no robust gain: keep today\'s package'
        rec = dict(schema=1, id='pkg-%s-v1' % cls, **{'class': cls}, line=lines.get(cls), train=a.train, validate=lists[1:], objective='0.35 x class scenarios + 0.65 x (natural, balanced, strong1) mean utility, table mode',
                   today=BASE, found=found, changed=changed, evidence=ev, mean_table_gain=round(gain, 2), verdict=verdict,
                   proof='MODEL: the CSTY fields act through the sim engine-AI model (doc section 4 row 22). Direction is the finding; numbers need the in-game probe.')
        json.dump(rec, open(os.path.join(a.out, cls + '.json'), 'w'), indent=1)
        print('%-18s gain(table) %+.2f  %s  changed: %s' % (cls, gain, verdict, changed)); sys.stdout.flush()


if __name__ == '__main__':
    main()
