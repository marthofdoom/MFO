#!/usr/bin/env python3
"""list_balance/extract.py -- read an installed MO2 list's WINNING balance records into one JSON (card I2s).

TEST INPUT ONLY for tools/iaus_sim. Nothing in native/ ever reads this data (no-list-specific-data rule).
READ-ONLY on the list. Output default: ~/.cache/mfo-list-balance/<name>.json (never committed).

usage: extract.py --list /mnt/gaming/modlists/LoreRim --profile Ultra --data "Stock Game/Data"
       extract.py --list /mnt/gaming/modlists/Tuxborn --profile "Tuxborn - Deck" --data "Game Root/Data"

Reuses esp_inspect.Record/_zstr. Only the SPEL/SCRL/ALCH/MGEF/GMST/RACE/ECZN top groups are walked, in
plugins.txt load order; the last plugin to define a (type, origin plugin, local id) wins. Plugins are located
through profiles/<P>/modlist.txt (top = highest priority), then overwrite/, then the --data folders.
"""
import argparse, json, os, re, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import esp_inspect as ei

WANT = {b'SPEL', b'SCRL', b'ALCH', b'MGEF', b'GMST', b'RACE', b'ECZN', b'BOOK'}
# GMSTs kept (EDID regex). The exact names found are written to the JSON ("gmst").
GMST_RE = re.compile(r'Regen|DiffMult|Difficulty|PowerAttack|MagicCost|MagicSkill|MagicDiff|MagicCasting|'
                     r'StaminaAttack|MeleeWeaponStamina|BowDrawStamina|BlockStamina|AVDamage|AVDHealth|'
                     r'fMagickaReturn|fMeleeReach|fWeaponDamage|fDamage|fSneakAttack|fCombatDistance', re.I)
AV_HEALTH, AV_MAGICKA, AV_STAMINA = 24, 25, 26
SCHOOLS = {18: 'Alteration', 19: 'Conjuration', 20: 'Destruction', 21: 'Illusion', 22: 'Restoration'}
DELIVERY = ['Self', 'Touch', 'Aimed', 'TargetActor', 'TargetLocation']
CASTTYPE = ['ConstantEffect', 'FireAndForget', 'Concentration', 'Scroll']


class Blob:   # stand-in for esp_inspect.Plugin: Record only reads .data
    def __init__(self, d): self.data = d


def scan(path):
    with open(path, 'rb') as f: data = f.read()
    blob = Blob(data); n = len(data); recs = []; masters = []; light = path.lower().endswith('.esl')
    off = 0
    while off + 24 <= n:
        sig = data[off:off + 4]; size = struct.unpack_from('<I', data, off + 4)[0]
        if sig == b'GRUP':
            if data[off + 8:off + 12] in WANT:
                end = off + size; o = off + 24
                while o + 24 <= end:
                    s2 = data[o:o + 4]; sz = struct.unpack_from('<I', data, o + 4)[0]
                    if s2 == b'GRUP': o += sz; continue
                    fl, fid = struct.unpack_from('<II', data, o + 8)
                    if not fl & 0x20:
                        recs.append((s2.decode(), fid, ei.Record(s2.decode(), fid, fl, o, sz, blob, '').subs()))
                    o += 24 + sz
            off += size; continue
        if sig == b'TES4':
            if struct.unpack_from('<I', data, off + 8)[0] & 0x200: light = True
            for t, d in ei.Record('TES4', 0, 0, off, size, blob, '').subs():
                if t == 'MAST': masters.append(ei._zstr(d))
        off += 24 + size
    return masters, light, recs


def find_plugins(root, profile, data_dirs):
    prof = os.path.join(root, 'profiles', profile)
    mods = [l[1:] for l in open(os.path.join(prof, 'modlist.txt'), encoding='utf-8', errors='replace').read().splitlines()
            if l.startswith('+')]
    dirs = [os.path.join(root, 'mods', m) for m in mods] + [os.path.join(root, 'overwrite')] + \
           [os.path.join(root, d) for d in data_dirs]
    where = {}
    for d in dirs:
        try: names = os.listdir(d)
        except OSError: continue
        for nm in names:
            if nm.lower().endswith(('.esp', '.esm', '.esl')): where.setdefault(nm.lower(), os.path.join(d, nm))
    order = [l[1:].strip() for l in open(os.path.join(prof, 'plugins.txt'), encoding='utf-8', errors='replace').read().splitlines()
             if l.startswith('*')]
    return order, where


def f32(b, o): return struct.unpack_from('<f', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def i32(b, o): return struct.unpack_from('<i', b, o)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--list', required=True); ap.add_argument('--profile', required=True)
    ap.add_argument('--data', action='append', default=[]); ap.add_argument('--out'); ap.add_argument('--name')
    a = ap.parse_args()
    name = a.name or os.path.basename(a.list.rstrip('/'))
    order, where = find_plugins(a.list, a.profile, a.data)
    win = {}; missing = []
    for pl in order:
        p = where.get(pl.lower())
        if not p: missing.append(pl); continue
        try: masters, light, recs = scan(p)
        except Exception as e:
            print('skip', pl, e, file=sys.stderr); continue
        names = [m.lower() for m in masters] + [pl.lower()]
        for sig, fid, subs in recs:
            mi = min(fid >> 24, len(names) - 1)
            owner = names[mi]
            loc = fid & (0xFFF if (light and owner == pl.lower()) else 0xFFFFFF)
            win[(sig, owner, loc)] = (names, light, pl, subs)

    def ref(names, light, pl, fid):   # a FormID read inside plugin pl -> winner-table key part
        mi = min(fid >> 24, len(names) - 1); owner = names[mi]
        return owner, fid & (0xFFF if (light and owner == pl.lower()) else 0xFFFFFF)

    sub = lambda subs, t: next((d for k, d in subs if k == t), None)
    # MGEF
    mgef = {}
    for (sig, owner, loc), (names, light, pl, subs) in win.items():
        if sig != 'MGEF': continue
        d = sub(subs, 'DATA')
        try:
            if d is None or len(d) < 88: continue
            fl = u32(d, 0)
            mgef[(owner, loc)] = dict(hostile=bool(fl & 1), detrimental=bool(fl & 4), baseCost=f32(d, 4),
                                      skill=SCHOOLS.get(i32(d, 12), 'None'), resist=i32(d, 16), minSkill=u32(d, 40),
                                      archetype=u32(d, 64), av=i32(d, 68),
                                      castType=u32(d, 80), delivery=u32(d, 84))
        except Exception as ex:
            print('bad MGEF', owner, loc, ex, file=sys.stderr)

    def fx(names, light, pl, subs):
        out = []
        for e in ei_effects(subs):
            m = mgef.get(ref(names, light, pl, e['mgef']))
            if not m: continue
            out.append(dict(e, **{k: m[k] for k in ('hostile', 'detrimental', 'skill', 'resist', 'minSkill',
                                                      'archetype', 'av', 'delivery', 'baseCost')}))
            del out[-1]['mgef']
        return out

    def ei_effects(subs):
        o = []; cur = None
        for t, d in subs:
            if t == 'EFID': cur = {'mgef': u32(d, 0), 'mag': 0.0, 'area': 0, 'dur': 0}; o.append(cur)
            elif t == 'EFIT' and cur is not None and len(d) >= 12:
                cur['mag'], cur['area'], cur['dur'] = f32(d, 0), u32(d, 4), u32(d, 8)
        return o

    taught = set()   # (owner, local) of every spell some BOOK teaches (DATA flag 0x04 = teaches spell; teaches FormID at +4)
    for (sig, owner, loc), (names, light, pl, subs) in win.items():
        if sig != 'BOOK': continue
        d = sub(subs, 'DATA')
        if d and len(d) >= 8 and d[0] & 0x04: taught.add(ref(names, light, pl, u32(d, 4)))
    spells, scrolls, potions, gmst, races, ecz = [], [], [], {}, [], []
    for (sig, owner, loc), (names, light, pl, subs) in win.items():
        edid = ei._zstr(sub(subs, 'EDID') or b'')
        if sig == 'GMST':
            d = sub(subs, 'DATA')
            if d and GMST_RE.search(edid) and len(d) >= 4:
                if edid[0] == 'f': gmst[edid] = f32(d, 0)
                elif edid[0] in 'ib': gmst[edid] = i32(d, 0)   # 's' GMSTs are strings: skipped
        elif sig in ('SPEL', 'SCRL'):
            d = sub(subs, 'SPIT')
            if d is None or len(d) < 36 or (sig == 'SPEL' and u32(d, 8) != 0): continue   # spells only (not powers/abilities)
            e = fx(names, light, pl, subs)
            if not e: continue
            row = dict(edid=edid, cost=u32(d, 0), castType=CASTTYPE[min(u32(d, 16), 3)],
                       delivery=DELIVERY[min(u32(d, 20), 4)], chargeTime=f32(d, 12), castDuration=f32(d, 24), range=f32(d, 28),
                       autoCalc=bool(u32(d, 4) & 1), taught=(owner, loc) in taught, effects=e)
            (spells if sig == 'SPEL' else scrolls).append(row)
        elif sig == 'ALCH':
            e = fx(names, light, pl, subs)
            if e: potions.append(dict(edid=edid, effects=e))
        elif sig == 'RACE':
            d = sub(subs, 'DATA')
            if d and len(d) >= 96 and f32(d, 36) > 0:
                races.append(dict(edid=edid, playable=bool(u32(d, 32) & 1), hp=f32(d, 36), mp=f32(d, 40), sp=f32(d, 44),
                                  hpRegen=f32(d, 84), mpRegen=f32(d, 88), spRegen=f32(d, 92)))
        elif sig == 'ECZN':
            d = sub(subs, 'DATA')
            if d and len(d) >= 12: ecz.append((d[9], d[11]))   # minLevel, maxLevel (owner4 loc4 rank1 min1 flags1 max1)
    # SPEL name/edid spells keep only ones with at least one cost-bearing effect
    lv = {}
    for mn, mx in ecz: lv[mx] = lv.get(mx, 0) + 1
    out = dict(list=name, profile=a.profile, plugins=len(order), pluginsMissing=len(missing),
               gmst=dict(sorted(gmst.items())), races=sorted(races, key=lambda r: r['edid']),
               spells=spells, scrolls=scrolls,
               potions=[p for p in potions if any(x['av'] in (AV_HEALTH, AV_MAGICKA, AV_STAMINA) and not x['hostile'] for x in p['effects'])],
               zoneMaxLevelHistogram=dict(sorted(lv.items())), counts=dict(spells=len(spells), scrolls=len(scrolls), mgef=len(mgef)))
    path = a.out or os.path.expanduser('~/.cache/mfo-list-balance/%s.json' % name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    json.dump(out, open(path, 'w'), separators=(',', ':'))
    print(path, out['counts'], 'gmst', len(gmst), 'races', len(races), 'potions', len(out['potions']), 'missing', len(missing))


if __name__ == '__main__':
    main()
