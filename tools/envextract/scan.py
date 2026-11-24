"""Scan a plugin set into one in-memory / pickled structure: winning cells, navmeshes, base objects, placed refs.

Keys are ints: (global plugin index << 24) | local id, where the plugin index is the position of the OWNING plugin
(the one whose master list / own name the raw FormID resolved to) in the set's load order.
"""
import os, struct, time, pickle, math, zlib
from multiprocessing import Pool
import plugin as P

BASE_SIGS = [b'DOOR', b'CONT', b'ACTI', b'FURN', b'MISC', b'WEAP', b'ARMO', b'AMMO', b'BOOK', b'ALCH', b'INGR', b'KEYM',
             b'SCRL', b'SLGM', b'NPC_', b'LVLN', b'STAT', b'MSTT', b'SCOL', b'FLOR', b'TREE', b'APPA']
BASE_SET = set(BASE_SIGS)
_G = {}   # worker globals (fork-shared)


def _ctx(order):
    names = [o.lower() for o in order]
    _G['order'] = order; _G['names'] = names; _G['idx'] = {n: i for i, n in enumerate(names)}


def _plug(i):
    pl = _G.get('plug', {})
    if i not in pl:
        pl[i] = P.Plugin(_G['where'][_G['names'][i]]); _G['plug'] = pl
    return pl[i]


def key_of(pg, i, raw):
    owner, local = pg.resolve(raw, _G['order'][i])
    return (_G['idx'].get(owner, 4095) << 24) | local


def _zrange(nv):
    """(min z, max z) of the vertices of an NVNM blob (16 byte header, then u32 vertex count: same offsets interior and exterior)"""
    n = struct.unpack_from('<I', nv, 16)[0]
    if n == 0: return (0.0, 0.0)
    zs = struct.unpack_from('<%df' % (3 * n), nv, 20)[2::3]
    return (min(zs), max(zs))


def pass1(i):
    pg = P.Plugin(_G['where'][_G['names'][i]]); out = dict(cells=[], navm=[], bases={}, worlds=[])
    cache = {}
    def K(raw):
        if raw == 0: return 0
        k = cache.get(raw)
        if k is None: k = cache[raw] = key_of(pg, i, raw)
        return k
    for lab, s, e in pg.top_groups():
        if lab in (b'CELL', b'WRLD'):
            for sig, fl, fid, o, size, c in pg.walk(s, e):
                if sig == b'CELL':
                    if fl & P.F_DELETED: out['cells'].append((K(fid), None)); continue
                    d = dict(P.subrecords(pg.data, o, size, fl))
                    dat = d.get(b'DATA', b'\0'); cl = d.get(b'XCLC')
                    out['cells'].append((K(fid), dict(
                        world=K(c['world']), flags=dat[0] | (dat[1] << 8 if len(dat) > 1 else 0),
                        grid=struct.unpack_from('<ii', cl, 0) if cl and len(cl) >= 8 and not fl & P.F_PERSISTENT else None, persist=bool(fl & P.F_PERSISTENT),
                        water=struct.unpack('<f', d[b'XCLW'])[0] if b'XCLW' in d and len(d[b'XCLW']) >= 4 else None,
                        edid=d.get(b'EDID', b'').split(b'\0')[0].decode('latin-1'), plugin=i)))
                elif sig == b'WRLD':
                    if fl & P.F_DELETED: continue
                    d = dict(P.subrecords(pg.data, o, size, fl)); dn = d.get(b'DNAM')
                    out['worlds'].append((K(fid), d.get(b'EDID', b'').split(b'\0')[0].decode('latin-1'),
                                          struct.unpack_from('<ff', dn, 0) if dn and len(dn) >= 8 else None))
                elif sig == b'NAVM':
                    if fl & P.F_DELETED: out['navm'].append((K(fid), None)); continue
                    nv = dict(P.subrecords(pg.data, o, size, fl)).get(b'NVNM')
                    if nv is None: continue
                    out['navm'].append((K(fid), (i, bytes(nv))))
        elif lab in BASE_SET:
            for sig, fl, fid, o, size, c in pg.walk(s, e):
                if sig not in BASE_SET: continue
                if fl & P.F_DELETED: out['bases'][K(fid)] = None; continue
                d = dict(P.subrecords(pg.data, o, size, fl))
                ob = d.get(b'OBND')
                out['bases'][K(fid)] = (sig.decode(), d.get(b'EDID', b'').split(b'\0')[0].decode('latin-1'),
                                        struct.unpack_from('<6h', ob, 0) if ob and len(ob) >= 12 else None,
                                        d.get(b'MODL', b'').split(b'\0')[0].decode('latin-1') or None)
    return i, out


def _has_xesp(pg, o, size, fl):
    body = pg.data[o:o + size]
    if fl & P.F_COMPRESSED: body = zlib.decompress(body[4:])
    return b'XESP' in body


def pass2(i):
    want = _G['want']; pg = P.Plugin(_G['where'][_G['names'][i]])
    refs = []; moved = []; cache = {}; wc = {}
    def K(raw):
        if raw == 0: return 0
        k = cache.get(raw)
        if k is None: k = cache[raw] = key_of(pg, i, raw)
        return k
    for lab, s, e in pg.top_groups():
        if lab != b'CELL' and lab != b'WRLD': continue
        for sig, fl, fid, o, size, c in pg.walk(s, e):
            if sig != b'REFR' and sig != b'ACHR': continue
            craw = c['cell']
            ck = wc.get(craw)
            if ck is None: ck = wc[craw] = K(craw)
            rk = K(fid)
            if ck not in want:
                # state evidence (README, enable parents): an override, an initially disabled ref or a ref with an enable parent
                # can decide whether something is on; plain new refs are enabled with no parent and need no row
                if fl & P.F_DELETED: moved.append((rk, ck, True, False, None)); continue
                ov = (rk >> 24) != i
                if ov or fl & P.F_DISABLED or _has_xesp(pg, o, size, fl):
                    ep = None
                    for t, d in P.subrecords(pg.data, o, size, fl):
                        if t == b'XESP' and len(d) >= 5: ep = (K(struct.unpack_from('<I', d, 0)[0]), d[4])
                    moved.append((rk, ck, False, bool(fl & P.F_DISABLED), ep))
                continue
            if fl & P.F_DELETED:
                refs.append((rk, ck, fl, None)); continue
            nm = None; pos = rot = None; scale = 1.0; ep = None; tel = None; lock = None; cnt = 1
            for t, d in P.subrecords(pg.data, o, size, fl):
                if t == b'NAME': nm = K(struct.unpack_from('<I', d, 0)[0])
                elif t == b'DATA' and len(d) >= 24: v = struct.unpack_from('<6f', d, 0); pos, rot = v[:3], v[3:]
                elif t == b'XSCL': scale = struct.unpack_from('<f', d, 0)[0]
                elif t == b'XESP' and len(d) >= 5: ep = (K(struct.unpack_from('<I', d, 0)[0]), d[4])
                elif t == b'XTEL' and len(d) >= 32:
                    v = struct.unpack_from('<I6fI', d, 0); tel = (K(v[0]),) + tuple(v[1:7]) + (v[7],)
                elif t == b'XLOC' and len(d) >= 8: lock = (d[0], K(struct.unpack_from('<I', d, 4)[0]), d[8] if len(d) > 8 else 0)
                elif t == b'XCNT' and len(d) >= 4: cnt = struct.unpack_from('<i', d, 0)[0]
            if nm is None or pos is None: continue
            refs.append((rk, ck, fl, (sig == b'ACHR', nm, pos, rot, scale, ep, tel, lock, cnt)))
    return i, refs, moved


def _pool_run(fn, n, procs):
    with Pool(procs) as p:
        for r in p.imap(fn, range(n), chunksize=1): yield r


def scan_set(order, where, procs=6, log=print):
    """-> dict with the merged winners.  Slow part: two passes over every plugin."""
    _ctx(order); _G['where'] = where; n = len(order); t0 = time.time()
    cells = {}; navm = {}; bases = {}; worlds = {}; plugins = []; navorig = {}
    p1 = [None] * n
    for i, out in _pool_run(pass1, n, procs): p1[i] = out
    log('pass 1: %d plugins in %.0fs' % (n, time.time() - t0))
    for i in range(n):
        pg = P.Plugin(where[order[i].lower()])
        plugins.append(dict(name=order[i], light=pg.light, masters=pg.masters))
        o = p1[i]
        for k, v in o['cells']:
            if v is None: cells.pop(k, None)
            else: cells[k] = v
        for k, v in o['navm']:
            if v is None: navm.pop(k, None)
            else:
                if k not in navm and k not in navorig: navorig[k] = _zrange(v[1])     # the first record of the mesh: its own height
                navm[k] = v
        for k, v in o['bases'].items():
            if v is None: bases.pop(k, None)
            else: bases[k] = v
        for k, e, w in o['worlds']: worlds[k] = (e, w)
    p1 = None
    # cells that hold a navmesh: interior from the NVNM parent, exterior from world+grid
    navcells = set(); navinfo = {}
    nameidx = {o.lower(): i for i, o in enumerate(order)}
    def rk(pg, raw):
        if raw == 0: return 0
        mi = raw >> 24
        if mi < len(pg['masters']): owner, loc = pg['masters'][mi].lower(), raw & 0xFFFFFF
        else: owner, loc = pg['name'].lower(), raw & (0xFFF if pg['light'] else 0xFFFFFF)
        return (nameidx.get(owner, 4095) << 24) | loc
    ext_cells = {}   # (world, gx, gy) -> cell key
    for ck, c in cells.items():
        if c['grid'] is not None and not c['flags'] & 1: ext_cells[(c['world'], c['grid'][0], c['grid'][1])] = ck
    want = set()
    for k, (pi, nv) in navm.items():
        ver, magic, world = struct.unpack_from('<III', nv, 0)
        pg = plugins[pi]
        if world:
            gy, gx = struct.unpack_from('<hh', nv, 12); wk = rk(pg, world)
            ck = ext_cells.get((wk, gx, gy)); navinfo[k] = (wk, gx, gy, ck)
        else:
            ck = rk(pg, struct.unpack_from('<I', nv, 12)[0]); navinfo[k] = (0, None, None, ck)
        if ck: navcells.add(ck)
    want = set(navcells)
    for (wk2, gx2, gy2) in {(v[0], v[1], v[2]) for v in navinfo.values() if v[0]}:   # window cells without a navmesh still get their statics
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                c2 = ext_cells.get((wk2, gx2 + dx, gy2 + dy))
                if c2: want.add(c2)
    navworlds = {v[0] for v in navinfo.values() if v[0]}
    for ck, c in cells.items():   # exterior persistent cell of a world that has navmeshes
        if c['persist'] and not c['flags'] & 1 and c['world'] in navworlds: want.add(ck)
    log('%d navmeshes, %d cells, %d with navmesh (+persistent), %d bases' % (len(navm), len(cells), len(navcells), len(bases)))
    # pass 2
    _G['want'] = want; t1 = time.time(); refs = {}; states = {}
    for i, rr, moved in _pool_run(pass2, n, procs):
        for rkey, ck, fl, body in rr:
            states.pop(rkey, None)
            if body is None: refs.pop(rkey, None); states[rkey] = None
            else: refs[rkey] = (ck, fl) + body
        for rkey, ck, dele, dis, ep in moved:
            refs.pop(rkey, None)
            if dele: states[rkey] = None          # deleted elsewhere
            else: states[rkey] = (dis, ep)
    log('pass 2: %d refs in %.0fs' % (len(refs), time.time() - t1))
    return dict(order=order, plugins=plugins, cells=cells, navm=navm, navinfo=navinfo, bases=bases, worlds=worlds, refs=refs, states=states, navorig=navorig,
                nameidx=nameidx, want=want)


def save(sc, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f: pickle.dump(sc, f, protocol=4)


def load(path):
    with open(path, 'rb') as f: return pickle.load(f)
