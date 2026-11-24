#!/usr/bin/env python3
"""list_balance/book_census.py -- COUNT TEST: the lab's IAUS books against native's rule (placed-spells-plan.md L1).

Recomputes iaus/SpellBook.cpp StaticOf for every taught spell of a compiled list straight from its rows, independently of tools/iaus_sim/Data.h
(a second derivation ON PURPOSE: this file is the test, never a source of logic), and compares the verdicts with the simulator's --book-census lines.
Exit 1 when one spell's verdict differs. TEST INPUT ONLY, nothing here is committed data.

usage: book_census.py --sim <iaus_sim binary> --list <name>.sim.json [--names] [--log MFO.log]
  --names  also print the edids per drop reason
  --log    compare with the "[spellbook] ... no IAUS candidate" lines of a deck log (by the local FormID; a local id two plugins share is skipped)
"""
import argparse, collections, json, re, subprocess, sys

SCRIPT, LIGHT, BOUND, SUMMON, REANIM, CLOAK, HAZARD = 1, 12, 17, 18, 22, 35, 40
CONTROL = (6, 7, 8, 21, 24); HEALTH, WARD = 24, 63
NATURE = ['Unknown', 'Offense', 'Control', 'Heal', 'Buff', 'Summon', 'Ward']


def verdict(s):
    fx = s['effects']; p = s['placed']; ar = [e['archetype'] for e in fx]
    self_ = s['delivery'] == 'Self'; tl = s['delivery'] == 'TargetLocation'; conc = s['castType'] == 'Concentration'
    cost = lambda e: e['baseCost'] * (max(e['mag'], 1.0) * max(e['dur'], 1.0)) ** 1.1
    hostile = any(e['hostile'] or e['detrimental'] for e in fx); control = any(a in CONTROL for a in ar)
    kind = 'offense' if hostile or control else 'heal' if any(e['av'] == HEALTH for e in fx) else 'buff'
    ward = any(e['archetype'] in (0, 4, 5, 32, 34) and e['av'] == WARD for e in fx)
    top = max(fx, key=cost)['archetype']
    if SUMMON in ar: shape = 'summon'
    elif REANIM in ar: shape = 'reanimate'
    elif SCRIPT in ar: shape = 'script' if top == SCRIPT else 'rider'
    elif BOUND in ar: shape = 'bound'
    elif CLOAK in ar: shape = 'cloak'
    elif ward: shape = 'ward'
    elif LIGHT in ar: shape = 'light'
    elif HAZARD in ar and self_: shape = 'selfhazard'
    elif p['drops'] and conc and not self_: shape = 'wall'
    elif tl: shape = 'rune' if p['proj'] else 'burst'
    else: shape = 'other'
    if shape == 'reanimate': n = 0
    elif SUMMON in ar: n = 5
    elif shape == 'ward': n = 6
    elif control: n = 2
    elif shape == 'cloak' and self_: n = 4
    elif kind == 'offense': n = 1
    elif kind == 'heal': n = 3
    elif shape == 'script' or (kind == 'buff' and not self_ and all(a == SCRIPT for a in ar)): n = 0
    else: n = 4
    area = any(e['area'] > 0 for e in fx) or p['payload'] > 0
    blast = self_ and not conc and SUMMON not in ar and ((n in (1, 2) and (area or p['impact'])) or p['hazHostile'])
    placed = (tl or shape == 'wall') and SUMMON not in ar   # M1: a summon is kept
    if s['castType'] == 'ConstantEffect': d = 'not castable'
    elif kind == 'buff' and not (conc and not self_) and shape not in ('summon', 'reanimate') and LIGHT in ar and s['delivery'] in ('Aimed', 'TargetLocation'): d = 'unanimated road only'
    elif placed: d = 'target location'
    elif blast: d = 'hostile area around himself: blocked for now (no animated road, no bystander check)'
    elif n == 0: d = 'nature unknown'
    elif not any(0 <= a <= 46 and a not in (1, 43) for a in ar): d = "its effects are script-only or unknown archetypes: IAUS cannot weight what it does"
    else: d = 'keep'
    return NATURE[n], d, shape


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--sim', required=True); ap.add_argument('--list', required=True)
    ap.add_argument('--names', action='store_true'); ap.add_argument('--log'); a = ap.parse_args()
    J = json.load(open(a.list)); spells = {s['key']: s for s in J['spells']}
    out = subprocess.run([a.sim, '--list', a.list, '--book-census', '--classes', 'mage', '--fights', '1', '--rating', '0', '--threads', '4', '--no-json'], capture_output=True, text=True).stdout
    lab = {f[1]: (NATURE[int(f[3])], f[4]) for f in (l.split('\t') for l in out.splitlines() if l.startswith('CENSUS\t'))}
    if not lab: sys.exit('no CENSUS line from the simulator')
    cnt = collections.Counter(); names = collections.defaultdict(list); bad = nat = 0
    for k, (ln, ld) in lab.items():
        n, d, shape = verdict(spells[k]); cnt[(d, n if d == 'keep' else shape if d == 'target location' else '')] += 1
        if d != 'keep': names[d].append(spells[k]['edid'])
        if d != ld: bad += 1; print('VERDICT DIFFERS', spells[k]['edid'], 'native-rule', n, d, 'lab', ln, ld)
        elif n != ln and d == 'keep': nat += 1; print('nature differs', spells[k]['edid'], 'native-rule', n, 'lab', ln)
    for k, v in sorted(cnt.items()): print('  %-5d %s %s' % (v, *k))
    if a.names:
        for d in names: print(d, sorted(names[d]))
    print('book census %s: %d taught, %d verdict(s) differ, %d nature(s) differ' % (J['list'], len(lab), bad, nat))
    if a.log:
        loc = collections.defaultdict(list)
        for k in lab: loc[int(k.split(':')[1], 16)].append(k)
        seen = {}
        for m in re.finditer(r'\[spellbook\] (.*?) \(([0-9A-F]{8})\): nature=(\w+) -- no IAUS candidate: (.*)', open(a.log, encoding='utf-8', errors='replace').read()):
            f = int(m.group(2), 16); seen[f] = (m.group(1), m.group(3), m.group(4))
        hit = miss = 0
        for f, (nm, n, why) in seen.items():
            ks = loc.get(f & 0xFFF if f >> 24 == 0xFE else f & 0xFFFFFF, [])
            if len(ks) != 1: continue
            ld = lab[ks[0]][1]; hit += 1
            if ld != why: miss += 1; print('LOG DIFFERS', nm, '%08X' % f, 'log:', why, 'lab:', ld, spells[ks[0]]['edid'])
        print('log: %d dropped spells named, %d matched to a taught spell, %d differ' % (len(seen), hit, miss))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
