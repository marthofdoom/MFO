#!/usr/bin/env python3
"""list_balance/compile_world.py -- resolve the raw world extract into what the IAUS sim reads (sim v2).

  ~/.cache/mfo-list-balance/<name>.world.json   (extract_world.py: raw winning records)
  ~/.cache/mfo-list-balance/<name>.json         (extract.py: the taught spell pool, potions)      [optional]
  ~/.cache/mfo-list-balance/gmst_defaults.json  (gmst_defaults.py: the executable's code defaults) [optional]
    -> ~/.cache/mfo-list-balance/<name>.sim.json  (never committed; TEST INPUT ONLY)

What is resolved here (every formula and its proof level is in Docs/research/2026-11/iaus-sim-v2.md):
  - GMST layering: code default < the list's winning records.
  - every hostile placed actor at 6 player-level buckets x 2 leveled-list draws: template chain per ACBS template flag,
    LVLN / LVLI picks by level, health / magicka / stamina and skills (the CK auto-calc formula, self-checked against the
    DNAM values the CK stored), best weapon, armor rating, resists and armor from abilities, spells, combat style.
  - encounters: the hostile actors placed together in one cell, with their encounter zone's level clamp.
  - follower tables: playable races, the three combat class weight rows, gear ladders, potions, perk multipliers.

usage: compile_world.py --name LoreRim
"""
import argparse, json, os, random, sys

AV_1H, AV_2H, AV_BOW, AV_BLOCK, AV_HEAVY, AV_LIGHT = 6, 7, 8, 9, 11, 12
SK = lambda av: av - 6          # skill actor value -> index into the 18-entry skill arrays
BUCKETS = [5, 12, 20, 28, 38, 50]
RESIST_AV = {39: 'armor', 40: 'poison', 41: 'fire', 42: 'shock', 43: 'frost', 44: 'magic'}
SCHOOL = {18: 'Alteration', 19: 'Conjuration', 20: 'Destruction', 21: 'Illusion', 22: 'Restoration'}
DELIVERY = ['Self', 'Touch', 'Aimed', 'TargetActor', 'TargetLocation']
CASTTYPE = ['ConstantEffect', 'FireAndForget', 'Concentration', 'Scroll']


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--name', required=True); ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args(); cache = os.path.expanduser('~/.cache/mfo-list-balance/')
    W = json.load(open(cache + a.name + '.world.json'))
    L = json.load(open(cache + a.name + '.json')) if os.path.exists(cache + a.name + '.json') else {}
    D = json.load(open(cache + 'gmst_defaults.json'))['gmst'] if os.path.exists(cache + 'gmst_defaults.json') else {}
    gm = dict(D); gm.update(W['gmst'])
    G = lambda k, d: gm.get(k, d)
    races, classes, cstyles, weapons, armors, ammo, lvli, lvln, npcs, spells, perks = (W[k] for k in ('races', 'classes', 'cstyles', 'weapons', 'armors', 'ammo', 'lvli', 'lvln', 'npcs', 'spells', 'perks'))
    rng = random.Random(a.seed)
    stats = dict(perkEntries=0, perkEntriesSkippedConds=0, selfcheck=[])

    # ── leveled lists ──
    def pick(entries, flags, level):
        ok = [e for e in entries if e[0] <= level]
        if not ok: ok = [min(entries, key=lambda e: e[0])] if entries else []
        if not ok: return None
        if not flags & 1:   # not "calculate from all levels <= PC": only the highest eligible level
            top = max(e[0] for e in ok); ok = [e for e in ok if e[0] == top]
        return rng.choice(ok)

    def pick_actor(ref, level, depth=0):   # -> npc index or None
        if ref is None or depth > 10: return None
        if ref[0] == 'n': return ref[1]
        l = lvln[ref[1]]
        if not l: return None
        e = pick(l['e'], l['flags'], level)
        return pick_actor([e[1], e[2]], level, depth + 1) if e else None

    def items_of(refs, level, out, depth=0):   # refs: [[kind, idx, count?]...] -> flat [(kind, idx)]
        for r in refs:
            if r[0] in ('w', 'a', 'm'): out.append((r[0], r[1]))
            elif r[0] == 'l' and depth < 8 and lvli[r[1]]:
                l = lvli[r[1]]
                if l['flags'] & 4: items_of([[e[1], e[2]] for e in l['e'] if e[0] <= level], level, out, depth + 1)
                else:
                    e = pick(l['e'], l['flags'], level)
                    if e and rng.random() * 100 >= l['none']: items_of([[e[1], e[2]]], level, out, depth + 1)

    # ── template chain: which record supplies each group ──
    GROUPS = ((0x1, ('race',)), (0x2, ('level', 'cmin', 'cmax', 'pcMult', 'autocalc', 'hpOff', 'mpOff', 'spOff', 'speed', 'skills', 'skillOff', 'hpD', 'mpD', 'spD', 'clas')),
              (0x8, ('spells', 'perks')), (0x10, ('aggr', 'conf', 'csty')), (0x80, ('flags',)), (0x100, ('items', 'outfit')), (0x1000, ('kw',)))

    def effective(i, level, depth=0):
        n = npcs[i]
        if n is None: return None
        eff = dict(n)
        if n['tpl'] and n['tflags'] and depth < 12:
            ti = pick_actor(n['tpl'], level)
            t = effective(ti, level, depth + 1) if ti is not None and ti != i else None
            if t:
                for bit, keys in GROUPS:
                    if n['tflags'] & bit:
                        for k in keys: eff[k] = t[k]
        return eff

    # ── spells ──
    def spell_row(s):   # the list.json spell schema (one C++ classifier reads both sources)
        return dict(edid=s['edid'], cost=s['cost'], castType=CASTTYPE[min(s['castType'], 3)], delivery=DELIVERY[min(s['delivery'], 4)], chargeTime=s['chargeTime'],
                    castDuration=s['castDuration'], range=s['range'], autoCalc=not (s['flags'] & 1), taught=False, recovery=s.get('recovery', 0.0), shout=bool(s.get('shout')),
                    effects=[dict(mag=e['mag'], area=e['area'], dur=e['dur'], hostile=e['hostile'], detrimental=e['detrimental'], skill=SCHOOL.get(e['school'], 'None'), resist=e['resist'],
                                  minSkill=e['minSkill'], archetype=e['archetype'], av=e['av'], delivery=0, baseCost=e['baseCost'], conds=e['conds'], kw=e['kw']) for e in s['effects']])

    def passive(spell_ids, acc):   # constant abilities: resists / armor (ValueModifier 0 / PeakValueModifier 34 on a resist AV, unconditioned)
        for si in spell_ids:
            s = spells[si] if 0 <= si < len(spells) else None
            if not s or s['type'] != 4: continue
            for e in s['effects']:
                if e['archetype'] in (0, 34) and e['av'] in RESIST_AV and not e['conds']:
                    acc[RESIST_AV[e['av']]] = acc.get(RESIST_AV[e['av']], 0.0) + e['mag'] * (-1.0 if e['detrimental'] else 1.0)

    # ── perks: unconditioned multiply entries an NPC carries ──
    def npc_perk_mults(pids):
        atk = pw = 1.0
        for pi in pids:
            p = perks[pi] if 0 <= pi < len(perks) else None
            if not p: continue
            for e in p['entries']:
                if e['type'] != 2 or e.get('fn') != 3 or 'v' not in e: continue
                if e.get('ep') in (35, 28):
                    stats['perkEntries'] += 1
                    if e['conds'] or p['conds']: stats['perkEntriesSkippedConds'] += 1; continue
                    if e['ep'] == 35: atk *= e['v']
                    else: pw *= e['v']
        return atk, pw

    def hms(race, clas, level, off, which):
        w = (clas['hW'], clas['mW'], clas['sW']) if clas else (1, 1, 1); tot = max(1, sum(w))
        base = (race['hp'], race['mp'], race['sp'])[which]
        return base + off + (level - 1) * ((G('fNPCHealthLevelBonus', 5.0) if which == 0 else 0.0) + G('iAVDhmsLevelUp', 10) * w[which] / tot)

    def skills_of(e, race, clas, level):
        if e['skills'] and not e['pcMult'] and not e['autocalc']:
            return [min(100, e['skills'][i] + (e['skillOff'][i] if e['skillOff'] else 0)) for i in range(18)]
        w = clas['skillW'] if clas else [1] * 18; tot = max(1, sum(w)); boost = {b[0]: b[1] for b in race.get('boosts', [])}
        return [min(G('iAVDAutoCalcSkillMax', 100), G('iAVDSkillStart', 15) + boost.get(i + 6, 0) + (level - 1) * G('iAVDSkillsLevelUp', 8) * w[i] / tot + (e['skillOff'][i] if e['skillOff'] else 0)) for i in range(18)]

    def weapon_row(w):
        return dict(name=w['edid'], dmg=w['dmg'], speed=w['speed'], reach=w['reach'], stagger=w['stagger'], anim=w['anim'], skill=w['skill'], crit=w['crit'], silver=w['silver'], value=w['value'])

    def armor_rating(pieces, sk):   # displayed rating: each piece x lerp(fArmorRatingBase, fArmorRatingMax, its skill / 100)
        lo, hi = G('fArmorRatingBase', 1.0), G('fArmorRatingMax', 2.5); ar = 0.0; n = 0; shield = False; seen = {}
        for p in pieces:
            key = p['slots'] & 0x3BF or p['slots']
            if key in seen and seen[key]['ar'] >= p['ar']: continue
            seen[key] = p
        for p in seen.values():
            if p['ar'] <= 0: continue
            s = sk[SK(AV_HEAVY)] if p['type'] == 1 else sk[SK(AV_LIGHT)] if p['type'] == 0 else 0
            ar += p['ar'] * (lo + (hi - lo) * s / 100.0); n += 1; shield |= p['shield']
        return ar, n, shield

    spec_ix = {}; specs = []; fspell_ix = {}; fspells = []

    def spell_ref(si):
        if si not in fspell_ix: fspell_ix[si] = len(fspells); fspells.append(spell_row(spells[si]))
        return fspell_ix[si]

    def build_spec(i, zone_level):
        e = effective(i, zone_level)
        if e is None or e['race'] < 0 or races[e['race']] is None: return -1
        race = races[e['race']]; clas = classes[e['clas']] if e['clas'] >= 0 else None
        level = e['level']
        if e['pcMult']:
            level = max(1, round(zone_level * e['level'] / 1000.0)); level = max(level, e['cmin'])
            if e['cmax']: level = min(level, e['cmax'])
        if e['autocalc'] or e['pcMult'] or not e['hpD']:
            hp, mp, sp = (hms(race, clas, level, e[k], j) for j, k in enumerate(('hpOff', 'mpOff', 'spOff')))
        else: hp, mp, sp = e['hpD'] + e['hpOff'], e['mpD'] + e['mpOff'], e['spD'] + e['spOff']
        sk = skills_of(e, race, clas, level)
        inv = []; items_of(e['items'], level, inv); items_of(e['outfit'], level, inv)
        ws = [weapons[j] for k, j in inv if k == 'w' and weapons[j]]; arm = [armors[j] for k, j in inv if k == 'a' and armors[j]]; am = [ammo[j] for k, j in inv if k == 'm' and ammo[j]]
        melee = max((w for w in ws if w['anim'] in (1, 2, 3, 4, 5, 6)), key=lambda w: w['dmg'], default=None)
        rngd = max((w for w in ws if w['anim'] in (7, 9)), key=lambda w: w['dmg'], default=None)
        staff = next((w for w in ws if w['anim'] == 8), None)
        ar, pieces, shield = armor_rating(arm, sk)
        res = {}; passive(race.get('spells', []), res); passive(e['spells'], res)
        for pi in e['perks']:
            p = perks[pi] if 0 <= pi < len(perks) else None
            if p: passive([en['spell'] for en in p['entries'] if en['type'] == 1 and en.get('spell', -1) >= 0], res)
        atk, pw = npc_perk_mults(e['perks'])
        cast = [spell_ref(si) for si in e['spells'] if 0 <= si < len(spells) and spells[si] and (spells[si]['type'] == 0 or spells[si].get('shout')) and spells[si]['effects']]
        natk = [x for x in race.get('attacks', []) if not (x[2] & 0x4) and x[1] > 0] or race.get('attacks', [])
        cs = cstyles[e['csty']] if e['csty'] >= 0 and cstyles[e['csty']] else None
        kws = set(race.get('kw', [])) | set(e.get('kw', []))
        spec = dict(name=e['edid'], level=level, hp=round(max(1.0, hp), 1), mp=round(max(0.0, mp), 1), sp=round(max(0.0, sp), 1),
                    hpR=race['hpRegen'], mpR=race['mpRegen'], spR=race['spRegen'], skills=[round(x, 1) for x in sk],
                    melee=weapon_row(melee) if melee else None, ranged=weapon_row(rngd) if rngd else None, staff=bool(staff),
                    ammoDmg=max((x['dmg'] for x in am), default=0.0) if rngd else 0.0, shield=shield,
                    unarmed=race['unarmedDmg'], unarmedReach=race['unarmedReach'], atkDmgMult=round(sum(x[0] for x in natk) / len(natk), 3) if natk else 1.0,
                    ar=round(ar + race.get('skinAR', 0.0) + res.get('armor', 0.0), 1), pieces=pieces + (1 if shield else 0),
                    resist={k: round(v, 1) for k, v in res.items() if k != 'armor' and v}, spells=cast[:8],
                    style=dict(general=cs['general'], melee=cs['melee'], close=cs['close'], longRange=cs['longRange'], flags=cs['flags']) if cs else None,
                    conf=e['conf'], aggr=e['aggr'], run=round(race['run'] * (e['speed'] or 100) / 100.0, 1), mass=race['mass'],
                    undead='ActorTypeUndead' in kws, dragon='ActorTypeDragon' in kws, daedra='ActorTypeDaedra' in kws, animal='ActorTypeAnimal' in kws,
                    creature='ActorTypeNPC' not in kws, immunePar='ImmuneParalysis' in kws, race=race['edid'], atkMult=round(atk, 3), powMult=round(pw, 3))
        key = json.dumps(spec, sort_keys=True)
        if key not in spec_ix: spec_ix[key] = len(specs); specs.append(spec)
        return spec_ix[key]

    # self-check of the auto-calc formula against the values the CK stored (static, auto-calc actors with DNAM health)
    chk = 0; bad = 0
    for i, n in enumerate(npcs):
        if not n or n['tpl'] or not n['autocalc'] or n['pcMult'] or not n['hpD'] or n['race'] < 0 or n['clas'] < 0 or not races[n['race']] or not classes[n['clas']]: continue
        want = n['hpD']; got = hms(races[n['race']], classes[n['clas']], n['level'], n['hpOff'], 0); chk += 1
        if abs(got - want) > 1.5: bad += 1
        if chk >= 4000: break
    stats['selfcheck'].append(dict(what='auto-calc health formula vs CK-stored DNAM health (static auto-calc actors)', checked=chk, off_by_more_than_1=bad))

    # the same check for skills
    chk = 0; bad = 0
    for i, n in enumerate(npcs):
        if not n or n['tpl'] or not n['autocalc'] or n['pcMult'] or not n['skills'] or n['race'] < 0 or n['clas'] < 0 or not races[n['race']] or not classes[n['clas']]: continue
        got = skills_of(n, races[n['race']], classes[n['clas']], n['level']); chk += 1
        if any(abs(got[j] - n['skills'][j]) > 1.01 for j in range(18)): bad += 1
        if chk >= 4000: break
    stats['selfcheck'].append(dict(what='auto-calc skill formula vs CK-stored DNAM skills (static auto-calc actors)', checked=chk, off_by_more_than_1=bad))

    encounters = []
    allenc = W['encounters']
    if len(allenc) > 1200: allenc = random.Random(a.seed + 7).sample(allenc, 1200)
    for enc in allenc:
        mem = enc['m']
        if len(mem) > 8:
            mem = sorted(mem, key=lambda m: m[2] ** 2 + m[3] ** 2)[:8]
        z = enc['zone']; variants = []
        for b in BUCKETS:
            zl = b
            if z:
                zmin, zmax, zfl = z
                if zl < zmin and not (zfl & 0x2): zl = zmin
                if zmax and zl > zmax: zl = zmax
            row = []
            for v in range(1):
                grp = []
                for m in mem:
                    ni = pick_actor([m[0], m[1]], zl)
                    si = build_spec(ni, zl) if ni is not None else -1
                    if si >= 0: grp.append([si, m[2], m[3]])
                row.append(grp)
            variants.append(row)
        if any(g for row in variants for g in row):
            encounters.append(dict(cell=enc['cell'], interior=enc['interior'], zone=z, v=variants))

    # ── follower tables ──
    def by_edid(tab, names):
        for nm in names:
            for r in tab:
                if r and r['edid'] == nm: return r
        return None
    fclass = dict(melee=by_edid(classes, ['CombatWarrior1H', 'CombatWarrior2H', 'EncClassBanditMelee']),
                  ranged=by_edid(classes, ['CombatRanger', 'CombatArcher', 'EncClassBanditMissile']),
                  mage=by_edid(classes, ['CombatMageDestruction', 'CombatSorcerer', 'CombatMageElemental', 'EncClassBanditWizard']))

    def ladder(rows, keyf, n=14):
        rows = sorted(rows, key=keyf)
        if len(rows) <= n: return rows
        return [rows[round(i * (len(rows) - 1) / (n - 1))] for i in range(n)]
    usable = [w for w in weapons if w and w['playable'] and not w['ench'] and w['dmg'] > 0 and w['value'] > 0 and w['speed'] > 0]
    wl = {nm: [weapon_row(w) for w in ladder([w for w in usable if w['anim'] in an], lambda w: (w['value'], w['dmg']))]
          for nm, an in (('oneHand', (1, 3, 4)), ('dagger', (2,)), ('twoHand', (5, 6)), ('bow', (7,)), ('crossbow', (9,)))}
    ua = [x for x in armors if x and x['playable'] and not x['ench'] and x['ar'] > 0 and x['value'] > 0]
    al = {}
    for tn, t in (('heavy', 1), ('light', 0)):
        for sn, mask in (('body', 0x4), ('head', 0x3), ('hands', 0x8), ('feet', 0x80), ('shield', 0x200)):
            al[tn + '_' + sn] = [round(x['ar'], 1) for x in ladder([x for x in ua if x['type'] == t and x['slots'] & mask and (sn == 'shield') == x['shield']], lambda x: (x['value'], x['ar']))]
    arrows = [round(x['dmg'], 1) for x in ladder([x for x in ammo if x and not x['bolt'] and x['dmg'] > 0], lambda x: x['dmg'])]
    bolts = [round(x['dmg'], 1) for x in ladder([x for x in ammo if x and x['bolt'] and x['dmg'] > 0], lambda x: x['dmg'])]

    # perk multipliers by skill threshold (MODEL: the follower holds every playable perk of his weapon / magic tree his skill qualifies
    # for; a ranked chain is "the best entry whose skill requirement he meets")
    ptab = dict(attack={}, power={}, spellMag={}, spellCost={}, armor={})
    for p in perks:
        if not p or not p['playable']: continue
        req = [(c[1], c[4]) for c in p['conds'] if c[0] == 277 and isinstance(c[1], int) and 6 <= c[1] <= 23 and c[3] in (2, 3)]
        if not req: continue
        av, thr = max(req, key=lambda r: r[1])
        for e in p['entries']:
            if e['type'] != 2 or e.get('fn') != 3 or 'v' not in e: continue
            ep = e.get('ep'); v = e['v']
            kws = [c[1] for c in e['conds'] if c[0] in (693, 560, 596) and isinstance(c[1], str) and c[4] == 1.0 and c[3] == 0]
            if ep == 35 and av in (6, 7, 8) and v > 1: ptab['attack'].setdefault(str(av), []).append([thr, v])
            elif ep == 28 and av in (6, 7) and v > 1: ptab['power'].setdefault(str(av), []).append([thr, v])
            elif ep == 29 and 18 <= av <= 22 and v > 1:
                el = next((k for k in kws if k.startswith('MagicDamage')), 'all')
                ptab['spellMag'].setdefault(SCHOOL[av] + ':' + el, []).append([thr, v])
            elif ep == 38 and 18 <= av <= 22 and 0 < v < 1: ptab['spellCost'].setdefault(SCHOOL[av], []).append([thr, v])
            elif ep == 85 and av in (11, 12) and v > 1: ptab['armor'].setdefault(str(av), []).append([thr, v])
    for t in ptab.values():
        for k in t: t[k] = sorted(t[k])

    pots = {}
    for p in W.get('potions', []):
        fx = p['effects']
        if len(fx) == 1 and fx[0]['av'] in (24, 25, 26) and fx[0]['archetype'] in (0, 34) and not fx[0]['hostile'] and not fx[0]['detrimental'] and 0 < fx[0]['mag'] <= 2000:
            pots.setdefault(str(fx[0]['av']), []).append([round(fx[0]['mag'], 1), fx[0]['dur']])
    for k in pots: pots[k] = ladder(pots[k], lambda x: x[0] * max(1, x[1]), 10)

    frace = [dict(edid=r['edid'], hp=r['hp'], mp=r['mp'], sp=r['sp'], hpR=r['hpRegen'], mpR=r['mpRegen'], spR=r['spRegen'], unarmed=r['unarmedDmg'], run=r['run'], mass=r['mass'],
                  boosts=r.get('boosts', [])) for r in races if r and r['playable'] and r['hp'] > 0 and 'Child' not in r['edid'] and 'Vampire' not in r['edid']]
    out = dict(list=W['list'], gmst={k: v for k, v in sorted(gm.items()) if isinstance(v, (int, float))}, specs=specs, encounters=encounters, buckets=BUCKETS,
               follower=dict(races=frace, classes=fclass, weapons=wl, armor=al, arrows=arrows, bolts=bolts, perks=ptab, potions=pots),
               spells=[s for s in L.get('spells', [])], foeSpells=fspells, potionsRaw=len(W.get('potions', [])), stats=stats)
    path = cache + a.name + '.sim.json'
    with open(path, 'w') as f: json.dump(out, f, separators=(',', ':'))
    print(path, 'specs', len(specs), 'encounters', len(encounters), 'spells', len(out['spells']), 'MB %.1f' % (os.path.getsize(path) / 1e6))
    print('  ', stats)
    print('   follower classes:', {k: (v['edid'] if v else None) for k, v in fclass.items()}, 'weapon ladders:', {k: len(v) for k, v in wl.items()},
          'perk tables:', {k: {kk: len(vv) for kk, vv in v.items()} for k, v in ptab.items()})


if __name__ == '__main__':
    main()
