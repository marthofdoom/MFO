#!/usr/bin/env python3
"""list_balance/extract_world.py -- the WORLD half of the list balance (IAUS sim v2).

Reads an installed MO2 list's WINNING actor / gear / encounter records into one JSON:
  ~/.cache/mfo-list-balance/<name>.world.json      (never committed; TEST INPUT ONLY for tools/iaus_sim)
READ-ONLY on the list (every file is opened 'rb'). Nothing in native/ ever reads this data.

usage: extract_world.py --list /mnt/gaming/modlists/LoreRim --profile Ultra --data "Stock Game/Data"
       extract_world.py --list /mnt/gaming/modlists/Tuxborn --profile "Tuxborn - Deck" --data "Game Root/Data"

Records (layouts: Docs/research/2026-11/iaus-sim-v2.md "Record layouts", each one self-checked against vanilla
values by --selfcheck): GMST (every numeric), KYWD, FACT (XNAM reactions), RACE, CLAS, CSTY, MOVT, WEAP, ARMO, AMMO,
OTFT, LVLI, LVLN, NPC_ (templates resolved per ACBS template flag), SPEL / SHOU / MGEF for the spells actors and
races carry, ALCH, PERK (entry points + conditions), ECZN, and CELL / WRLD children ACHR (placed actors), clustered
into encounters per cell.
"""
import argparse, json, os, struct, sys, time, zlib

_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _here); sys.path.insert(0, os.path.join(_here, '..'))
import esp_inspect as ei
from extract import find_plugins, Blob, f32, u32, i32

FLAT = {b'GMST', b'KYWD', b'FACT', b'RACE', b'CLAS', b'CSTY', b'MOVT', b'WEAP', b'ARMO', b'AMMO', b'OTFT', b'LVLI',
        b'LVLN', b'NPC_', b'SPEL', b'SHOU', b'MGEF', b'ALCH', b'PERK', b'ECZN', b'LCTN', b'ENCH'}
u16 = lambda b, o: struct.unpack_from('<H', b, o)[0]
i16 = lambda b, o: struct.unpack_from('<h', b, o)[0]
PLAYER_FACTION = ('skyrim.esm', 0xDB1)
PLAYER_REF_BASE = ('skyrim.esm', 0x7)


def scan(path):
    """-> masters, light, flat [(sig, fid, flags, subs)], cells [(fid, world, subs)], achr [(fid, cell, world, subs)]"""
    with open(path, 'rb') as f: data = f.read()
    blob = Blob(data); n = len(data); flat = []; cells = []; achr = []; masters = []
    light = path.lower().endswith('.esl')

    def rec_subs(sig, fid, fl, o, sz):
        return ei.Record(sig, fid, fl, o, sz, blob, '').subs()

    def walk(o, end, cell, world):
        while o + 24 <= end:
            sig = data[o:o + 4]; sz = u32(data, o + 4)
            if sig == b'GRUP':
                gt = i32(data, o + 12); lab = u32(data, o + 8); c2, w2 = cell, world
                if gt == 1: w2 = lab
                elif gt in (6, 8, 9, 10): c2 = lab
                if sz < 24: return
                walk(o + 24, min(o + sz, end), c2, w2); o += sz; continue
            if sig == b'ACHR' or sig == b'CELL' or sig == b'WRLD':
                fl, fid = struct.unpack_from('<II', data, o + 8)
                if sig == b'WRLD': world = fid
                elif sig == b'CELL':
                    cell = fid
                    try: cells.append((fid, world, fl, rec_subs('CELL', fid, fl, o, sz)))
                    except Exception: pass
                elif not fl & 0x820:   # deleted / initially disabled: no fight there
                    try: achr.append((fid, cell, world, rec_subs('ACHR', fid, fl, o, sz)))
                    except Exception: pass
                else: achr.append((fid, cell, world, None))   # a later plugin disabled it: must still win
            o += 24 + sz

    off = 0
    while off + 24 <= n:
        sig = data[off:off + 4]; size = u32(data, off + 4)
        if sig == b'GRUP':
            lab = data[off + 8:off + 12]
            if lab in FLAT:
                end = off + size; o = off + 24
                while o + 24 <= end:
                    s2 = data[o:o + 4]; sz = u32(data, o + 4)
                    if s2 == b'GRUP': o += sz; continue
                    fl, fid = struct.unpack_from('<II', data, o + 8)
                    if not fl & 0x20:
                        try: flat.append((s2.decode(), fid, fl, rec_subs(s2.decode(), fid, fl, o, sz)))
                        except Exception: pass
                    o += 24 + sz
            elif lab in (b'CELL', b'WRLD'):
                walk(off + 24, off + size, 0, 0)
            off += size; continue
        if sig == b'TES4':
            if u32(data, off + 8) & 0x200: light = True
            for t, d in ei.Record('TES4', 0, 0, off, size, blob, '').subs():
                if t == 'MAST': masters.append(ei._zstr(d))
        off += 24 + size
    return masters, light, flat, cells, achr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--list', required=True); ap.add_argument('--profile', required=True)
    ap.add_argument('--data', action='append', default=[]); ap.add_argument('--out'); ap.add_argument('--name')
    ap.add_argument('--vanilla', action='store_true', help='only the five base ESMs of this install (the unmodded reference set, name Vanilla)')
    ap.add_argument('--selfcheck', action='store_true', help='print the vanilla layout self-check rows and exit 1 on a miss')
    a = ap.parse_args()
    name = a.name or os.path.basename(a.list.rstrip('/'))
    order, where = find_plugins(a.list, a.profile, a.data)
    if a.vanilla: order = order[:5]; name = a.name or 'Vanilla'
    win = {}; cellw = {}; achrw = {}; missing = 0; t0 = time.time()
    for pl in order:
        p = where.get(pl.lower())
        if not p: missing += 1; continue
        try: masters, light, flat, cells, achr = scan(p)
        except Exception as e:
            print('skip', pl, e, file=sys.stderr); continue
        names = [m.lower() for m in masters] + [pl.lower()]

        def key(fid, names=names, light=light, pl=pl):
            mi = min(fid >> 24, len(names) - 1); owner = names[mi]
            return owner, fid & (0xFFF if (light and owner == pl.lower()) else 0xFFFFFF)
        for sig, fid, fl, subs in flat: win[(sig,) + key(fid)] = (key, fl, subs)
        for fid, world, fl, subs in cells: cellw[key(fid)] = (key, key(world) if world else None, subs)
        for fid, cell, world, subs in achr: achrw[key(fid)] = (key, key(cell) if cell else None, key(world) if world else None, subs)
    print('scanned %d plugins in %.0fs: %d flat, %d cells, %d achr' % (len(order), time.time() - t0, len(win), len(cellw), len(achrw)), file=sys.stderr)

    sub = lambda subs, t: next((d for k, d in subs if k == t), None)
    subsall = lambda subs, t: [d for k, d in subs if k == t]
    edid = lambda subs: ei._zstr(sub(subs, 'EDID') or b'')
    by = {}
    for (sig, owner, loc), v in win.items(): by.setdefault(sig, {})[(owner, loc)] = v

    def kinds(k):   # which record type a key is
        return [s for s in by if k in by[s]]

    kw = {k: edid(v[2]) for k, v in by.get('KYWD', {}).items()}

    def kwlist(key, subs):
        d = sub(subs, 'KWDA')
        return [kw.get(key(u32(d, i)), '') for i in range(0, len(d) - 3, 4)] if d else []

    # ── GMST ──
    gmst = {}
    for k, (key, fl, subs) in by.get('GMST', {}).items():
        e = edid(subs); d = sub(subs, 'DATA')
        if not e or d is None or len(d) < 4: continue
        if e[0] == 'f': gmst[e] = f32(d, 0)
        elif e[0] in 'ib': gmst[e] = i32(d, 0)

    # ── MGEF ──
    mgef = {}
    for k, (key, fl, subs) in by.get('MGEF', {}).items():
        d = sub(subs, 'DATA')
        if d is None or len(d) < 88: continue
        mgef[k] = dict(fl=u32(d, 0), baseCost=f32(d, 4), school=i32(d, 12), resist=i32(d, 16), minSkill=u32(d, 40),
                       arch=u32(d, 64), av=i32(d, 68), castType=u32(d, 80), delivery=u32(d, 84), av2=i32(d, 88) if len(d) >= 92 else -1,
                       kw=[x for x in kwlist(key, subs) if x], edid=edid(subs))

    def effects(key, subs):
        out = []; cur = None
        for t, d in subs:
            if t == 'EFID':
                m = mgef.get(key(u32(d, 0))); cur = None
                if m:
                    cur = dict(mag=0.0, area=0, dur=0, hostile=bool(m['fl'] & 1), detrimental=bool(m['fl'] & 4), recover=bool(m['fl'] & 2),
                               school=m['school'], resist=m['resist'], minSkill=m['minSkill'], archetype=m['arch'], av=m['av'],
                               baseCost=m['baseCost'], kw=m['kw'], ed=m['edid'], conds=0)
                    out.append(cur)
            elif t == 'EFIT' and cur is not None and len(d) >= 12: cur['mag'], cur['area'], cur['dur'] = f32(d, 0), u32(d, 4), u32(d, 8)
            elif t == 'CTDA' and cur is not None: cur['conds'] += 1
        return out

    # ── index tables (only what is referenced gets exported) ──
    tabs = {s: {} for s in ('race', 'clas', 'csty', 'weap', 'armo', 'ammo', 'lvli', 'lvln', 'npc', 'spel', 'perk')}
    rows = {s: [] for s in tabs}

    def idx(tab, k, build):
        t = tabs[tab]
        if k in t: return t[k]
        t[k] = len(rows[tab]); rows[tab].append(None)
        try: r = build(k)
        except Exception as ex:
            print('bad', tab, k, ex, file=sys.stderr); r = None
        rows[tab][t[k]] = r
        return t[k]

    def b_spel(k):
        if k in by.get('SHOU', {}):   # a shout: its strongest word's spell, the recovery as its cooldown
            key, fl, subs = by['SHOU'][k]; words = [d for d in subsall(subs, 'SNAM') if len(d) >= 12]
            if not words: return None
            w = words[-1]; sk = key(u32(w, 4))
            if sk not in by.get('SPEL', {}): return None
            r = b_spel(sk)
            if r: r = dict(r, shout=True, recovery=f32(w, 8), edid=edid(subs))
            return r
        key, fl, subs = by['SPEL'][k]; d = sub(subs, 'SPIT')
        if d is None or len(d) < 36: return None
        return dict(edid=edid(subs), cost=u32(d, 0), flags=u32(d, 4), type=u32(d, 8), chargeTime=f32(d, 12), castType=u32(d, 16),
                    delivery=u32(d, 20), castDuration=f32(d, 24), range=f32(d, 28), effects=effects(key, subs))

    def spel_ref(k): return idx('spel', k, b_spel) if (k in by.get('SPEL', {}) or k in by.get('SHOU', {})) else -1

    movt = {}
    for k, (key, fl, subs) in by.get('MOVT', {}).items():
        d = sub(subs, 'SPED')
        if d and len(d) >= 40: movt[k] = (f32(d, 16), f32(d, 20), edid(subs))   # forward walk, forward run

    def race_move(redid, kws):
        # The engine binds a race to its movement types through the behaviour graph's state names, which no record
        # states. MODEL: the MOVT whose EDID starts with the race's name stem; humanoids (ActorTypeNPC) use NPC_1HM_MT.
        stem = redid.replace('Race', '').replace('Vampire', '')
        if True:
            best = None
            for k2, (w, r, e) in movt.items():
                st = e.split('_')[0]
                if r > 0 and len(st) >= 3 and (stem.lower().startswith(st.lower()) or st.lower().startswith(stem.lower()) and len(stem) >= 4):
                    pr = (('Combat' in e or 'Run' in e) * 2 + ('1HM' in e or 'Default' in e), r)
                    if best is None or pr > best[0]: best = (pr, w, r, e)
            if best: return best[1], best[2], best[3]
        for k2, (w, r, e) in movt.items():
            if e == 'NPC_1HM_MT': return w, r, e
        return 80.0, 370.0, ''

    def armor_rating_of_skin(key, fid):
        k = key(fid)
        if k in by.get('ARMO', {}):
            d = sub(by['ARMO'][k][2], 'DNAM')
            return i32(d, 0) / 100.0 if d and len(d) >= 4 else 0.0
        return 0.0

    def b_race(k):
        key, fl, subs = by['RACE'][k]; d = sub(subs, 'DATA')
        if d is None or len(d) < 128: return None
        walk, run, mt = race_move(edid(subs), kwlist(key, subs))
        skin = sub(subs, 'WNAM')
        atk = []   # ATKD: damageMult f32 @0, chance f32 @4, spell @8, flags u32 @12 (0x4 power), angle @16, strikeAngle @20, stagger @24, type @28, knockdown @32, recovery @36, staminaMult @40
        for t, dd in subs:
            if t == 'ATKD' and len(dd) >= 44: atk.append([round(f32(dd, 0), 3), round(f32(dd, 4), 3), u32(dd, 12), round(f32(dd, 24), 3), round(f32(dd, 36), 3), round(f32(dd, 40), 3)])
        return dict(edid=edid(subs), playable=bool(u32(d, 32) & 1), flags=u32(d, 32), boosts=[[d[i * 2], d[i * 2 + 1]] for i in range(7) if d[i * 2] < 0x80 and d[i * 2 + 1]], hp=f32(d, 36), mp=f32(d, 40), sp=f32(d, 44),
                    mass=f32(d, 52), size=u32(d, 64), hpRegen=f32(d, 84), mpRegen=f32(d, 88), spRegen=f32(d, 92),
                    unarmedDmg=f32(d, 96), unarmedReach=f32(d, 100), run=run, walk=walk, movt=mt,
                    skinAR=armor_rating_of_skin(key, u32(skin, 0)) if skin and len(skin) >= 4 else 0.0,
                    kw=[x for x in kwlist(key, subs) if x],
                    spells=[s for s in (spel_ref(key(u32(dd, 0))) for dd in subsall(subs, 'SPLO') if len(dd) >= 4) if s >= 0],
                    attacks=atk)

    def b_clas(k):
        key, fl, subs = by['CLAS'][k]; d = sub(subs, 'DATA')
        if d is None or len(d) < 36: return None
        return dict(edid=edid(subs), skillW=list(d[6:24]), hW=d[32], mW=d[33], sW=d[34])

    def b_csty(k):
        key, fl, subs = by['CSTY'][k]
        g = sub(subs, 'CSGD') or b''; me = sub(subs, 'CSME') or b''; cr = sub(subs, 'CSCR') or b''; lr = sub(subs, 'CSLR') or b''; dt = sub(subs, 'DATA') or b''
        fl_ = lambda b, n: [round(f32(b, i * 4), 3) for i in range(min(n, len(b) // 4))]
        return dict(edid=edid(subs), general=fl_(g, 10), melee=fl_(me, 8), close=fl_(cr, 4), longRange=fl_(lr, 1), flags=u32(dt, 0) if len(dt) >= 4 else 0)

    def b_weap(k):
        key, fl, subs = by['WEAP'][k]; d = sub(subs, 'DATA'); n = sub(subs, 'DNAM')
        if d is None or n is None or len(d) < 10 or len(n) < 100: return None
        kws = kwlist(key, subs); cr = sub(subs, 'CRDT')
        return dict(edid=edid(subs), value=u32(d, 0), weight=f32(d, 4), dmg=u16(d, 8), anim=n[0], speed=f32(n, 4), reach=f32(n, 8),
                    flags=u16(n, 12), flags2=u32(n, 40), minRange=f32(n, 28), maxRange=f32(n, 32), skill=i32(n, 76), stagger=f32(n, 96),
                    crit=u16(cr, 0) if cr and len(cr) >= 2 else 0, ench=sub(subs, 'EITM') is not None, silver='WeapMaterialSilver' in kws,
                    playable=not (u16(n, 12) & 0x80) and not (fl & 0x4), kw=[x for x in kws if x])

    def b_armo(k):
        key, fl, subs = by['ARMO'][k]; n = sub(subs, 'DNAM'); b2 = sub(subs, 'BOD2'); bt = sub(subs, 'BODT'); d = sub(subs, 'DATA')
        slots = u32(b2, 0) if b2 and len(b2) >= 8 else u32(bt, 0) if bt and len(bt) >= 4 else 0
        typ = u32(b2, 4) if b2 and len(b2) >= 8 else u32(bt, 8) if bt and len(bt) >= 12 else 2
        kws = kwlist(key, subs)
        return dict(edid=edid(subs), ar=i32(n, 0) / 100.0 if n and len(n) >= 4 else 0.0, slots=slots, type=typ, value=i32(d, 0) if d and len(d) >= 8 else 0,
                    shield='ArmorShield' in kws, ench=sub(subs, 'EITM') is not None, playable=not (fl & 0x4), kw=[x for x in kws if x])

    def b_ammo(k):
        key, fl, subs = by['AMMO'][k]; d = sub(subs, 'DATA')
        if d is None or len(d) < 16: return None
        return dict(edid=edid(subs), dmg=f32(d, 8), bolt=not (u32(d, 4) & 0x4), value=u32(d, 12), kw=[x for x in kwlist(key, subs) if x])

    def item_ref(k):
        if k in by.get('WEAP', {}): return ['w', idx('weap', k, b_weap)]
        if k in by.get('ARMO', {}): return ['a', idx('armo', k, b_armo)]
        if k in by.get('AMMO', {}): return ['m', idx('ammo', k, b_ammo)]
        if k in by.get('LVLI', {}): return ['l', idx('lvli', k, b_lvli)]
        if k in by.get('ALCH', {}): return ['p', 0]
        return None

    def lvl_entries(key, subs, ref):
        out = []
        for d in subsall(subs, 'LVLO'):
            if len(d) < 12: continue
            r = ref(key(u32(d, 4)))
            if r: out.append([u16(d, 0), r[0], r[1], u16(d, 8)])
        return out

    def b_lvli(k):
        key, fl, subs = by['LVLI'][k]; f = sub(subs, 'LVLF'); c = sub(subs, 'LVLD')
        return dict(flags=f[0] if f else 0, none=c[0] if c else 0, e=lvl_entries(key, subs, item_ref))

    def actor_ref(k):
        if k in by.get('NPC_', {}): return ['n', idx('npc', k, b_npc)]
        if k in by.get('LVLN', {}): return ['l', idx('lvln', k, b_lvln)]
        return None

    def b_lvln(k):
        key, fl, subs = by['LVLN'][k]; f = sub(subs, 'LVLF'); c = sub(subs, 'LVLD')
        return dict(edid=edid(subs), flags=f[0] if f else 0, none=c[0] if c else 0, e=lvl_entries(key, subs, actor_ref))

    # faction hostility to the player: an XNAM "enemy" (combat reaction 1) in either direction
    fact_x = {}
    for k, (key, fl, subs) in by.get('FACT', {}).items():
        fact_x[k] = {key(u32(d, 0)): u32(d, 8) for d in subsall(subs, 'XNAM') if len(d) >= 12}
    player_enemies = {f for f, r in fact_x.get(PLAYER_FACTION, {}).items() if r == 1}

    KW_FN = (560, 682, 693, 699, 722, 596, 660, 501)

    def any_edid(k):
        for sg in ('WEAP', 'ARMO', 'NPC_', 'SPEL', 'RACE', 'AMMO', 'ENCH', 'MGEF', 'KYWD'):
            if k in by.get(sg, {}): return edid(by[sg][k][2])
        return '?'

    def b_perk(k):
        key, fl, subs = by['PERK'][k]; out = dict(edid=edid(subs), conds=[], entries=[], playable=False, ranks=1)
        cur = None; tab = 0; seen_data = False

        def ctda(d):
            fn = u16(d, 8); p1 = u32(d, 12); r = p1
            if fn in KW_FN: r = kw.get(key(p1), '?')
            elif fn == 448: r = perk_ref(key(p1))
            elif fn == 214: r = (mgef.get(key(p1)) or {}).get('edid', '?')
            elif fn in (72, 182, 69, 264, 570): r = any_edid(key(p1))
            return [fn, r, u32(d, 16), d[0] >> 5, round(f32(d, 4), 4), d[0] & 1, tab]
        for t, d in subs:
            if t == 'PRKE' and len(d) >= 3: cur = dict(type=d[0], rank=d[1], conds=[]); tab = 0
            elif t == 'PRKF':
                if cur and cur['type'] in (1, 2): out['entries'].append(cur)
                cur = None
            elif t == 'DATA':
                if cur is None and not seen_data and len(d) >= 5: seen_data = True; out['ranks'] = d[2]; out['playable'] = bool(d[3])
                elif cur is not None and cur['type'] == 2 and len(d) >= 3: cur['ep'], cur['fn'] = d[0], d[1]
                elif cur is not None and cur['type'] == 1 and len(d) >= 4: cur['spell'] = spel_ref(key(u32(d, 0)))
            elif t == 'PRKC' and cur is not None: tab = d[0]
            elif t == 'CTDA' and len(d) >= 32: (cur['conds'] if cur is not None else out['conds']).append(ctda(d))
            elif t == 'EPFD' and cur is not None:
                if len(d) == 4: cur['v'] = round(f32(d, 0), 5)
                elif len(d) == 8: cur['v'] = round(f32(d, 0), 5); cur['v2'] = round(f32(d, 4), 5)
            elif t == 'NNAM' and cur is None and len(d) >= 4: out['next'] = key(u32(d, 0))
        return out

    def perk_ref(k): return idx('perk', k, b_perk) if k in by.get('PERK', {}) else -1

    raw_npc = {}

    def raw(k):
        if k in raw_npc: return raw_npc[k]
        key, fl, subs = by['NPC_'][k]; a = sub(subs, 'ACBS'); r = dict(k=k, key=key)
        if a is None or len(a) < 24: raw_npc[k] = None; return None
        r['base'] = dict(flags=u32(a, 0), edid=edid(subs))
        r['stats'] = dict(mpOff=i16(a, 4), spOff=i16(a, 6), level=u16(a, 8), cmin=u16(a, 10), cmax=u16(a, 12), speed=u16(a, 14), hpOff=i16(a, 20),
                          autocalc=bool(u32(a, 0) & 0x10), pcMult=bool(u32(a, 0) & 0x80))
        d = sub(subs, 'DNAM')
        if d and len(d) >= 42: r['stats'].update(skills=list(d[0:18]), skillOff=list(d[18:36]), hpD=u16(d, 36), mpD=u16(d, 38), spD=u16(d, 40))
        c = sub(subs, 'CNAM'); r['stats']['clas'] = key(u32(c, 0)) if c and len(c) >= 4 else None
        rn = sub(subs, 'RNAM'); r['traits'] = dict(race=key(u32(rn, 0)) if rn and len(rn) >= 4 else None)
        ai = sub(subs, 'AIDT'); z = sub(subs, 'ZNAM')
        r['ai'] = dict(aggr=ai[0] if ai else 0, conf=ai[1] if ai and len(ai) > 1 else 2, assist=ai[5] if ai and len(ai) > 5 else 0,
                       csty=key(u32(z, 0)) if z and len(z) >= 4 else None)
        r['factions'] = [key(u32(dd, 0)) for dd in subsall(subs, 'SNAM') if len(dd) >= 4]
        r['spells'] = dict(spells=[key(u32(dd, 0)) for dd in subsall(subs, 'SPLO') if len(dd) >= 4],
                           perks=[key(u32(dd, 0)) for dd in subsall(subs, 'PRKR') if len(dd) >= 4])
        do = sub(subs, 'DOFT')
        r['inv'] = dict(items=[(key(u32(dd, 0)), i32(dd, 4)) for dd in subsall(subs, 'CNTO') if len(dd) >= 8], outfit=key(u32(do, 0)) if do and len(do) >= 4 else None)
        r['kw'] = kwlist(key, subs)
        t = sub(subs, 'TPLT'); r['tpl'] = key(u32(t, 0)) if t and len(t) >= 4 and u32(t, 0) else None; r['tflags'] = u16(a, 18)
        raw_npc[k] = r; return r

    def first_npc(k, depth=0):   # a template that is a leveled list: its first actor (MODEL, see the doc)
        if depth > 8: return None
        if k in by.get('NPC_', {}): return k
        if k in by.get('LVLN', {}):
            key, fl, subs = by['LVLN'][k]
            for d in subsall(subs, 'LVLO'):
                if len(d) >= 12:
                    r = first_npc(key(u32(d, 4)), depth + 1)
                    if r: return r
        return None

    resolved = {}

    def resolve(k, depth=0):
        if k in resolved: return resolved[k]
        r = raw(k)
        if r is None: return None
        out = {g: r[g] for g in ('base', 'stats', 'traits', 'ai', 'factions', 'spells', 'inv', 'kw')}
        if r['tpl'] and r['tflags'] and depth < 12:
            tk = first_npc(r['tpl']); t = resolve(tk, depth + 1) if tk and tk != k else None
            if t:
                for bit, g in ((0x1, 'traits'), (0x2, 'stats'), (0x4, 'factions'), (0x8, 'spells'), (0x10, 'ai'), (0x80, 'base'), (0x100, 'inv'), (0x1000, 'kw')):
                    if r['tflags'] & bit: out[g] = t[g]
                if r['tflags'] & 0x80: out['base'] = dict(t['base'], edid=r['base']['edid'])
        resolved[k] = out; return out

    def outfit_items(k):
        if not k or k not in by.get('OTFT', {}): return []
        key, fl, subs = by['OTFT'][k]; d = sub(subs, 'INAM') or b''
        return [r for r in (item_ref(key(u32(d, i))) for i in range(0, len(d) - 3, 4)) if r]

    def b_npc(k):
        # OWN fields (unresolved) + the template reference: the sim resolves the chain per ACBS template flag at the
        # encounter's level (a template is usually a LVLN picked by level). hostile is resolved here (factions do not vary by level).
        own = raw(k); r = resolve(k)
        if own is None or r is None: return None
        s = own['stats']; facs = set(r['factions'])
        # Aggression (AIDT byte 0): 1 Aggressive attacks ENEMIES, 2 Very Aggressive attacks enemies and NEUTRALS, 3 Frenzied anyone.
        # Faction reaction (XNAM): 0 neutral, 1 enemy, 2 ally, 3 friend, read in both directions against PlayerFaction.
        rel = [fact_x.get(f, {}).get(PLAYER_FACTION) for f in facs] + [fact_x.get(PLAYER_FACTION, {}).get(f) for f in facs]
        enemy = 1 in rel; friendly = 2 in rel or 3 in rel; ag = r['ai']['aggr']
        hostile = ag >= 3 or (ag >= 1 and enemy) or (ag == 2 and not friendly)
        rk = own['traits']['race']; tpl = actor_ref(own['tpl']) if own['tpl'] and own['tpl'] != k else None
        return dict(edid=own['base']['edid'], flags=own['base']['flags'], tpl=tpl, tflags=own['tflags'] if tpl else 0,
                    level=s['level'], cmin=s['cmin'], cmax=s['cmax'], pcMult=s['pcMult'], autocalc=s['autocalc'],
                    hpOff=s['hpOff'], mpOff=s['mpOff'], spOff=s['spOff'], speed=s['speed'], skills=s.get('skills'), skillOff=s.get('skillOff'),
                    hpD=s.get('hpD', 0), mpD=s.get('mpD', 0), spD=s.get('spD', 0),
                    race=idx('race', rk, b_race) if rk in by.get('RACE', {}) else -1,
                    clas=idx('clas', s['clas'], b_clas) if s['clas'] in by.get('CLAS', {}) else -1,
                    csty=idx('csty', own['ai']['csty'], b_csty) if own['ai']['csty'] in by.get('CSTY', {}) else -1,
                    aggr=own['ai']['aggr'], conf=own['ai']['conf'], assist=own['ai']['assist'], hostile=hostile,
                    spells=[x for x in (spel_ref(sk) for sk in own['spells']['spells']) if x >= 0],
                    perks=[x for x in (perk_ref(pk) for pk in own['spells']['perks']) if x >= 0],
                    items=[x + [c] for x, c in ((item_ref(ik), c) for ik, c in own['inv']['items']) if x], outfit=outfit_items(own['inv']['outfit']),
                    kw=[x for x in own['kw'] if x])

    hmemo = {}

    def khostile(k, depth=0):   # hostility of a placed base (NPC_ or LVLN) from keys, before anything is indexed
        if k in hmemo: return hmemo[k]
        h = False
        if k in by.get('NPC_', {}):
            r = resolve(k)
            if r:
                facs = set(r['factions'])
                rel = [fact_x.get(f, {}).get(PLAYER_FACTION) for f in facs] + [fact_x.get(PLAYER_FACTION, {}).get(f) for f in facs]
                ag = r['ai']['aggr']; h = ag >= 3 or (ag >= 1 and 1 in rel) or (ag == 2 and 2 not in rel and 3 not in rel)
        elif k in by.get('LVLN', {}) and depth < 6:
            key, fl, subs = by['LVLN'][k]
            h = any(khostile(key(u32(d, 4)), depth + 1) for d in subsall(subs, 'LVLO') if len(d) >= 12)
        hmemo[k] = h; return h

    # ── placed actors -> encounters (clusters of hostile actors inside one cell) ──
    ecz = {}
    for k, (key, fl, subs) in by.get('ECZN', {}).items():
        d = sub(subs, 'DATA')
        if d and len(d) >= 12: ecz[k] = (d[9], d[11], d[10], edid(subs))
    cellinfo = {}
    for k, (key, world, subs) in cellw.items():
        z = sub(subs, 'XEZN'); dt = sub(subs, 'DATA')
        cellinfo[k] = (edid(subs), key(u32(z, 0)) if z and len(z) >= 4 else None, bool(dt and dt[0] & 1))
    wrld_zone = {}
    placed = {}
    for k, (key, cell, world, subs) in achrw.items():
        if subs is None: continue
        nm = sub(subs, 'NAME'); dt = sub(subs, 'DATA')
        if not nm or not dt or len(dt) < 12: continue
        bk = key(u32(nm, 0))
        if bk == PLAYER_REF_BASE: continue
        if not khostile(bk): continue   # only hostile actors are exported (the tables stay small)
        ar = actor_ref(bk)
        if not ar: continue
        z = sub(subs, 'XEZN'); x, y, zz = f32(dt, 0), f32(dt, 4), f32(dt, 8)
        ci = cellinfo.get(cell, ('', None, False))
        gk = (cell,) if ci[2] else (world, int(x // 8192), int(y // 8192))   # exterior: an 8192-unit bin (persistent refs sit in one cell)
        placed.setdefault(gk, []).append((x, y, zz, ar, key(u32(z, 0)) if z and len(z) >= 4 else ci[1], ci[0]))

    def lvln_hostile(i, depth=0):
        r = rows['lvln'][i]
        if not r or depth > 6: return False
        for lv, kind, j, c in r['e']:
            if kind == 'n' and rows['npc'][j] and rows['npc'][j]['hostile']: return True
            if kind == 'l' and lvln_hostile(j, depth + 1): return True
        return False

    def is_hostile(ar):
        return (rows['npc'][ar[1]] or {}).get('hostile', False) if ar[0] == 'n' else lvln_hostile(ar[1])

    encounters = []
    for gk, lst in placed.items():
        hs = [p for p in lst if is_hostile(p[3])]
        if not hs: continue
        par = list(range(len(hs)))

        def find(i):
            while par[i] != i: par[i] = par[par[i]]; i = par[i]
            return i
        for i in range(len(hs)):
            for j in range(i + 1, len(hs)):
                if (hs[i][0] - hs[j][0]) ** 2 + (hs[i][1] - hs[j][1]) ** 2 + (hs[i][2] - hs[j][2]) ** 2 < 1800.0 ** 2: par[find(i)] = find(j)
        groups = {}
        for i in range(len(hs)): groups.setdefault(find(i), []).append(hs[i])
        for g in groups.values():
            zk = next((p[4] for p in g if p[4]), None); z = ecz.get(zk)
            cx = sum(p[0] for p in g) / len(g); cy = sum(p[1] for p in g) / len(g)
            encounters.append(dict(cell=g[0][5], interior=len(gk) == 1, zone=list(z[:3]) if z else None, zoneEdid=z[3] if z else '',
                                   m=[[p[3][0], p[3][1], round(p[0] - cx), round(p[1] - cy)] for p in g]))

    # follower-usable gear: every playable, unenchanted weapon / armor / ammo (the sim picks a kit by tier from these)
    for k, (key, fl, subs) in by.get('WEAP', {}).items():
        if sub(subs, 'EITM') is None and sub(subs, 'CNAM') is None: idx('weap', k, b_weap)
    for k, (key, fl, subs) in by.get('ARMO', {}).items():
        if sub(subs, 'EITM') is None and sub(subs, 'TNAM') is None and not (fl & 0x4): idx('armo', k, b_armo)
    for k in by.get('AMMO', {}): idx('ammo', k, b_ammo)
    for k, (key, fl, subs) in by.get('RACE', {}).items():
        d = sub(subs, 'DATA')
        if d and len(d) >= 36 and u32(d, 32) & 1: idx('race', k, b_race)   # playable races (followers)
    # every playable perk (the follower's tree) plus those NPCs carry (already indexed)
    for k in by.get('CLAS', {}): idx('clas', k, b_clas)
    for k, (key, fl, subs) in by.get('PERK', {}).items(): idx('perk', k, b_perk)
    for r in rows['perk']:
        if r and 'next' in r: r['next'] = tabs['perk'].get(r['next'], -1)
    potions = []
    for k, (key, fl, subs) in by.get('ALCH', {}).items():
        e = effects(key, subs); en = sub(subs, 'ENIT')
        if e and en and len(en) >= 8 and not (u32(en, 4) & 0x2) and not (u32(en, 4) & 0x20000):   # not food, not poison
            potions.append(dict(edid=edid(subs), value=i32(en, 0), effects=[{x: v for x, v in ef.items() if x in ('mag', 'dur', 'av', 'archetype', 'hostile', 'detrimental')} for ef in e]))

    selfcheck = []

    def ck(what, got, want):
        ok = got == want if not isinstance(want, float) else got is not None and abs(got - want) < 1e-3
        selfcheck.append(dict(what=what, got=got, want=want, ok=bool(ok)))
    sk = lambda sig, loc: by.get(sig, {}).get(('skyrim.esm', loc))
    # vanilla anchors; a list that overrides one shows as a miss ("ok": false), which is information, not an error
    w = rows['weap'][tabs['weap'][('skyrim.esm', 0x12EB7)]] if ('skyrim.esm', 0x12EB7) in tabs['weap'] else None   # IronSword
    if w: ck('WEAP IronSword anim=1 (1H sword)', w['anim'], 1); ck('WEAP IronSword skill=6 (OneHanded)', w['skill'], 6); ck('WEAP IronSword vanilla dmg 7', w['dmg'], 7); ck('WEAP IronSword vanilla speed 1.0', w['speed'], 1.0); ck('WEAP IronSword vanilla stagger 0.75', w['stagger'], 0.75)
    w = rows['weap'][tabs['weap'][('skyrim.esm', 0x1359D)]] if ('skyrim.esm', 0x1359D) in tabs['weap'] else None   # IronGreatsword
    if w: ck('WEAP IronGreatsword anim=5', w['anim'], 5); ck('WEAP IronGreatsword skill=7 (TwoHanded)', w['skill'], 7); ck('WEAP IronGreatsword vanilla speed 0.75', round(w['speed'], 3), 0.75)
    w = rows['weap'][tabs['weap'][('skyrim.esm', 0x13985)]] if ('skyrim.esm', 0x13985) in tabs['weap'] else None   # LongBow
    if w: ck('WEAP HuntingBow anim=7', w['anim'], 7); ck('WEAP HuntingBow skill=8 (Marksman)', w['skill'], 8)
    ar = rows['armo'][tabs['armo'][('skyrim.esm', 0x12E49)]] if ('skyrim.esm', 0x12E49) in tabs['armo'] else None   # ArmorIronCuirass
    if ar: ck('ARMO IronCuirass type=1 heavy', ar['type'], 1); ck('ARMO IronCuirass vanilla AR 25', ar['ar'], 25.0); ck('ARMO IronCuirass body slot 0x4', ar['slots'] & 0x4, 4)
    rc = rows['race'][tabs['race'][('skyrim.esm', 0x13746)]] if ('skyrim.esm', 0x13746) in tabs['race'] else None   # NordRace
    if rc: ck('RACE Nord playable', rc['playable'], True); ck('RACE Nord vanilla hp 50', rc['hp'], 50.0); ck('RACE Nord vanilla unarmed 4', rc['unarmedDmg'], 4.0); ck('RACE Nord run speed (MOVT NPC_1HM_MT fwd run) vanilla 370', rc['run'], 370.0)
    ck('GMST fArmorScalingFactor vanilla 0.12', gmst.get('fArmorScalingFactor'), 0.12); ck('GMST fMaxArmorRating vanilla 80', gmst.get('fMaxArmorRating'), 80.0)
    ck('FACT PlayerFaction has enemies', len(player_enemies) > 0, True)
    bz = ecz.get(('skyrim.esm', 0x1BDF3))
    cs = sk('CSTY', 0x3BE1B)   # csHumanMeleeLvl1? (only checks the size contract)
    if cs: ck('CSTY CSGD is 10 floats', len(sub(cs[2], 'CSGD') or b'') >= 32, True)

    out = dict(list=name, profile=a.profile, plugins=len(order), pluginsMissing=missing, gmst=dict(sorted(gmst.items())),
               races=rows['race'], classes=rows['clas'], cstyles=rows['csty'], weapons=rows['weap'], armors=rows['armo'], ammo=rows['ammo'],
               lvli=rows['lvli'], lvln=rows['lvln'], npcs=rows['npc'], spells=rows['spel'], perks=rows['perk'], potions=potions,
               encounters=encounters, selfcheck=selfcheck)
    path = a.out or os.path.expanduser('~/.cache/mfo-list-balance/%s.world.json' % name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f: json.dump(out, f, separators=(',', ':'))
    bad = [c for c in selfcheck if not c['ok']]
    print(path, {k: len(v) for k, v in out.items() if isinstance(v, list)}, 'gmst', len(gmst), 'in %.0fs' % (time.time() - t0))
    for c in selfcheck: print('  selfcheck %-5s %s (got %r)' % ('ok' if c['ok'] else 'MISS', c['what'], c['got']))
    if a.selfcheck and bad: sys.exit(1)


if __name__ == '__main__':
    main()
