"""Write one .mfoenv for a candidate unit from a scan."""
import math, struct
import fmt, navm as N, refs as R, features as F

TOOL_VERSION = 1


def _ids(sc, env):
    """closure: global key -> env id (plugin table built on demand, max 255 plugins)"""
    plug = env['plug']; order = sc['order']
    def eid(k):
        if not k: return 0
        gi = k >> 24; loc = k & 0xFFFFFF
        name = order[gi].lower() if gi < len(order) else 'unknown'
        if name not in plug:
            if len(plug) > 255: raise SystemExit('more than 255 plugins in one environment')
            plug[name] = len(plug)
        return (plug[name] << 24) | loc
    return eid


def _res(sc, pi):
    pg = sc['plugins'][pi]; nameidx = sc['nameidx']
    def rk(raw):
        if raw == 0: return 0
        mi = raw >> 24
        if mi < len(pg['masters']): owner, loc = pg['masters'][mi].lower(), raw & 0xFFFFFF
        else: owner, loc = pg['name'].lower(), raw & (0xFFF if pg['light'] else 0xFFFFFF)
        return (nameidx.get(owner, 4095) << 24) | loc
    return rk


def unit_cells(unit, win):
    if unit[0] == 'i': return [unit]
    return [('x', unit[1], unit[2] + dx, unit[3] + dy) for dx in range(-win, win + 1) for dy in range(-win, win + 1)]


def export(sc, unit, groups, by, cls, feat, path, setname, win=1, name=None):
    env = dict(plug={}); eid = _ids(sc, env); S = fmt.Strings()
    units = unit_cells(unit, win)
    mkeys = [k for u in units for k in groups.get(u, ())]
    mkeys.sort()
    keyidx = {k: i for i, k in enumerate(mkeys)}
    meshes = []; verts = []; tris = []; links = []; doors = []; cellinfo = {}
    vbase = tbase = lbase = dbase = 0
    cells = sc['cells']
    # exterior grid cell -> cell key
    ext = {}
    for ck, c in cells.items():
        if c['grid'] is not None and not c['flags'] & 1 and not c['persist']: ext[(c['world'], c['grid'][0], c['grid'][1])] = ck
    # refs first (door rows point into them)
    refrows = []; refidx = {}
    for u in units:
        for rk, (kind, sig, bmin, bmax) in by.get(u, ()):
            ref = sc['refs'][rk]; (ck, fl, isactor, nm, pos, rot, scale, ep, tel, lock, cnt) = ref
            b = sc['bases'][nm]
            flags = (1 if fl & 0x400 else 0) | (2 if sig == 'LVLN' else 0) | (4 if ep else 0) | (8 if lock else 0) | (16 if tel else 0) | (32 if ep and ep[1] & 1 else 0)
            cid = ck if u[0] == 'i' else ext.get((u[1], u[2], u[3]), ck)
            tcell = 0
            if tel and tel[0] in sc['refs']: tcell = sc['refs'][tel[0]][0]
            refidx[rk] = len(refrows)
            refrows.append((eid(rk), eid(nm), eid(cid), kind, R.SIG_ID[sig], flags, pos[0], pos[1], pos[2], rot[0], rot[1], rot[2], scale,
                            bmin[0], bmin[1], bmin[2], bmax[0], bmax[1], bmax[2],
                            eid(lock[1]) if lock else 0, lock[0] if lock else 0, lock[2] if lock else 0, min(max(cnt, 0), 65535),
                            eid(tel[0]) if tel else 0, *(tel[1:7] if tel else (0.0,) * 6), tel[7] if tel else 0, eid(tcell) if tcell else 0,
                            eid(ep[0]) if ep else 0, S.add(b[1]), S.add(b[3] or ''), 0))
    for mi, k in enumerate(mkeys):
        pi, nv = sc['navm'][k]; m = F.parse_np(nv); rk = _res(sc, pi)
        nt = len(m['tris']); nvx = len(m['verts'])
        v = m['verts']
        for x, y, z in v.tolist(): verts.append((x, y, z))
        ntl = 0; nlk = 0
        # link table owner edge: external edge value indexes the link table
        owner = {}
        for ti, t in enumerate(m['tris'].tolist()):
            v0, v1, v2, n0, n1, n2, fl, cv = t[0][0], t[0][1], t[0][2], t[1][0], t[1][1], t[1][2], t[2], t[3]
            nb = [n0, n1, n2]
            for e in range(3):
                if fl & (1 << e): owner[nb[e]] = (ti, e); nb[e] = -1
            tris.append((v0, v1, v2, nb[0], nb[1], nb[2], fl, cv))
        nl = 0
        if m['links'] is not None:
            for li, (kind, tm, tt) in enumerate(m['links'].tolist()):
                ti, e = owner.get(li, (0, 0)); tk = rk(tm)
                links.append((mi, ti, e, kind, eid(tk), keyidx.get(tk, -1), tt, 0)); nl += 1
        nd = 0
        if m['doors'] is not None:
            for (ti, crc, dref) in m['doors'].tolist():
                dk = rk(dref); doors.append((mi, ti, 0, crc, eid(dk), refidx.get(dk, -1))); nd += 1
        ck = m['cell'] and rk(m['cell']) or 0
        wk = rk(m['world']) if m['world'] else 0
        gx = m['gridX'] if m['world'] else fmt.NOGRID; gy = m['gridY'] if m['world'] else fmt.NOGRID
        cellk = ck if not m['world'] else ext.get((wk, gx, gy), 0)
        mn = v.min(axis=0).tolist(); mx = v.max(axis=0).tolist()
        meshes.append((eid(k), eid(cellk), eid(wk), gx, gy, vbase, nvx, tbase, nt, lbase, nl, dbase, nd, *mn, *mx))
        vbase += nvx; tbase += nt; lbase += nl; dbase += nd
        if cellk and cellk in cells: cellinfo[cellk] = cells[cellk]
    cell_meta = []
    for ck, c in sorted(cellinfo.items()):
        cell_meta.append(dict(id='%08X' % eid(ck), edid=c['edid'], interior=bool(c['flags'] & 1), gridX=c['grid'][0] if c['grid'] else None,
                              gridY=c['grid'][1] if c['grid'] else None, flags=c['flags'], waterHeight=c['water']))
    order = sorted(env['plug'].items(), key=lambda kv: kv[1]); plugs = [S.add(n) for n, _ in order]
    kinds = {}
    for r in refrows: kinds[r[3]] = kinds.get(r[3], 0) + 1
    meta = dict(tool='envextract', formatVersion=fmt.VERSION, toolVersion=TOOL_VERSION, name=name or '', set=setname, **{'class': cls},
                interior=unit[0] == 'i', unit=list(unit[1:]) if unit[0] == 'x' else ['%08X' % unit[1]],
                plugins=[n for n, _ in order], cells=cell_meta, features=feat, refCountsByKind={str(k): v for k, v in sorted(kinds.items())},
                counts=dict(meshes=len(meshes), verts=len(verts), tris=len(tris), links=len(links), doors=len(doors), refs=len(refrows)))
    fmt.write(path, meta, plugs, S, meshes, verts, tris, links, doors, refrows)
    return meta
