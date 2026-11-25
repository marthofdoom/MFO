#!/usr/bin/env python3
"""Rows for mode_accept.py on synthetic runs with a known answer, and mutants of it: each must fail at least one row.
    test_mode_accept.py            rows + mutants (exit 1 on a failed row or a mutant that survives)
Nothing here plays a fight."""
import sys, os, random, tempfile, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mode_accept as M

M.BOOT = 200
CLS = ('archer',)


def run_text(scens, set_='gen', family='heldout', caps='strict', refuse=0, against='-', lucks=8, gen=None, loss=0.0, cls='archer', drawn=0, only=None, win='0.50', luck0=0, done=True, row_against='-', kite=None, seed=3):
    """scens: a list over scenarios of {mode: need per fight} (the planted truth). One decision a planted mode: it owns it by the need on both halves, plus noise. A tie besides."""
    r = random.Random(5); o = ['ACCEPTRUN gen=%s family=%s seed=%d set=%s caps=%s against=%s places=0 luck=0:%d refuse=%d k=16 drawn=%d' % (gen or M.gen_version(), family, seed, set_, caps, against, lucks, refuse, drawn)]
    for k, need in enumerate(scens):
        if only is not None and k not in only: continue
        for f in range(luck0, luck0 + lucks):
            uO = 50 + 0.1 * r.random() - (loss if not callable(loss) else loss(k, need)); w = win(k) if callable(win) else win
            ra = row_against(k) if callable(row_against) else row_against
            o.append('ACCEPTFIGHT %d %s %d row=x against=%s target=0.50 win=%s n=24 ok=1 way=1 uI=49.0000 uO=%.4f winI=%d winO=1 durI=1 durO=1' % (k, cls, f, ra, w, uO, f % 2))
            for m, v in need.items():
                x = v + 0.02 * r.random(); others = [z for z in ('free', 'stand') + (('kite',) if 'kite' in caps else ()) if z != m]
                o.append('ACCEPTDEC %d %s %d t=1.00 pass=0 spots=3 own=%s %s' % (k, cls, f, m, ' '.join('%s=%.4f/0.0500/%.4f' % (z, x, x) for z in others)))
            o.append('ACCEPTDEC %d %s %d t=2.00 pass=1 spots=3 own=free stand=0.2000/0.0100/5.0000' % (k, cls, f))
            o.append('ACCEPTDEC %d %s %d t=2.50 pass=1 spots=3 own=stand free=0.9000/0.6000/5.0000' % (k, cls, f))
            o.append('ACCEPTDEC %d %s %d t=3.00 pass=1 spots=0 own=none' % (k, cls, f))
            if kite: o.append('ACCEPTKITE %d %s %d %s' % (k, cls, f, kite))
    if done: o.append('ACCEPTDONE fights=1 decisions=1 modeRolls=1')
    return '\n'.join(o) + '\n'


TMP = tempfile.mkdtemp(); N = [0]
def write(text): N[0] += 1; p = os.path.join(TMP, '%d.txt' % N[0]); open(p, 'w').write(text); return p
def judge(*texts):
    try: return M.judge([write(t) for t in texts])
    except (ValueError, KeyError) as e: return 9, [str(e)]
def mix(n_free, n_stand, n_both=0, n_tie=0): return [{'free': 1.0}] * n_free + [{'stand': 1.0}] * n_stand + [{'free': 1.0, 'stand': 1.0}] * n_both + [{}] * n_tie
def loo(scens, st, **k):   # the two leave-one-out arms on the first quarter of each mode's scenarios
    ks = {m: [i for i, s in enumerate(scens) if m in s][:12] for m in ('free', 'stand')}
    return [run_text(scens, st, refuse=M.BIT[m], only=set(ks[m]), loss=k.get('loss', lambda q, need: 1.0), **{a: b for a, b in k.items() if a != 'loss'}) for m in ('free', 'stand')]
def full(scens, **k): return [t for st in ('gen', 'real') for t in [run_text(scens, st, **k)] + loo(scens, st, **{a: b for a, b in k.items() if a not in ('row_against',)})]
GOOD = mix(30, 25, 5, 10)


def scen_state(needs_per_luck):   # one scenario, planted fights -> (state, needed)
    o = ['ACCEPTRUN gen=%s family=heldout seed=3 set=gen caps=strict against=- places=0 luck=0:8 refuse=0 k=16 drawn=0' % M.gen_version()]
    for f, decs in enumerate(needs_per_luck):
        o.append('ACCEPTFIGHT 0 archer %d row=x against=- win=0.5 ok=1 way=1 uI=1 uO=1 winI=1 winO=1' % f)
        for own, a, se, b in decs: o.append('ACCEPTDEC 0 archer %d t=1 pass=0 spots=1 own=%s %s=%.4f/%.4f/%.4f' % (f, own, 'stand' if own == 'free' else 'free', a, se, b))
    runs, _ = M.parse([write('\n'.join(o) + '\nACCEPTDONE x=1\n')]); s = M.scenarios(runs[('gen', 0)], 'archer')[0]; return s.state, s.needed


def rows():
    d = M.decision; R = []; add = lambda n, v: R.append((n, bool(v)))
    add('a lead over 0.3 and 2 se is the owner\'s, its value is half B', d({'free': (1.0, 0.2, 0.7)}) == 0.7)
    add('a lead of 0.3 is a tie', d({'free': (0.30, 0.01, 0.9)}) is None)
    add('a lead inside 2 se is a tie', d({'free': (1.0, 0.51, 0.9)}) is None)
    add('a twin is a tie', d({'free': (0.0, 0.0, 0.0)}) is None)
    add('every other mode must be beaten', d({'free': (2.0, 0.1, 1.0), 'kite': (0.1, 0.1, 5.0)}) is None)
    add('the value is over the second best, not the weakest', d({'free': (2.0, 0.1, 9.0), 'kite': (0.5, 0.1, 0.4)}) == 0.4)
    add('half B may say the lead was luck: negative values count', d({'free': (1.0, 0.1, -0.8)}) == -0.8)
    add('half A never is the value', d({'free': (3.0, 0.1, 0.0)}) == 0.0)
    flip = [[('stand', 1.0, 0.1, 1.0)]] * 8
    add('eight lucks, a steady need: resolved', scen_state(flip) == ('resolved', ['stand']))
    add('seven lucks are too few', scen_state(flip[:7])[0] == 'open')
    add('a need of either sign is none', scen_state([[('stand', 1.0, 0.1, (-1) ** f * 3.0 + 0.3)] for f in range(8)])[0] == 'open')
    add('the same at sixteen is unresolved', scen_state([[('stand', 1.0, 0.1, (-1) ** f * 3.0 + 0.3)] for f in range(16)]) == ('unresolved', []))
    add('a steady need under the scenario margin is tied', scen_state([[('stand', 1.0, 0.1, 0.1)]] * 8)[0] == 'tied')
    add('a negative steady need is no need', scen_state([[('stand', 1.0, 0.1, -1.0)]] * 8) == ('tied', []))
    add('no decision at all is a tie', scen_state([[]] * 8)[0] == 'tied')
    add('two modes of one fight can both be needed', scen_state([[('stand', 1.0, 0.1, 1.0), ('free', 1.0, 0.1, 1.0)]] * 8) == ('resolved', ['free', 'stand']))
    rc, o = judge(*full(GOOD)); add('a set that teaches both modes passes', rc == 0 or print('\n'.join(o)))
    add('ties are their own count, never resolved', any('resolved 60 tied 10' in x for x in o))
    add('kite is not required where it is not served', any('mode kite' in x and 'not required' in x for x in o) and not any('OVER ALL kite' in x for x in o))
    rc, o = judge(*full([s for s in GOOD], caps='strict,kite')); add('kite is required where it is served', rc == 1 and any('NOT MET' in x and 'mode kite' in x for x in o))
    k = [{'kite': 1.0}] * 20 + GOOD
    add('... and met by scenarios that need it', judge(*full(k, caps='strict,kite'))[0] == 1 and any(x.startswith('  ok') and 'mode kite' in x for x in judge(*full(k, caps='strict,kite'))[1]))
    add('a blade has no kite line', not any('NOT MET' in x and 'mode kite' in x for x in judge(*full(GOOD, caps='strict,kite', cls='warrior'))[1]))
    runs = lambda line, cls: M.parse([write(run_text(k, caps='strict,kite', cls=cls, kite=line))])[0][('gen', 0)]
    good, bad = 'kiteSec=9 episodes=2 dead=0 longest=6.0 strikes=5 gap=2.0 dmgShare=0.500', 'kiteSec=9 episodes=2 dead=0 longest=6.0 strikes=0 gap=9.0 dmgShare=0.500'
    nk = lambda line, cls: M.scenarios(runs(line, cls), cls)[0].needed
    add('a blade\'s kite without strike windows is no need', nk(bad, 'warrior') == [] and nk(good, 'warrior') == ['kite'])
    add('a kite that deals no share is none', nk(good.replace('0.500', '0.100'), 'warrior') == []); add('a kite that never ends is none', nk(good.replace('longest=6.0', 'longest=40.0'), 'warrior') == [])
    add('episodes that never strike are none', nk(good.replace('dead=0', 'dead=2'), 'warrior') == []); add('a shooter\'s kite is judged by U alone', nk(bad, 'archer') == ['kite'])
    rc, o = judge(*full(mix(20, 19))); add('39 resolved are too few', rc == 1 and any('NOT MET' in x and 'resolved' in x for x in o))
    rc, o = judge(*full(mix(48, 2), cls='warrior')); add('a mode 4 percent need is not taught', rc == 1 and any('NOT MET' in x and 'mode stand' in x for x in o))
    rc, o = judge(*full(mix(60, 9, 0), cls='warrior')); add('13 percent is under the lower bound', rc == 1 and any('NOT MET' in x and 'mode stand' in x for x in o))
    rc, o = judge(*full(mix(80, 20), cls='warrior')); add('a single habit fails', rc == 1 and any('NOT MET' in x and 'mode free' in x for x in o))
    rc, o = judge(*full(GOOD, win=lambda q: '0.50' if q % 3 else '0.95')); add('fights that are won anyway are not judged', rc == 1 and any('not judged' in x for x in o))
    rc, o = judge(*full(GOOD, lucks=4)); add('four lucks do not judge', rc == 1)
    rc, o = judge(*full(GOOD, family='design')); add('a design family is no acceptance', rc == 2 and any('NOT AN ACCEPTANCE' in x for x in o))
    add('another generator is refused', judge(*full(GOOD, gen='g1.9'))[0] == 2)
    add('one set alone is no acceptance', judge(run_text(GOOD), *loo(GOOD, 'gen'))[0] == 1)
    add('no leave-one-out arm, no pass', judge(run_text(GOOD), run_text(GOOD, 'real'))[0] == 1)
    rc, o = judge(*full(GOOD, loss=lambda q, need: -1.0 if 'free' in need else 1.0)); add('a need the leave-one-out arm does not share is said and fails', rc == 1 and any('NO PROXY' in x for x in o))
    rc, o = judge(*[run_text(GOOD, st) for st in ('gen', 'real')], *[t for st in ('gen', 'real') for t in loo(GOOD, st, loss=lambda q, need: 0.0 if q % 10 == 0 else 1.0)]); add('a tenth of disagreement is allowed', rc == 0)
    # ONE-MODE
    am = mix(2, 60, 0, 0); ag = lambda q: 'stand' if q % 2 else '-'
    def arch(**k):
        c = k.pop('c', 'archer'); rowa = k.pop('rowa', ag); dl = k.pop('dl', 0.1); part = k.pop('part', None); a = 'stand' if c == 'archer' else '-'; sc = k.pop('sc', am)
        return [t for st in ('gen', 'real') for t in [run_text(sc, st, cls=c, against=a, row_against=rowa), run_text(sc, st, cls=c, against=a, refuse=M.BIT['free'], loss=dl, only=part), run_text(sc, st, cls=c, against=a, refuse=M.BIT['stand'], loss=1.0, row_against=rowa)]]
    one = lambda o: any('ONE-MODE(stand)' in x for x in o)
    add('an archer whose answer is the stand is ONE-MODE by the four proofs', one(judge(*arch())[1]))
    add('not without the rows against it', not one(judge(*arch(rowa=lambda q: 'stand' if q % 5 == 0 else '-'))[1]))
    add('not when the oracle without the minority loses', not one(judge(*arch(dl=0.6))[1]))
    add('not on a part of the arm', not one(judge(*arch(part=set(range(6))))[1]))
    add('not when another mode is needed on the rows against it', not one(judge(*arch(sc=[{'free': 1.0} if q in (1, 3, 5, 7) else {'stand': 1.0} for q in range(62)]))[1]))
    add('no other class may be ONE-MODE', not one(judge(*arch(c='evoker'))[1]))
    add('no other class may be drawn against a mode', judge(run_text(GOOD, cls='evoker', against='stand'))[0] == 9)
    add('a run that did not finish is refused', judge(run_text(GOOD, done=False))[0] == 9)
    add('a fight twice is refused', judge(run_text(GOOD), run_text(GOOD))[0] == 9)
    add('two seeds of one set are refused', judge(run_text(GOOD), run_text(GOOD, luck0=8, seed=4))[0] == 9)
    add('more luck of the same run is joined', judge(*[run_text(GOOD, st, lucks=4, luck0=f) for st in ('gen', 'real') for f in (0, 4)], *loo(GOOD, 'gen', lucks=8), *loo(GOOD, 'real', lucks=8))[0] == 0)
    p = [write(run_text([{'stand': 1.0}, {}, {'stand': 1.0}], lucks=4))]; add('the open ones are asked for with their next lucks', M.open_list(p) == ['gen --mode-class archer --mode-luck 4:8 --mode-scen 0,1,2'])
    add('places are the unit: 40 of 100 scenarios out of 2 of 5 places is no lower bound', M.share_low([i < 40 for i in range(100)], [i // 20 for i in range(100)]) < 0.05 < 0.25 < M.share_low([i < 40 for i in range(100)], list(range(100))))
    rc, o = judge(*[run_text(GOOD, st, only=set(range(60))) for st in ('gen', 'real')], *[run_text(GOOD, st, only=set(range(60, 70)), lucks=4) for st in ('gen', 'real')], *[t for st in ('gen', 'real') for t in loo(GOOD, st)]); add('ten scenarios short of luck fail, whatever the others say', rc == 1)
    rc, o = judge(*[run_text(GOOD, st) for st in ('gen', 'real')], *[run_text(GOOD, st, refuse=M.BIT[m], only=set(range(60, 70)), loss=1.0) for st in ('gen', 'real') for m in ('free', 'stand')]); add('leave-one-out arms on ties prove nothing', rc == 1)
    add('wilson: 8 of 40, 14 of 80, 19 of 120 reach 10 percent, one less does not', all(M.wilson_low(a, n) >= 0.0995 and M.wilson_low(a - 1, n) < 0.10 for a, n in ((8, 40), (14, 80), (19, 120))))
    return R


# (name, what it breaks): the source of mode_accept.py with one text swapped
MUT = [('margin', 'a <= DEC_MARGIN or', 'False or'), ('se', 'or a <= DEC_SE * se for', 'for'), ('sign', 'a <= DEC_MARGIN', '-a <= DEC_MARGIN'), ('any', 'if any(a <=', 'if all(a <='), ('half', 'others[second][2]', 'others[second][0]'), ('second', 'second = min(', 'second = max('), ('abs', 'need[own] += v', 'need[own] += abs(v)'),
       ('lower', 's.needs[m][0] > SCEN_MARGIN and s.needs[m][1] > 0.0', 's.needs[m][0] > SCEN_MARGIN'), ('scen margin', 's.needs[m][0] > SCEN_MARGIN and s', 's.needs[m][0] > 0 and s'), ('luck', 'if n >= MIN_LUCK and s.needs', 'if s.needs'), ('tied', 'all(s.needs[m][2] < SCEN_MARGIN', 'any(s.needs[m][2] < SCEN_MARGIN'), ('tied is resolved', "res = [k for k in ids if sc[k].state == 'resolved']", "res = [k for k in ids if sc[k].state in ('resolved', 'tied')]"),
       ('forty', 'len(res) >= MIN_RESOLVED; P', 'len(res) >= 30; P'), ('point share', 'min(wilson_low(sum(hits), len(hits)), boot', 'max(sum(hits) / len(hits), 0 * boot'), ('no cluster', 'for h, c in zip(hits, cluster): by[c].append(h)', 'for i, (h, c) in enumerate(zip(hits, cluster)): by[i].append(h)'), ('habit', '>= MAX_ONLY for m', '>= 2 for m'), ('base', 'b = inside >= BASE_SHARE * len(ids)', 'b = True'), ('few', 'P(False, \'luck\', \'%d scenarios have under %d lucks\' % (len(few), MIN_LUCK)); ok = False', 'pass'),
       ('caps', "+ (['kite'] if 'kite' in c else [])", "+ ['kite']"), ('never kite', "+ (['kite'] if 'kite' in c else [])", '+ []'), ('blade line', "if not (m == 'kite' and cls in MELEE)]", ']'), ('strikes', "and not (key in run.kite and strikes_ok(run.kite[key]))", 'and False'), ('strike gap', "and float(k['gap']) <= MELEE_KITE['gap']", ''), ('strike share', "float(k['dmgShare']) >= MELEE_KITE['dmgShare'] and", ''), ('strike end', "and float(k['longest']) <= MELEE_KITE['longest']", ''), ('strike dead', "and int(k['dead']) / ep <= MELEE_KITE['deadShare']", ''), ('strikes all', "if own == 'kite' and key[0] in MELEE and", "if own == 'kite' and"),
       ('loo sign', 'agree += mean(x) > 0.0', 'agree += 1'), ('loo flip', 'agree += mean(x) > 0.0', 'agree += mean(x) < 0.0'), ('loo strict', 'agree >= LOO_AGREE * tot\n', 'agree >= tot\n'), ('loo none', 'tot >= LOO_MIN and agree >=', 'agree >='), ('loo drop', 'return ok and b, one', 'return ok, one'),
       ('one rows', 'p1 = len(rows) >=', 'p1 = True or len(rows) >='), ('one share', 'p2 = bool(rr) and not any', 'p2 = True or not any'), ('one loss', 'p3 = whole and up < SCEN_MARGIN', 'p3 = whole'), ('one part', 'whole = bool(ls) and all(', 'whole = bool(ls) and any('), ('one class', 'if cls in ONE_MODE:\n', 'if True:\n'), ('draw', 'if cur.head[\'against\'] != \'-\' and t[2] not in ONE_MODE', 'if False'),
       ('family', "if same['family'] != 'heldout': out.append('VERDICT pilot", "if False: out.append('VERDICT pilot"), ('gen', "if same['gen'] != gen_version()", 'if False'), ('sets', "out.append('SET %s: not played' % st); ok = False", "out.append('SET %s: not played' % st)"), ('unfinished', "if not done: raise ValueError('%s: a run that did not finish' % p)\n    return", 'return'), ('twice', 'if key in cur.fight: raise', 'if False: raise'), ('seeds', "for k in ('seed', 'places', 'drawn'):", "for k in ('places', 'drawn'):")]


def main():
    global M
    src = open(M.__file__).read(); bad = [n for n, ok in rows() if not ok]; total = len(rows())
    print('ROWS %d, failed %d' % (total, len(bad))); [print('  FAILED', b) for b in bad]
    alive = []
    for name, a, b in MUT:
        if a not in src: alive.append(name + ' (text not found)'); continue
        m = type(M)('mut'); m.__file__ = M.__file__; exec(compile(src.replace(a, b), 'mut:' + name, 'exec'), m.__dict__); m.BOOT = 60; keep, M = M, m
        out = sys.stdout; sys.stdout = open(os.devnull, 'w')
        try: caught = any(not ok for _, ok in rows())
        except Exception: caught = True
        sys.stdout = out; M = keep
        if not caught: alive.append(name)
    print('MUTANTS %d, survived %d %s' % (len(MUT), len(alive), alive)); return 1 if bad or alive else 0


if __name__ == '__main__': sys.exit(main())
