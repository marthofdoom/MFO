#!/usr/bin/env python3
"""The judge of the modes' acceptance (s5-plan.md 5.3 and 5.4, package S5-D). It reads what `iaus_sim --mode-accept` printed
(ModeAccept.h) and says which modes the oracle NEEDS, per class, and whether the lines hold.

    mode_accept.py <out> [<out> ...]          judge (exit 0 PASS, 1 NOT MET, 2 not an acceptance or bad input)
    mode_accept.py --open <out> ...           the scenarios that need more luck, as arguments for the next --mode-accept
    mode_accept.py --fought <calib out> ...   what was FOUGHT (GENFOUGHT lines): histograms and the pairwise holes

THE RULES
  decision   the mode whose re-estimated row is best on half A owns it when it leads EVERY other mode by over DEC_SE standard errors and over
             DEC_MARGIN U there. Else the decision is a TIE and belongs to no mode. Its value is the lead on half B, over the mode that was second on half A
             (nothing selected B, so it is unbiased).
  fight      need(m) = the sum of the values of the decisions m owns.
  scenario   NEEDS m when, over its lucks (MIN_LUCK or more), the mean of need(m) is over SCEN_MARGIN and its lower 95 percent bound over zero. TIED when
             every mode's upper bound is under SCEN_MARGIN. Both ask MIN_LUCK lucks. Else OPEN until MAX_LUCK, then unresolved.
  class      each set (generated, extracted) apart: RESOLVED scenarios need at least one mode. Shares are of the resolved ones. Bounds: the lower of Wilson's
             and a bootstrap over scenarios (over places in the extracted set).
A mode that --move-caps does not serve has no line and is not required. GoTo is a column, Disengage likewise, until they are served and ruled on.
A MELEE class's kite is not in this tune. When it is served its need counts only in fights whose strike windows hold (MELEE_KITE): a blade that backs off for ever and never
strikes is no kite. The recorder is World.h Tally / ACCEPTKITE."""
import sys, os, re, math, random, collections
HERE = os.path.dirname(os.path.abspath(__file__))
DEC_MARGIN, DEC_SE, SCEN_MARGIN = 0.30, 2.0, 0.25
MIN_LUCK, MAX_LUCK = 8, 16
MIN_RESOLVED, MIN_SHARE, MAX_ONLY = 40, 0.10, 0.60
BASE_LO, BASE_HI, BASE_SHARE = 0.30, 0.70, 0.80
LOO_AGREE, LOO_SHARE, LOO_MIN = 0.80, 0.10, 5
AGAINST_SHARE = 0.30
ONE_MODE = ('archer',)                       # who may pass as ONE-MODE by the four proofs (5.4)
MELEE = ('warrior', 'knight', 'mercenary', 'thief')
MODES = ('free', 'stand', 'goto', 'kite', 'disengage')
LINED = ('free', 'stand', 'kite')            # the modes that can have a line
BIT = {m: 1 << i for i, m in enumerate(MODES)}
MELEE_KITE = {'dmgShare': 0.30, 'gap': 4.0, 'deadShare': 0.34, 'longest': 12.0}   # PLACEHOLDER: damage dealt share, longest stretch without an attack, episodes that never struck, the longest one
BOOT = 2000
SCEN_SETS = ('gen', 'real')


def gen_version():
    return re.search(r'kModeGenVersion = "([^"]+)"', open(os.path.join(HERE, 'ModeGen.h')).read()).group(1)


def tcrit(n):   # two-sided 95 percent, n samples
    T = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365, 8: 2.306, 9: 2.262, 10: 2.228, 11: 2.201, 12: 2.179, 13: 2.160, 14: 2.145, 15: 2.131, 20: 2.086, 30: 2.042}
    d = n - 1
    return T[d] if d in T else 1.96 + 2.5 / max(1, d)
def mean(v): return sum(v) / len(v) if v else 0.0
def bounds(v):
    n = len(v); m = mean(v)
    if n < 2: return m, -1e9, 1e9
    h = tcrit(n) * math.sqrt(sum((x - m) ** 2 for x in v) / (n - 1) / n)
    return m, m - h, m + h
def wilson_low(k, n, z=1.96):
    if not n: return 0.0
    p = k / n
    return (p + z * z / (2 * n) - z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n))) / (1 + z * z / n)
def boot(clusters, stat, q):   # clusters: lists of items. stat over the drawn items
    if not clusters: return 0.0
    r = random.Random(20261125); out = []
    for _ in range(BOOT):
        pick = [x for _ in clusters for x in clusters[r.randrange(len(clusters))]]; out.append(stat(pick))
    out.sort(); return out[min(len(out) - 1, int(q * len(out)))]
def share_low(hits, cluster):   # hits: bool a scenario, cluster: its cluster id
    by = collections.defaultdict(list)
    for h, c in zip(hits, cluster): by[c].append(h)
    return min(wilson_low(sum(hits), len(hits)), boot(list(by.values()), lambda v: sum(v) / len(v) if v else 0.0, 0.025))


def decision(others):
    """others: {mode: (leadA, seA, leadB)} of the owner. Returns its value, or None for a tie."""
    if not others: return None
    if any(a <= DEC_MARGIN or a <= DEC_SE * se for a, se, _ in others.values()): return None
    second = min(others, key=lambda m: others[m][0])
    return others[second][2]


class Run:
    def __init__(self): self.fight = {}; self.dec = collections.defaultdict(list); self.kite = {}; self.head = None
def kv(tokens): return dict(t.split('=', 1) for t in tokens if '=' in t)
def parse(paths):
    """-> {(set, refuse mask): Run}. One family, version, caps and against; a set of one seed and one library."""
    runs = {}; same = {}
    for p in paths:
        cur = None; done = True
        for raw in open(p):
            t = raw.split()
            if not t: continue
            if t[0] == 'ACCEPTRUN':
                if not done: raise ValueError('%s: a run that did not finish' % p)
                h = kv(t[1:]); done = False
                for k in ('gen', 'family', 'caps', 'k'):
                    if same.setdefault(k, h[k]) != h[k]: raise ValueError('%s: %s %s, not %s' % (p, k, h[k], same[k]))
                for k in ('seed', 'places', 'drawn'):
                    if same.setdefault((h['set'], k), h[k]) != h[k]: raise ValueError('%s: two runs of set %s with different %s' % (p, h['set'], k))
                cur = runs.setdefault((h['set'], int(h['refuse'])), Run()); cur.head = h
            elif t[0] == 'ACCEPTFIGHT':
                key = (t[2], int(t[1]), int(t[3]))
                if key in cur.fight: raise ValueError('%s: fight %r twice' % (p, key))
                cur.fight[key] = kv(t[4:])
                if same.setdefault((cur.head['set'], t[2], 'against'), cur.head['against']) != cur.head['against']: raise ValueError('%s: %s in two draws (--mode-against)' % (p, t[2]))
                if cur.head['against'] != '-' and t[2] not in ONE_MODE: raise ValueError('%s: %s may not be drawn against a mode (5.4)' % (p, t[2]))
            elif t[0] == 'ACCEPTKITE': cur.kite[(t[2], int(t[1]), int(t[3]))] = kv(t[4:])
            elif t[0] == 'ACCEPTDEC':
                d = kv(t[4:]); own = d['own']
                cur.dec[(t[2], int(t[1]), int(t[3]))].append((own, {m: tuple(float(x) for x in d[m].split('/')) for m in MODES if m in d}, int(d.get('spots', 0))))
            elif t[0] == 'ACCEPTDONE': done = True
        if not done: raise ValueError('%s: a run that did not finish' % p)
    return runs, same


def served(caps):
    c = caps.split(',')
    return ['free', 'stand'] + (['kite'] if 'kite' in c else [])
def lines_for(cls, caps): return [m for m in served(caps) if not (m == 'kite' and cls in MELEE)]   # a blade has no Kite line: its column is printed, with the strike windows
def strikes_ok(k):
    ep = max(1, int(k['episodes']))
    return float(k['dmgShare']) >= MELEE_KITE['dmgShare'] and float(k['gap']) <= MELEE_KITE['gap'] and int(k['dead']) / ep <= MELEE_KITE['deadShare'] and float(k['longest']) <= MELEE_KITE['longest']
def fight_need(run, key):
    need = dict.fromkeys(MODES, 0.0)
    for own, others, _ in run.dec.get(key, []):
        if own == 'none': continue
        v = decision(others)
        if v is None: continue
        if own == 'kite' and key[0] in MELEE and not (key in run.kite and strikes_ok(run.kite[key])): continue
        need[own] += v
    return need


class Scen:
    def __init__(self): self.keys = []; self.needs = {}; self.state = 'open'; self.needed = []
def scenarios(run, cls):
    out = {}
    for key in sorted(run.fight):
        if key[0] == cls: out.setdefault(key[1], Scen()).keys.append(key)
    for s in out.values():
        n = len(s.keys); needs = [fight_need(run, k) for k in s.keys]; s.needs = {m: bounds([x[m] for x in needs]) for m in MODES}
        s.needed = [m for m in MODES if n >= MIN_LUCK and s.needs[m][0] > SCEN_MARGIN and s.needs[m][1] > 0.0]
        if s.needed: s.state = 'resolved'
        elif n >= MIN_LUCK and all(s.needs[m][2] < SCEN_MARGIN for m in MODES): s.state = 'tied'
        else: s.state = 'open' if n < MAX_LUCK else 'unresolved'
    return out
def base_in(f): return f.get('ok') == '1' and f.get('way') == '1' and BASE_LO <= float(f['win']) <= BASE_HI
def gain(run, cls, s): return mean([float(run.fight[k]['uO']) - float(run.fight[k]['uI']) for k in s.keys])
def loss(run, other, s):   # the paired loss of a leave-one-out arm, or None where it did not play every luck of the scenario
    if other is None or any(k not in other.fight for k in s.keys): return None
    return [float(run.fight[k]['uO']) - float(other.fight[k]['uO']) for k in s.keys]


def judge_class(runs, st, cls, out):
    run = runs[(st, 0)]; caps = run.head['caps']; sc = scenarios(run, cls); ids = sorted(sc); lines = lines_for(cls, caps); ok = True; drawn = max(1, int(run.head['drawn']))
    cl = lambda k: k % drawn if st == 'real' else k   # the scenarios of one place are one cluster
    P = lambda good, name, text: out.append('  %-6s %-14s %s' % ('ok' if good else 'NOT MET', name, text))
    res = [k for k in ids if sc[k].state == 'resolved']; cnt = collections.Counter(s.state for s in sc.values())
    by = collections.defaultdict(list)
    for k in ids: by[cl(k)].append(gain(run, cls, sc[k]))
    g = mean([x for v in by.values() for x in v]); lo, hi = (boot(list(by.values()), mean, q) for q in (0.025, 0.975))
    inside = sum(base_in(run.fight[sc[k].keys[0]]) for k in ids); wI = [mean([float(run.fight[x]['winI']) for x in sc[k].keys]) for k in ids]; inI = sum(BASE_LO <= w <= BASE_HI for w in wI)
    out.append('%s %s: %d scenarios, %d to %d lucks, resolved %d tied %d open %d unresolved %d, oracle less IAUS %+.2f U [%+.2f, %+.2f], IAUS on the road wins inside 30-70%% in %d' % (
        st.upper(), cls, len(ids), min(len(s.keys) for s in sc.values()), max(len(s.keys) for s in sc.values()), len(res), cnt['tied'], cnt['open'], cnt['unresolved'], g, lo, hi, inI))
    few = [k for k in ids if len(sc[k].keys) < MIN_LUCK]
    b = inside >= BASE_SHARE * len(ids); P(b, 'base win', '%d of %d inside 30-70%% (road off, the calibration\'s check)%s' % (inside, len(ids), '' if b else ': the class is not judged')); ok &= b
    if few: P(False, 'luck', '%d scenarios have under %d lucks' % (len(few), MIN_LUCK)); ok = False
    b = len(res) >= MIN_RESOLVED; P(b, 'resolved', '%d, at least %d' % (len(res), MIN_RESOLVED)); ok &= b
    need = {m: [m in sc[k].needed for k in res] for m in MODES}; only = {m: [sc[k].needed == [m] for k in res] for m in MODES}; c = [cl(k) for k in res]
    taught = {m: bool(res) and share_low(need[m], c) >= MIN_SHARE for m in MODES}; habit = {m: bool(res) and share_low(only[m], c) >= MAX_ONLY for m in MODES}
    fails = [m for m in lines if not taught[m]]; fails2 = [m for m in lines if habit[m]]; n = max(1, len(res))
    for m in lines: P(taught[m] and not habit[m], 'mode ' + m, 'needed in %d of %d (%.0f%%), alone in %.0f%%' % (sum(need[m]), len(res), 100 * sum(need[m]) / n, 100 * sum(only[m]) / n))
    for m in MODES:
        if m not in lines and sum(need[m]): out.append('  column %-14s needed in %d of %d (no line)' % ('mode ' + m, sum(need[m]), len(res)))
    for m in LINED:
        if m not in served(caps): out.append('  -      %-14s not served by the caps: not required' % ('mode ' + m))
    one = None
    if ok and (fails or fails2):
        dom = max(lines, key=lambda m: sum(need[m])); minor = [m for m in lines if m != dom]; mask = sum(BIT[m] for m in minor)
        if cls in ONE_MODE:
            rows = [k for k in ids if run.fight[sc[k].keys[0]].get('against') == dom]; rr = [k for k in rows if sc[k].state == 'resolved']
            p1 = len(rows) >= AGAINST_SHARE * len(ids)
            p2 = bool(rr) and not any(sum(m in sc[k].needed for k in rr) >= MIN_SHARE * len(rr) for m in minor)
            ls = [loss(run, runs.get((st, mask)), sc[k]) for k in ids]; whole = bool(ls) and all(x is not None for x in ls); up = None
            if whole:
                q = collections.defaultdict(list)
                for k, x in zip(ids, ls): q[cl(k)].append(mean(x))
                up = boot(list(q.values()), mean, 0.975)
            p3 = whole and up < SCEN_MARGIN
            dec = [d for k in res for x in sc[k].keys for d in run.dec.get(x, [])]
            p4 = len(res) >= MIN_RESOLVED and any({o, *q} >= {dom, 'free' if dom != 'free' else 'stand'} for o, q, _ in dec) and any(o == dom and sp >= 2 for o, q, sp in dec)
            P(p1, 'one-mode 1', '%d of %d scenarios are rows built against %s, at least %.0f%%' % (len(rows), len(ids), dom, 100 * AGAINST_SHARE))
            P(p2, 'one-mode 2', 'on those rows (%d resolved) no other mode reaches %.0f%%' % (len(rr), 100 * MIN_SHARE))
            P(p3, 'one-mode 3', 'the oracle without %s loses under %.2f U (%s)' % (','.join(minor), SCEN_MARGIN, 'arm refuse=%d is not complete' % mask if not whole else 'upper bound %.2f' % up))
            P(p4, 'one-mode 4', 'the resolved ones include %s against %s and places of %s' % (dom, 'free' if dom != 'free' else 'stand', dom))
            if p1 and p2 and p3 and p4: one = dom; out.append('  ONE-MODE(%s): the weights of %s are not searched (the oracle does not need them: loss under the margin)' % (dom, ','.join(minor)))
    if one is None: ok = ok and not fails and not fails2
    # the true leave-one-out arms against need(m)
    agree = tot = cover = 0
    for m in lines:
        o = runs.get((st, BIT[m]))
        for k in ids:
            x = loss(run, o, sc[k])
            if x is None: continue
            cover += 1
            if m in sc[k].needed: tot += 1; agree += mean(x) > 0.0
    b = cover >= LOO_SHARE * len(ids) * len(lines) and tot >= LOO_MIN and agree >= LOO_AGREE * tot
    P(b, 'leave-one-out', '%d of %d needing scenarios lose without the mode (%d arm-scenarios played), at least %.0f%% of at least %d' % (agree, tot, cover, 100 * LOO_AGREE, LOO_MIN))
    if tot >= LOO_MIN and agree < LOO_AGREE * tot: out.append('  THE DECISION MEASURE IS NO PROXY FOR THE NEED HERE: redo on leave-one-out arms (four times the cost)')
    return ok and b, one, res, need, only


def judge(paths):
    out = []; runs, same = parse(paths)
    if same['gen'] != gen_version(): return 2, ['generator %s, the code is %s: a held-out family of another generator is refused' % (same['gen'], gen_version())]
    if not any(r == 0 for _, r in runs): return 2, ['no full run']
    if same['family'] != 'heldout': out.append('FAMILY %s: the numbers are a pilot, NOT AN ACCEPTANCE' % same['family'])
    ok = True; tot = collections.Counter(); N = 0; best = collections.defaultdict(float); caps = same['caps']
    for st in SCEN_SETS:
        if (st, 0) not in runs:
            out.append('SET %s: not played' % st); ok = False; continue
        classes = sorted({k[0] for k in runs[(st, 0)].fight}); N = 0; tot = collections.Counter(); best = collections.defaultdict(float)
        for cls in classes:
            good, one, res, need, only = judge_class(runs, st, cls, out); ok &= good
            if one or not res: continue   # a ONE-MODE class does not count toward the over-all line
            N += len(res)
            for m in LINED: tot[m] += sum(only[m]); best[m] = max(best[m], sum(need[m]) / len(res))
        for m in served(caps):
            b = best[m] >= MIN_SHARE and tot[m] <= MAX_ONLY * max(1, N); ok &= b
            out.append('%s OVER ALL %-6s %s: for some class %.0f%%, alone %.0f%% over all' % (st.upper(), m, 'ok' if b else 'NOT MET', 100 * best[m], 100 * tot[m] / max(1, N)))
    if same['family'] != 'heldout': out.append('VERDICT pilot %s (no acceptance)' % ('would pass' if ok else 'would not pass')); return 2, out
    out.append('VERDICT %s' % ('PASS' if ok else 'NOT MET')); return (0 if ok else 1), out


def open_list(paths):
    runs, same = parse(paths); out = []
    for st in SCEN_SETS:
        if (st, 0) not in runs: continue
        r = runs[(st, 0)]
        for cls in sorted({k[0] for k in r.fight}):
            by = collections.defaultdict(list)
            for k, s in sorted(scenarios(r, cls).items()):
                if s.state == 'open': by[(len(s.keys), min(MAX_LUCK, max(MIN_LUCK, 2 * len(s.keys))))].append(str(k))
            for (a, b), ks in sorted(by.items()): out.append('%s --mode-class %s --mode-luck %d:%d --mode-scen %s' % (st, cls, a, b, ','.join(ks)))
    return out


# ── what was fought ──
BANDS = [('foes', 'n', [(1, '1'), (3, '2-3'), (6, '4-6'), (99, '7-10')]), ('slowest foe', 'runLo', [(0.40, 'under 0.40'), (1.0, '0.40-1'), (9, 'over 1')]), ('fastest foe', 'runHi', [(0.40, 'under 0.40'), (1.0, '0.40-1'), (9, 'over 1')]),
         ('reach over his', 'reach', [(0.001, 'no melee foe'), (0.9, 'shorter'), (1.15, 'alike'), (9, 'longer')]), ('level', 'level', [(17, '10'), (35, '25'), (99, '45')]), ('start u', 'start', [(500, 'under 500'), (1000, '500-1000'), (1500, '1000-1500'), (9999, 'over 1500')]),
         ('narrowest u', 'width', [(-0.5, 'no way'), (150, 'under 150'), (400, '150-400'), (1e9, 'over 400')]), ('height u', 'dz', [(64, 'flat'), (200, '64-200'), (1e9, 'over 200')]), ('doors', 'doors', [(0.5, 'none'), (5, '1-5'), (1e9, '6+')]), ('water %', 'water', [(0.5, 'none'), (20, 'some'), (101, 'much')]),
         ('party', 'party', [(1, 'alone'), (2, 'two'), (3, 'three'), (9, 'four')]), ('player health', 'playerHp', [(0.41, 'hurt'), (2, 'whole')]), ('player far', 'playerDist', [(500, 'near'), (1e9, 'far')]), ('light', 'light', [(0.2, 'dark'), (0.5, 'dim'), (0.8, 'light'), (2, 'day')]), ('wins', 'win', [(0.0, 'never'), (0.299, 'under 30%'), (0.70, '30-70%'), (0.9999, 'over 70%'), (1, 'always')])]
def makeup(d):
    s, c, b, a, boss = (int(d[k]) for k in ('shoot', 'cast', 'blade', 'beast', 'boss'))
    k = [n for n, v in (('shooters', s), ('mages', c), ('blades', b), ('beasts', a)) if v]
    return 'boss' if boss else 'mixed' if len(k) > 1 else k[0] if k else 'none'
def fought(paths):
    rows = []
    for p in paths:
        for raw in open(p):
            if not raw.startswith('GENFOUGHT '): continue
            a, b, c = raw.split(' | '); d = kv(b.split()); d.update(kv(c.split())); q = kv(a.split()); r = {'made of': makeup(d), 'terrain asked': re.sub(r'^(gen-|mode-)|-[abc]$', '', q['terrain']), 'class': a.split()[2], 'row': q['row'], 'second group': 'yes' if q['chain'] != '0' else 'no', 'player style': q['style'], 'a healer beside him': 'yes' if int(d['heal']) else 'no', 'player leaving': 'yes' if d['leaving'] == '1' else 'no', 'he is hunted': q['on']}
            for name, f, bands in BANDS: v = float(d[f]); r[name] = next(n for top, n in bands if v <= top)
            rows.append(r)
    if not rows: return ['no GENFOUGHT line']
    out = ['FOUGHT %d (scenario, class) pairs' % len(rows)]; axes = [k for k in rows[0]]
    for a in axes: out.append('  %-20s %s' % (a, '  '.join('%s %d (%.0f%%)' % (k, v, 100 * v / len(rows)) for k, v in sorted(collections.Counter(r[a] for r in rows).items(), key=lambda x: -x[1]))))
    pair = [a for a in axes if a not in ('wins', 'class', 'row', 'terrain asked')]; holes = []; cells = 0
    for i, a in enumerate(pair):
        for b in pair[i + 1:]:
            have = collections.Counter((r[a], r[b]) for r in rows)
            for x in sorted({r[a] for r in rows}):
                for y in sorted({r[b] for r in rows}):
                    cells += 1
                    if not have[(x, y)]: holes.append('%s=%s with %s=%s' % (a, x, b, y))
    out.append('PAIRWISE %d cells of %d axes, %d holes' % (cells, len(pair), len(holes))); out += ['  hole ' + h for h in holes]
    return out


def main(argv):
    if not argv: print(__doc__); return 2
    try:
        if argv[0] == '--fought': print('\n'.join(fought(argv[1:]))); return 0
        if argv[0] == '--open': print('\n'.join(open_list(argv[1:]))); return 0
        rc, out = judge(argv)
    except (ValueError, KeyError, OSError) as e: print('mode_accept: %s' % e, file=sys.stderr); return 2
    print('\n'.join(out)); return rc


if __name__ == '__main__': sys.exit(main(sys.argv[1:]))
