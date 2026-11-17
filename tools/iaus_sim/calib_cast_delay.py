#!/usr/bin/env python3
"""iaus_sim/calib_cast_delay.py -- pick -> fire cast delay out of saved field logs, for World.h StartCast (sim v3 `delivery`).

READ-ONLY. usage: calib_cast_delay.py [--windows] [--cpp] <run-dir> [...]     (a run dir holds MFO.log and APMF.log of ONE session)

THE JOIN. An IAUS cast pick ([iaus] ... pick=cast_<family> Name(FORM) in MFO.log) fires its gambit row in the same lap; the claim
it files reaches Harbinger, whose caster-type census (APMF.log [ctcensus], [Probe] bCasterTypeCensus=1) opens ONE WINDOW per claim:
    OPEN  #<series>.<n> driven=<the form the hand is given: the spell, or a delivery-flip proxy FF......> spell=<the source spell>
    BUILT / FIRED type=<caster type> ... at +<ms>      (FIRED = the engine released it from the hand, NotifyStartCast)
    CLOSE #<series>.<n> ... <ms>ms (<why>) | VERDICT ... | first build +N first YES +N first fire +N ... | conc yes|no
So a window IS the claim, its `spell=` is the pick's form whatever proxy drives the hand, and its FIRED offsets are the claim-to-fire
times. The pick -> OPEN step is measured too (the MFO pick line against the window that opens next for that follower and spell).
A window that overlaps a main-thread stall ([hb] mainAge > 6.5 s: the game sat in a menu or a load, wall time ran) is left out.
What is NOT a pick: a window whose claim came from the out-of-combat substrate road (MFO.log [substrate] ... CAST CLAIMED ... proxy
FF......, the Candlelight top-up): tagged `substrate` and left out of every number.

NATURE of a window: the family IAUS names that form with anywhere in MFO.log (cast_offense / cast_control -> offense, cast_heal ->
heal, cast_buff / cast_ward / cast_summon -> buff); a form IAUS never named (a table-driven claim) falls back to the caster type
that fired or was built (Restore -> heal, Offensive / TargetEffect / Stagger / Paralyze / Disarm -> offense, else buff) and is
counted separately as `typed`.

WHAT IS REPORTED, per nature:
  first     claim -> the FIRST fire of a window (+ the median pick -> OPEN step): n, p25 / median / p75, and the deciles the sim draws from
  repeat    fire -> the next fire inside one window (the claim stands and the engine casts again; a held IAUS pick in the sim), from
            MFO.log's own cast observer ([cast] ... CFC-fired, one line per fire; the census logs a window's FIRST fire only)
  unfired   windows that closed with no fire, by cause: replaced (the claim was repointed by a newer pick), refused (a caster was
            built and CheckStartCast answered NO every time: sight / line of fire / range, which the sim models itself), not-built
            (no caster was ever built), target-died (the window's target died inside it or within 1.5 s of its close: the lab ends that cast
            itself), shape-gap (not built, for a spell shape Harbinger never builds a caster for: SHAPE_GAP), and how long each stood.
            The lab's unfired share = (replaced + built-yes-unfired + not-built) / (those + fired).
--cpp prints the decile tables and the unfired shares as the rows of Data.h (Delivery).
"""
import os, re, statistics as st, sys, collections

TS = re.compile(r'^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]')
OPEN = re.compile(r'\[ctcensus\] 0x([0-9A-F]{8}) OPEN #(\d+)\.(\d+) driven=0x([0-9A-F]{8}) spell=0x([0-9A-F]{8}).*? hand=([LR]) target=0x([0-9A-F]{8}).*? conc=(yes|no) controller=(\w+)')
FIRED = re.compile(r'\[ctcensus\] 0x([0-9A-F]{8}) FIRED type=(\w+) item=0x([0-9A-F]{8}) \(driven 0x([0-9A-F]{8})\) hand=([LR]) .*? at \+(\d+) ms')
BUILT = re.compile(r'\[ctcensus\] 0x([0-9A-F]{8}) BUILT type=(\w+) item=0x([0-9A-F]{8}) \(driven 0x([0-9A-F]{8})\) hand=([LR]) at \+(\d+) ms -- CheckStartCast answered (YES|NO)')
CLOSE = re.compile(r'\[ctcensus\] 0x([0-9A-F]{8}) CLOSE #(\d+)\.(\d+) driven=0x([0-9A-F]{8}) spell=0x([0-9A-F]{8}) hand=([LR]) .*? (\d+)ms \(([^)]*)\) \| VERDICT: (.*?) \| built \[([^\]]*)\] fired \[([^\]]*)\]')
PICK = re.compile(r'\[iaus\] .*? \(([0-9A-F]{8})\) pick=cast_(\w+) .*?\(([0-9A-F]{8})\)')
NAMED = re.compile(r'cast_(offense|control|heal|buff|ward|summon) [^|(]*\(([0-9A-F]{8})\)')
CFC = re.compile(r'\[cast\] ([0-9A-F]{8}) .*? CFC-fired .*?\(([0-9A-F]{8})\)')
HB = re.compile(r'\[hb\] .*? mainAge=(\d+)ms')
SUBST = re.compile(r'\[substrate\] ([0-9A-F]{8}) CAST CLAIMED: .*? \(([0-9A-F]{8})\) at [0-9A-F]{8}, proxy ([0-9A-F]{8})')
DEATH = re.compile(r'died|dead\b|DEAD')
FORM = re.compile(r'\b([0-9A-F]{8})\b')
# SPELL SHAPES HARBINGER DOES NOT BUILD A CASTER FOR (review F1 of 5749026a): a NOT BUILT window of one of these is a classification gap of the
# framework (the census's own `predicted` label), the same every time, not a per-cast chance. They are left out of the unfired share and listed for
# Harbinger. form -> (name, the census label).
SHAPE_GAP = {'0003AE9F': ('Flame Cloak', 'no-row'), '0003AEA2': ('Frost Cloak', 'no-row'), '0003AEA3': ('Lightning Cloak', 'no-row'),
             '0002B96B': ('Frostbite', 'av26 row-without-caster'), '800279BD': ('Creeping Cold', 'av26 row-without-caster'),
             '0004D3F8': ('Turn Lesser Undead', 'TargetEffect no-row')}
UNFIRED = ('replaced', 'not-built', 'built-yes-unfired')   # what the lab's unfired share counts
NAT = {'offense': 'offense', 'control': 'offense', 'heal': 'heal', 'buff': 'buff', 'ward': 'buff', 'summon': 'buff'}
TYPE_NAT = {'Restore': 'heal', 'Offensive': 'offense', 'TargetEffect': 'offense', 'Stagger': 'offense', 'Paralyze': 'offense', 'Disarm': 'offense'}


def secs(line):
    m = TS.match(line)
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000.0 if m else None


def q(v, p):   # linear-interpolated quantile
    v = sorted(v)
    if not v: return float('nan')
    x = p * (len(v) - 1); i = int(x); f = x - i
    return v[i] if i + 1 >= len(v) else v[i] * (1 - f) + v[i + 1] * f


def read_run(d):
    mfo, apmf = os.path.join(d, 'MFO.log'), os.path.join(d, 'APMF.log')
    named, picks, subst, cfc, paused, deaths = {}, [], [], [], [], []
    with open(mfo, errors='replace') as f:
        for line in f:
            if DEATH.search(line):   # any line that says an actor died names it by FormID: the window's target dying is the lab's own castLost
                t = secs(line)
                if t is not None: deaths += [(t, f) for f in FORM.findall(line)]
            if '[iaus]' in line:
                for fam, form in NAMED.findall(line): named.setdefault(form, collections.Counter())[NAT[fam]] += 1
                m = PICK.search(line)
                if m: picks.append((secs(line), m.group(1), NAT.get(m.group(2), None), m.group(3)))
            elif '[substrate]' in line:
                m = SUBST.search(line)
                if m: subst.append((secs(line), m.group(1), m.group(2), m.group(3)))
            elif 'CFC-fired' in line:
                m = CFC.search(line)
                if m: cfc.append((secs(line), m.group(1), m.group(2)))
            elif '[hb]' in line:   # the main thread's age at a heartbeat: far past the 5 s beat = the game sat in a menu / a load (wall time ran, game time did not)
                m = HB.search(line)
                if m and int(m.group(1)) > 6500: paused.append((secs(line) - int(m.group(1)) / 1000.0, secs(line)))
    wins, live = [], {}
    with open(apmf, errors='replace') as f:
        for line in f:
            if '[ctcensus]' not in line: continue
            t = secs(line)
            m = OPEN.search(line)
            if m:
                w = dict(run=d, actor=m.group(1), id=(int(m.group(2)), int(m.group(3))), driven=m.group(4), spell=m.group(5), hand=m.group(6), target=m.group(7),
                         conc=m.group(8) == 'yes', ctrl=m.group(9), open=t, fires=[], cfc=[], paused=False, types=[], built=[], dur=None, why='(log ended)', verdict='', nat=None, typed=False, substrate=False)
                live[(m.group(1), w['id'])] = w; wins.append(w); continue
            m = FIRED.search(line)
            if m:
                for w in live.values():
                    if w['actor'] == m.group(1) and w['driven'] == m.group(4) and w['hand'] == m.group(5): w['fires'].append(int(m.group(6)) / 1000.0); w['types'].append(m.group(2))
                continue
            m = BUILT.search(line)
            if m:
                for w in live.values():
                    if w['actor'] == m.group(1) and w['driven'] == m.group(4) and w['hand'] == m.group(5): w['built'].append((int(m.group(6)) / 1000.0, m.group(7) == 'YES', m.group(2)))
                continue
            m = CLOSE.search(line)
            if m:
                w = live.pop((m.group(1), (int(m.group(2)), int(m.group(3)))), None)
                if w: w['dur'] = int(m.group(7)) / 1000.0; w['why'] = m.group(8); w['verdict'] = m.group(9)
    for w in wins:
        end = w['open'] + (w['dur'] if w['dur'] is not None else 1e9)
        w['paused'] = any(a < end and b > w['open'] for a, b in paused)
        w['died'] = any(f == w['target'] and w['open'] - 0.5 <= t <= end + 1.5 for t, f in deaths)   # inside the window or within 1.5 s of its close
        # every fire of the window, from MFO's own cast observer (the census logs the FIRST fire only): the form is the driven one
        w['cfc'] = sorted(ts - w['open'] for ts, a, form in cfc if a == w['actor'] and form in (w['driven'], w['spell']) and w['open'] - 0.05 <= ts <= end + 0.3)
        # the substrate road: its claim is logged by MFO within a second of the window's open, for that follower and source spell
        w['substrate'] = any(a == w['actor'] and sp == w['spell'] and abs(ts - w['open']) < 1.5 for ts, a, sp, px in subst)
        c = named.get(w['spell'])
        if c: w['nat'] = c.most_common(1)[0][0]
        else:
            ty = (w['types'] or [b[2] for b in w['built']] or [None])[0]
            if ty: w['nat'] = TYPE_NAT.get(ty, 'buff'); w['typed'] = True
    # pick -> OPEN: the first window of that follower and spell that opens within 1 s after the pick line
    lag = []
    for ts, actor, nat, form in picks:
        c = [w['open'] - ts for w in wins if w['actor'] == actor and w['spell'] == form and -0.05 <= w['open'] - ts <= 1.0]
        if c: lag.append(min(c))
    return wins, picks, lag


def cause(w):
    if w['fires']: return 'fired'
    if w['dur'] is None: return 'open-at-log-end'
    if w.get('died'): return 'target-died'   # the lab ends such a cast itself (castLost)
    if not w['built'] and w['spell'] in SHAPE_GAP: return 'shape-gap'   # no caster was built for a shape Harbinger never builds one for, however the window closed
    if 'repointed' in w['why']: return 'replaced'
    if w['built']: return 'refused' if not any(y for _, y, _ in w['built']) else 'built-yes-unfired'
    return 'not-built'


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]; flags = {a for a in sys.argv[1:] if a.startswith('--')}
    if not args: print(__doc__); return 2
    wins, picks, lag = [], [], []
    for d in args:
        w, p, l = read_run(d); wins += w; picks += p; lag += l
        print(f'{d}: {len(w)} claim windows ({sum(1 for x in w if x["substrate"])} substrate), {len(p)} [iaus] cast picks, {len(l)} picks joined to a window')
    if '--windows' in flags:
        for w in wins:
            print(f'  {w["run"][-12:]:12s} {w["actor"]} #{w["id"][0]}.{w["id"][1]:<3d} {w["spell"]} hand={w["hand"]} conc={int(w["conc"])} ctrl={w["ctrl"]:4s} nat={str(w["nat"]):8s}{"*" if w["typed"] else " "}'
                  f'{"S" if w["substrate"] else " "} dur={w["dur"] if w["dur"] is not None else -1:7.2f} {cause(w):18s} first={",".join(f"{x:.2f}" for x in w["fires"][:1])} cfc={",".join(f"{x:.1f}" for x in w["cfc"][:14])}{" PAUSED" if w["paused"] else ""}')
    print(f'pick -> OPEN (the claim reaches Harbinger): n={len(lag)} median {q(lag, .5):.3f} s  p25 {q(lag, .25):.3f}  p75 {q(lag, .75):.3f}')
    use = [w for w in wins if not w['substrate'] and w['nat'] and not w['paused']]
    print(f'windows that overlap a main-thread stall (a menu / a load: [hb] mainAge > 6.5 s), left out: {sum(1 for w in wins if w["paused"] and not w["substrate"])}')
    print(f'windows used: {len(use)} of {len(wins)} (substrate {sum(1 for w in wins if w["substrate"])}, no nature {sum(1 for w in wins if not w["substrate"] and not w["nat"])}); nature by caster type only: {sum(1 for w in use if w["typed"])}')
    lagmed = q(lag, .5) if lag else 0.0
    rows = {}
    for nat in ('offense', 'heal', 'buff'):
        for conc in (False, True):
            ws = [w for w in use if w['nat'] == nat and w['conc'] == conc]
            if not ws: continue
            first = [w['fires'][0] for w in ws if w['fires']]
            rep = [b - a for w in ws for a, b in zip(w['cfc'], w['cfc'][1:])]
            cs = collections.Counter(cause(w) for w in ws)
            print(f'\n== {nat}{" (concentration)" if conc else ""}: {len(ws)} windows ==')
            if first: print(f'  first   claim -> fire  n={len(first):3d}  p25 {q(first, .25):.2f}  median {q(first, .5):.2f}  p75 {q(first, .75):.2f}  max {max(first):.2f}   (+ pick -> OPEN {lagmed:.2f})')
            if rep: print(f'  repeat  fire  -> fire  n={len(rep):3d}  p25 {q(rep, .25):.2f}  median {q(rep, .5):.2f}  p75 {q(rep, .75):.2f}  max {max(rep):.2f}')
            n = len(ws)
            for k in ('fired', 'replaced', 'refused', 'target-died', 'shape-gap', 'built-yes-unfired', 'not-built', 'open-at-log-end'):
                if cs[k]:
                    du = [w['dur'] for w in ws if cause(w) == k and w['dur'] is not None]
                    print(f'  {k:18s} {cs[k]:3d}  {100.0 * cs[k] / n:5.1f} %' + (f'   stood: median {q(du, .5):.2f} s  p25 {q(du, .25):.2f}  p75 {q(du, .75):.2f}' if du else ''))
            rows[(nat, conc)] = (first, rep, cs, n)
    if '--cpp' in flags:
        # World.h Delivery rows. unfired = every close with no fire EXCEPT `refused` (sight / line of fire / range: the sim holds those casts itself),
        # over the windows that were not refused; stood = how long such a claim stood. A table with under 5 samples is the pooled one.
        def dec(v): return '{ ' + ', '.join(f'{q(v, i / 10):.2f}f' for i in range(11)) + ' }'
        pool_stood = [w['dur'] for w in use if cause(w) in UNFIRED and w['dur'] is not None]
        pool_first = [w['fires'][0] for w in use if w['fires'] and not w['conc']]
        print(f'// pooled first fire, fire-and-forget (the rows never sampled: summon, ward, armor) n={len(pool_first)}: {dec(pool_first)}')
        print(f'\n// World.h Delivery (deciles 0, 10, .. 100 %, seconds). pick -> OPEN {lagmed:.2f} s. pooled stood n={len(pool_stood)}: {dec(pool_stood)}')
        for (nat, conc), (first, rep, cs, n) in rows.items():
            ws = [w for w in use if w['nat'] == nat and w['conc'] == conc]
            unf = [w for w in ws if cause(w) in UNFIRED]
            den = sum(1 for w in ws if cause(w) in UNFIRED or cause(w) == 'fired')   # refused, target-died, shape-gap and open-at-log-end are in neither
            stood = [w['dur'] for w in unf if w['dur'] is not None]
            print(f'//   {nat}{"/conc" if conc else ""}: unfired {len(unf)}/{den} = {len(unf) / max(1, den):.3f}')
            print(f'//     first  n={len(first):2d} {dec(first) if len(first) >= 5 else "(pooled)"}')
            print(f'//     repeat n={len(rep):2d} {dec(rep) if len(rep) >= 5 else "(none: the first table)"}')
            print(f'//     stood  n={len(stood):2d} {dec(stood) if len(stood) >= 5 else "(pooled)"}')
    gaps = collections.Counter((w['spell'], w['nat'], w['conc']) for w in use if cause(w) == 'shape-gap')
    if gaps:
        print('\nNOT BUILT by shape (for Harbinger: a claim for one of these builds no caster):')
        for (sp, nat, conc), n in sorted(gaps.items(), key=lambda kv: -kv[1]):
            tot = sum(1 for w in use if w['spell'] == sp)
            print(f'  {sp} {SHAPE_GAP[sp][0]:20s} {SHAPE_GAP[sp][1]:26s} not built {n} of {tot} windows ({nat}{", concentration" if conc else ""})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
