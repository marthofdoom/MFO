#!/usr/bin/env python3
"""list_balance/gmst_defaults.py -- the CODE-DEFAULT value of every float / int game setting, read from the
unpacked executable (IAUS sim v2). A GMST no plugin defines keeps its compiled-in value. The executable holds each
one as a statically initialised Setting object in .data:

    +0x00  vtable pointer
    +0x08  value (float or int32, 4 bytes) + 4 bytes padding
    +0x10  pointer to the name string ("fArmorScalingFactor")

so every 8-byte pointer in .data that targets a setting-shaped name gives the value 8 bytes before it (layout
observed at fArmorScalingFactor on 1.6.1170: vtable, 0x3E19999A = 0.15, name pointer). Note that Skyrim.esm then
overrides many of them (fArmorScalingFactor 0.15 in code, 0.12 in Skyrim.esm): the sim layers code default <
plugins in load order.

READ-ONLY on the executable. Output: ~/.cache/mfo-list-balance/gmst_defaults.json (never committed).
usage: gmst_defaults.py <SkyrimSE.unpacked.exe> [--out path]
"""
import json, os, re, struct, sys


def main():
    if len(sys.argv) < 2: print(__doc__); sys.exit(2)
    path = sys.argv[1]; out = os.path.expanduser('~/.cache/mfo-list-balance/gmst_defaults.json')
    if '--out' in sys.argv: out = sys.argv[sys.argv.index('--out') + 1]
    with open(path, 'rb') as f: data = f.read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]; nsec = struct.unpack_from('<H', data, pe + 6)[0]
    opt = pe + 24; optsz = struct.unpack_from('<H', data, pe + 20)[0]; sec = opt + optsz
    secs = []
    for i in range(nsec):
        o = sec + i * 40; nm = data[o:o + 8].rstrip(b'\0').decode()
        vsz, va, rsz, raw = struct.unpack_from('<IIII', data, o + 8)
        secs.append((nm, va, max(vsz, rsz), raw, rsz))

    def rva2off(rva):
        for nm, va, sz, raw, rsz in secs:
            if va <= rva < va + rsz: return raw + (rva - va)
        return None

    def off2rva(off):
        for nm, va, sz, raw, rsz in secs:
            if raw <= off < raw + rsz: return va + (off - raw)
        return None
    names = {}   # rva of the name string -> name
    for m in re.finditer(rb'(?<=\x00)([fi][A-Z][A-Za-z0-9_ :]{2,70})\x00', data):
        r = off2rva(m.start(1))
        if r is not None: names[r] = m.group(1).decode()
    base = struct.unpack_from('<Q', data, opt + 24)[0]
    res = {}
    for nm, va, sz, raw, rsz in secs:
        if nm != '.data': continue
        for o in range(raw + 16, raw + rsz - 7, 8):
            q = struct.unpack_from('<Q', data, o)[0] - base
            n = names.get(q) if 0 < q < 0x10000000 else None
            if n and n not in res:
                v = struct.unpack_from('<f' if n[0] == 'f' else '<i', data, o - 8)[0]
                if v == v: res[n] = v
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w') as f: json.dump(dict(exe=os.path.basename(os.path.dirname(path)), gmst=dict(sorted(res.items()))), f, separators=(',', ':'))
    ok = True
    for k, want in (('fArmorScalingFactor', 0.15), ('fMaxArmorRating', 95.0), ('fDamagePCSkillMax', 3.0), ('fNPCHealthLevelBonus', 5.0)):
        got = res.get(k); good = got is not None and abs(got - want) < 1e-4; ok &= good
        print('  selfcheck %-4s %s = %r (expected code default %r)' % ('ok' if good else 'MISS', k, got, want))
    print(out, len(res), 'settings')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
