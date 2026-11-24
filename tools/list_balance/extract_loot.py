#!/usr/bin/env python3
"""list_balance/extract_loot.py -- the LOOT half of the list balance (IAUS sim v4 batch L, "full looting").

Reads an installed MO2 list's WINNING item, container, leveled-list, flora and NPC inventory records into one JSON:
  ~/.cache/mfo-list-balance/<name>.loot.json       (never committed; TEST INPUT ONLY for tools/iaus_sim --loot-list)
READ-ONLY on the list (every file is opened 'rb'). Nothing in native/ ever reads this data, and nothing in tools/iaus_sim
keys on a list's name or on a record's name: the sim joins by (plugin file, local id), the key tools/envextract writes.

usage: extract_loot.py --list /mnt/gaming/modlists/Tuxborn --profile "Tuxborn - Deck" --data "Game Root/Data"
       extract_loot.py --list /mnt/gaming/modlists/LoreRim --profile Ultra --data "Stock Game/Data" [--vanilla]
       extract_loot.py ... --selfcheck      # print the vanilla layout rows, exit 1 on a miss

Output (keys are "<plugin file, lowercase>:<local id, hex>"):
  items   key -> [kind, value, weight, power, extra]
          kind: gold lockpick misc gem ingot ore hide ingredient soulgem key book scroll ammo bolt weapon1h weapon2h bow crossbow staff
                armorbody armorhead armorhands armorfeet shield jewelry clothing potion food poison
          power: weapon damage / armor rating / ammo damage / 0. extra: for potion, food, poison the effect rows
          [archetype, actor value, magnitude, duration, flags] (flags: 1 hostile, 2 detrimental, 4 recover); for armor [type] (0 light 1 heavy 2 clothing);
          for a weapon [enchanted 0|1]
  cont    key -> [[item or leveled-list key, count], ...]                        CONT CNTO rows
  lvli    key -> {f: LVLF flags (1 all levels, 2 each, 4 use all), none: LVLD chance none, e: [[level, key, count], ...]}
  flora   key -> produce key (FLOR / TREE PFIG)
  bodies  [[[key, count], ...], ...]   one row per NPC_ record that owns anything: its CNTO rows plus its death item (INAM, count 1).
          MODEL in the sim: a corpse's contents are drawn from one of these rows (the fight's foes carry no record id).
  selfcheck [...]
Record layouts read here (UESP "Skyrim Mod:Mod File Format", each checked against a vanilla value by --selfcheck):
  MISC / KEYM / SLGM / SCRL DATA: i32 value, f32 weight.  INGR DATA: i32 value, f32 weight.  ALCH DATA: f32 weight; ENIT: i32 value, u32 flags
  (0x2 food, 0x20000 poison).  BOOK DATA: u8 flags, u8 type, u16, u32 teaches, i32 value, f32 weight.  AMMO DATA: u32 projectile, u32 flags
  (0x4 = NOT a bolt), f32 damage, u32 value[, f32 weight].  WEAP DATA: i32 value, f32 weight, u16 damage; DNAM[0] animation type.
  ARMO DATA: i32 value, f32 weight; DNAM i32 rating x 100; BOD2 u32 slots, u32 type.  CONT / NPC_ CNTO: u32 item, i32 count.  NPC_ INAM: u32 death item.
  LVLI LVLD u8, LVLF u8, LVLO: u16 level, u16, u32 ref, u16 count, u16.  FLOR / TREE PFIG: u32 produce.  MGEF DATA: as extract_world.py.
"""
import argparse, json, os, struct, sys, time

_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _here); sys.path.insert(0, os.path.join(_here, '..'))
import esp_inspect as ei
from extract import find_plugins, Blob, f32, u32, i32

WANT = {b'KYWD', b'MGEF', b'MISC', b'KEYM', b'SLGM', b'SCRL', b'INGR', b'ALCH', b'BOOK', b'AMMO', b'WEAP', b'ARMO', b'CONT', b'LVLI',
        b'NPC_', b'FLOR', b'TREE'}
u16 = lambda b, o: struct.unpack_from('<H', b, o)[0]


def scan(path):
    """-> masters, light, flat [(sig, fid, flags, subs)]: the top groups of WANT only (cells are never walked)."""
    with open(path, 'rb') as f: data = f.read()
    blob = Blob(data); n = len(data); flat = []; masters = []
    light = path.lower().endswith('.esl'); off = 0
    while off + 24 <= n:
        sig = data[off:off + 4]; size = u32(data, off + 4)
        if sig == b'GRUP':
            if size < 24: break
            if data[off + 8:off + 12] in WANT:
                end = min(off + size, n); o = off + 24
                while o + 24 <= end:
                    s2 = data[o:o + 4]; sz = u32(data, o + 4)
                    if s2 == b'GRUP':
                        if sz < 24: break
                        o += sz; continue
                    fl, fid = struct.unpack_from('<II', data, o + 8)
                    if not fl & 0x20:
                        try: flat.append((s2.decode(), fid, fl, ei.Record(s2.decode(), fid, fl, o, sz, blob, '').subs()))
                        except Exception: pass
                    o += 24 + sz
            off += size; continue
        if sig == b'TES4':
            if u32(data, off + 8) & 0x200: light = True
            for t, d in ei.Record('TES4', 0, 0, off, size, blob, '').subs():
                if t == 'MAST': masters.append(ei._zstr(d))
        off += 24 + size
    return masters, light, flat


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--list', required=True); ap.add_argument('--profile', required=True)
    ap.add_argument('--data', action='append', default=[]); ap.add_argument('--out'); ap.add_argument('--name')
    ap.add_argument('--vanilla', action='store_true', help='only the five base ESMs of this install (name Vanilla)')
    ap.add_argument('--selfcheck', action='store_true')
    a = ap.parse_args()
    name = a.name or os.path.basename(a.list.rstrip('/'))
    order, where = find_plugins(a.list, a.profile, a.data)
    if a.vanilla: order = order[:5]; name = a.name or 'Vanilla'
    win = {}; missing = 0; t0 = time.time()
    for pl in order:
        p = where.get(pl.lower())
        if not p: missing += 1; continue
        try: masters, light, flat = scan(p)
        except Exception as e:
            print('skip', pl, e, file=sys.stderr); continue
        names = [m.lower() for m in masters] + [pl.lower()]

        def key(fid, names=names, light=light, pl=pl):
            mi = min(fid >> 24, len(names) - 1); owner = names[mi]
            return owner, fid & (0xFFF if (light and owner == pl.lower()) else 0xFFFFFF)
        for sig, fid, fl, subs in flat: win[(sig,) + key(fid)] = (key, fl, subs)
    print('scanned %d plugins in %.0fs: %d records' % (len(order), time.time() - t0, len(win)), file=sys.stderr)

    sub = lambda subs, t: next((d for k, d in subs if k == t), None)
    by = {}
    for (sig, owner, loc), v in win.items(): by.setdefault(sig, {})[(owner, loc)] = v
    K = lambda k: '%s:%x' % (k[0], k[1])
    kw = {k: ei._zstr(sub(v[2], 'EDID') or b'') for k, v in by.get('KYWD', {}).items()}

    def kws(key, subs):
        d = sub(subs, 'KWDA')
        return {kw.get(key(u32(d, i)), '') for i in range(0, len(d) - 3, 4)} if d else set()

    mgef = {}
    for k, (key, fl, subs) in by.get('MGEF', {}).items():
        d = sub(subs, 'DATA')
        if d is not None and len(d) >= 88: mgef[k] = (u32(d, 0), u32(d, 64), i32(d, 68))   # flags, archetype, actor value

    def effects(key, subs):
        out = []; cur = None
        for t, d in subs:
            if t == 'EFID':
                m = mgef.get(key(u32(d, 0))); cur = None
                if m: cur = [m[1], m[2], 0.0, 0, (1 if m[0] & 1 else 0) | (2 if m[0] & 4 else 0) | (4 if m[0] & 2 else 0)]; out.append(cur)
            elif t == 'EFIT' and cur is not None and len(d) >= 12: cur[2] = round(f32(d, 0), 3); cur[3] = u32(d, 8)
        return out

    items = {}
    vw = lambda d: (i32(d, 0), round(f32(d, 4), 3)) if d is not None and len(d) >= 8 else None
    GOLD, PICK = ('skyrim.esm', 0xF), ('skyrim.esm', 0xA)
    for k, (key, fl, subs) in by.get('MISC', {}).items():
        x = vw(sub(subs, 'DATA'))
        if not x: continue
        ks = kws(key, subs)
        kind = ('gold' if k == GOLD else 'lockpick' if k == PICK else 'gem' if 'VendorItemGem' in ks else 'ingot' if 'VendorItemOreIngot' in ks
                else 'hide' if 'VendorItemAnimalHide' in ks else 'misc')
        items[K(k)] = [kind, x[0], x[1], 0, []]
    for sig, kind in (('KEYM', 'key'), ('SLGM', 'soulgem'), ('SCRL', 'scroll'), ('INGR', 'ingredient')):
        for k, (key, fl, subs) in by.get(sig, {}).items():
            x = vw(sub(subs, 'DATA'))
            if x: items[K(k)] = [kind, x[0], x[1], 0, []]
    for k, (key, fl, subs) in by.get('BOOK', {}).items():
        d = sub(subs, 'DATA')
        if d is not None and len(d) >= 16: items[K(k)] = ['book', i32(d, 8), round(f32(d, 12), 3), 0, []]
    for k, (key, fl, subs) in by.get('ALCH', {}).items():
        d = sub(subs, 'DATA'); en = sub(subs, 'ENIT')
        if d is None or en is None or len(d) < 4 or len(en) < 8: continue
        f = u32(en, 4); kind = 'poison' if f & 0x20000 else 'food' if f & 0x2 else 'potion'
        items[K(k)] = [kind, i32(en, 0), round(f32(d, 0), 3), 0, effects(key, subs)]
    for k, (key, fl, subs) in by.get('AMMO', {}).items():
        d = sub(subs, 'DATA')
        if d is None or len(d) < 16: continue
        items[K(k)] = ['ammo' if u32(d, 4) & 0x4 else 'bolt', u32(d, 12), round(f32(d, 16), 3) if len(d) >= 20 else 0.0, round(f32(d, 8), 2), []]
    for k, (key, fl, subs) in by.get('WEAP', {}).items():
        d = sub(subs, 'DATA'); n = sub(subs, 'DNAM')
        if d is None or n is None or len(d) < 10 or len(n) < 1: continue
        an = n[0]   # 1 sword 2 dagger 3 axe 4 mace 5 greatsword 6 battleaxe / warhammer 7 bow 8 staff 9 crossbow (extract_world.py selfcheck: IronSword 1, IronGreatsword 5, bow 7)
        kind = 'bow' if an == 7 else 'crossbow' if an == 9 else 'staff' if an == 8 else 'weapon2h' if an in (5, 6) else 'weapon1h'
        items[K(k)] = [kind, i32(d, 0), round(f32(d, 4), 3), u16(d, 8), [1 if sub(subs, 'EITM') is not None else 0]]
    for k, (key, fl, subs) in by.get('ARMO', {}).items():
        d = sub(subs, 'DATA'); n = sub(subs, 'DNAM'); b2 = sub(subs, 'BOD2'); bt = sub(subs, 'BODT')
        if d is None or len(d) < 8: continue
        slots = u32(b2, 0) if b2 and len(b2) >= 8 else u32(bt, 0) if bt and len(bt) >= 4 else 0
        typ = u32(b2, 4) if b2 and len(b2) >= 8 else u32(bt, 8) if bt and len(bt) >= 12 else 2
        ks = kws(key, subs)
        kind = ('jewelry' if ks & {'ArmorJewelry', 'ClothingRing', 'ClothingNecklace', 'ClothingCirclet'} else 'shield' if 'ArmorShield' in ks
                else 'clothing' if typ == 2 else 'armorbody' if slots & 0x4 else 'armorhead' if slots & 0x1003 else 'armorhands' if slots & 0x8
                else 'armorfeet' if slots & 0x80 else 'clothing')
        items[K(k)] = [kind, i32(d, 0), round(f32(d, 4), 3), (i32(n, 0) / 100.0) if n and len(n) >= 4 else 0.0, [typ]]

    lvli = {}
    for k, (key, fl, subs) in by.get('LVLI', {}).items():
        f = sub(subs, 'LVLF'); c = sub(subs, 'LVLD'); e = []
        for t, d in subs:
            if t == 'LVLO' and len(d) >= 12: e.append([u16(d, 0), K(key(u32(d, 4))), u16(d, 8)])
        lvli[K(k)] = dict(f=f[0] if f else 0, none=c[0] if c else 0, e=e)

    def cnto(key, subs):
        return [[K(key(u32(d, 0))), i32(d, 4)] for t, d in subs if t == 'CNTO' and len(d) >= 8]
    known = lambda r: r[0] in items or r[0] in lvli
    cont = {}
    for k, (key, fl, subs) in by.get('CONT', {}).items(): cont[K(k)] = [r for r in cnto(key, subs) if known(r) and r[1] > 0]
    flora = {}
    for sig in ('FLOR', 'TREE'):
        for k, (key, fl, subs) in by.get(sig, {}).items():
            d = sub(subs, 'PFIG')
            if d is not None and len(d) >= 4 and u32(d, 0):
                p = K(key(u32(d, 0)))
                if p in items or p in lvli: flora[K(k)] = p
    bodies = []
    for k, (key, fl, subs) in by.get('NPC_', {}).items():
        rows = [r for r in cnto(key, subs) if known(r) and r[1] > 0]; d = sub(subs, 'INAM')
        if d is not None and len(d) >= 4 and u32(d, 0):
            di = K(key(u32(d, 0)))
            if di in lvli or di in items: rows.append([di, 1])
        if rows: bodies.append(rows)

    selfcheck = []

    def ck(what, got, want):
        selfcheck.append(dict(what=what, got=got, want=want, ok=got == want))
    g = items.get('skyrim.esm:f'); ck('MISC Gold001 value 1 weight 0', g and g[:3], ['gold', 1, 0.0])
    g = items.get('skyrim.esm:a'); ck('MISC Lockpick kind', g and g[0], 'lockpick')
    g = items.get('skyrim.esm:3eadd'); ck('ALCH RestoreHealth01 is a potion of weight 0.5', g and [g[0], g[2]], ['potion', 0.5])
    ck('ALCH RestoreHealth01 restores health (archetype 0 value modifier, av 24)', g and g[4] and g[4][0][:2], [0, 24])
    g = items.get('skyrim.esm:12eb7'); ck('WEAP IronSword 1h damage 7 weight 9', g and [g[0], g[2], g[3]], ['weapon1h', 9.0, 7])
    g = items.get('skyrim.esm:12e49'); ck('ARMO IronCuirass body rating 25 weight 30', g and [g[0], g[2], g[3]], ['armorbody', 30.0, 25.0])
    g = items.get('skyrim.esm:1397d'); ck('AMMO IronArrow is an arrow of damage 8', g and [g[0], g[3]], ['ammo', 8.0])
    g = items.get('skyrim.esm:63b47'); ck('MISC Garnet is a gem', g and g[0], 'gem')
    g = items.get('skyrim.esm:2e4e2'); ck('SLGM petty soul gem', g and g[0], 'soulgem')
    ck('LVLI and CONT tables are not empty', bool(lvli) and bool(cont), True)

    out = dict(list=name, profile=a.profile, plugins=len(order), pluginsMissing=missing, items=items, cont=cont, lvli=lvli, flora=flora,
               bodies=bodies, selfcheck=selfcheck)
    bad = [c for c in selfcheck if not c['ok']]
    for c in selfcheck: print('%s  %s (got %r)' % ('ok  ' if c['ok'] else 'MISS', c['what'], c['got']), file=sys.stderr)
    if a.selfcheck: sys.exit(1 if bad else 0)
    path = a.out or os.path.expanduser('~/.cache/mfo-list-balance/%s.loot.json' % name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f: json.dump(out, f, separators=(',', ':'))
    print('%s: %d items, %d containers, %d leveled lists, %d flora, %d bodies' % (path, len(items), len(cont), len(lvli), len(flora), len(bodies)), file=sys.stderr)


if __name__ == '__main__':
    main()
