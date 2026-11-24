"""Write one .mfoenv for a candidate unit from a scan."""
import math, struct
import fmt, navm as N, refs as R, features as F, land, anomaly as AN

TOOL_VERSION = 3
TF_CUT0 = 0x1000          # TRIS.flags: internal edge k (bit 0x1000 << k) was one-way or not the swapped edge in the game data; the neighbour is cut (nb = -1)
LF_STALE_TARGET, LF_NO_OWNER_EDGE, LF_DZ_UNKNOWN = 1, 2, 4   # LINK.flags bits



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


def water_height(sc, cell):
    """-> (height or None, source).  CELL XCLW marker -> WRLD DNAM default (exterior cells that have water) -> explicit None.
    A cell without the 'has water' bit (CELL DATA 0x2) has none; XCLW sentinels (FLT_MAX, about -2.1e9) mean 'not set'."""
    if not cell['flags'] & 2: return None, 'none'
    w = cell.get('water')
    if w is not None and -1e8 < w < 1e8: return float(w), 'cell'
    if not cell['flags'] & 1:
        wd = sc['worlds'].get(cell['world'])
        if wd and wd[1] and -1e8 < wd[1][1] < 1e8: return float(wd[1][1]), 'world'
    return None, 'none'


def components(tris, links, mesh_tri_base, kinds):
    """union-find over every triangle of the env.  tris: list of (.., nb0, nb1, nb2 at [3:6]) in file order, mesh_tri_base[mi]
    = first TRIS row of mesh mi.  links: LINK rows.  -> list of component ids per triangle, ordered by size (0 = biggest)."""
    n = len(tris); par = list(range(n))
    def find(x):
        while par[x] != x: par[x] = par[par[x]]; x = par[x]
        return x
    def uni(a, b):
        a, b = find(a), find(b)
        if a != b: par[a] = b
    for mi, base in enumerate(mesh_tri_base):
        end = mesh_tri_base[mi + 1] if mi + 1 < len(mesh_tri_base) else n
        for g in range(base, end):
            for e in range(3):
                nb = tris[g][3 + e]
                if nb >= 0: uni(g, base + nb)
    for l in links:
        if l[3] in kinds and l[5] >= 0 and not l[7] & LF_STALE_TARGET and l[2] != 0xFF:
            uni(mesh_tri_base[l[0]] + l[1], mesh_tri_base[l[5]] + l[6])
    cnt = {}
    for g in range(n): r = find(g); cnt[r] = cnt.get(r, 0) + 1
    rank = {r: i for i, (r, _) in enumerate(sorted(cnt.items(), key=lambda kv: (-kv[1], kv[0])))}
    return [min(rank[find(g)], 65535) for g in range(n)]


def _seg_pt(p, a, b):
    """closest point on segment ab to p, and its distance"""
    ax, ay, az = a; dx, dy, dz = b[0] - ax, b[1] - ay, b[2] - az; L = dx * dx + dy * dy + dz * dz
    t = 0.0 if L == 0 else max(0.0, min(1.0, ((p[0] - ax) * dx + (p[1] - ay) * dy + (p[2] - az) * dz) / L))
    q = (ax + t * dx, ay + t * dy, az + t * dz)
    return q, ((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2) ** 0.5


def seam_rows(links, meshes, verts, tris, min_gap=0.25):
    """SEAM rows for kind 0 links whose edge does not meet the target triangle: (linkIdx, gap, from[3], to[3]).  gap = nearest approach between the
    owner edge and the edges of the target triangle (endpoint to edge pairs); from = the point on the owner edge, to = the point on the target edge."""
    out = []
    for li, l in enumerate(links):
        mi, ti, e, kind, _, tmi, tt, lf, _dz = l
        if kind != 0 or tmi < 0 or lf & (LF_STALE_TARGET | LF_NO_OWNER_EDGE): continue
        m = meshes[mi]; t = tris[m[7] + ti]; vb = m[5]
        a = verts[vb + t[e]]; b = verts[vb + t[(e + 1) % 3]]
        tm = meshes[tmi]; tv = [verts[tm[5] + i] for i in tris[tm[7] + tt][:3]]
        best = None
        for j in range(3):
            c, d = tv[j], tv[(j + 1) % 3]
            for p, seg, flip in ((a, (c, d), False), (b, (c, d), False), (c, (a, b), True), (d, (a, b), True)):
                q, dist = _seg_pt(p, *seg)
                if best is None or dist < best[0]: best = (dist, (q, p) if flip else (p, q))
        if best[0] > min_gap: out.append((li, best[0], *best[1][0], *best[1][1]))
    return out


DOOR_DZ = 100.0        # a door joins a triangle only when the joined point is within this height of the door's origin
DOOR_NEAR = 200.0      # ... and, when no triangle contains the door's position, within this horizontal distance


def _closest_on_segments(p, a, b):
    """vectorised: closest point on each segment a[i]-b[i] to p (3,) -> (n,3)"""
    import numpy as np
    d = b - a; L = (d * d).sum(1); L[L == 0] = 1.0
    t = np.clip(((p - a) * d).sum(1) / L, 0.0, 1.0)
    return a + t[:, None] * d


def door_rows(doorinfo, doors, meshes, verts, tris):
    """DREF rows: every door ref joined to its navmesh triangle.  The exact one from a DOOR row when the ref has one; else the triangle whose footprint
    CONTAINS the door's x,y (the one nearest in height), else the triangle with the nearest point to the door; either way the joined point must be
    within DOOR_DZ of the door in height (and a nearest point within DOOR_NEAR horizontally), else -1.  doorinfo = [(refIdx, pos, openDefault,
    lockLevel, teleport, startsDisabled, keyId)].  Row: (refIdx, meshIdx, tri, distance to the joined point, ...)."""
    import numpy as np
    exact = {d[5]: (d[0], d[1]) for d in doors if d[5] >= 0}
    A = []; B = []; Cc = []; owner = []
    if tris:
        V = np.asarray(verts, dtype=np.float64)
        for mi, m in enumerate(meshes):
            T = np.asarray([t[:3] for t in tris[m[7]:m[7] + m[8]]], dtype=np.int64)
            if len(T): A.append(V[m[5] + T[:, 0]]); B.append(V[m[5] + T[:, 1]]); Cc.append(V[m[5] + T[:, 2]]); owner += [(mi, i) for i in range(len(T))]
    A = np.concatenate(A) if A else np.zeros((0, 3)); B = np.concatenate(B) if B else np.zeros((0, 3)); Cc = np.concatenate(Cc) if Cc else np.zeros((0, 3))
    if len(A):
        lo = np.minimum(np.minimum(A, B), Cc); hi = np.maximum(np.maximum(A, B), Cc)
    out = []
    for ri, pos, openDef, lock, tele, off, key in doorinfo:
        tail = (int(openDef), lock, tele, off, key)
        if ri in exact: mi, ti = exact[ri]; out.append((ri, mi, ti, 0.0, *tail)); continue
        hit = None
        if len(A):
            p = np.asarray(pos, dtype=np.float64)
            sel = np.nonzero((lo[:, 0] <= p[0] + DOOR_NEAR) & (hi[:, 0] >= p[0] - DOOR_NEAR) & (lo[:, 1] <= p[1] + DOOR_NEAR) & (hi[:, 1] >= p[1] - DOOR_NEAR))[0]
            if len(sel):
                a, b, c = A[sel], B[sel], Cc[sel]
                v0 = b - a; v1 = c - a; v2 = p - a                               # barycentric coordinates of (x, y) in each triangle
                den = v0[:, 0] * v1[:, 1] - v1[:, 0] * v0[:, 1]; ok = np.abs(den) > 1e-9; den[~ok] = 1.0
                u = (v2[:, 0] * v1[:, 1] - v1[:, 0] * v2[:, 1]) / den; w = (v0[:, 0] * v2[:, 1] - v2[:, 0] * v0[:, 1]) / den
                inside = ok & (u >= 0) & (w >= 0) & (u + w <= 1)
                zplane = a[:, 2] + u * v0[:, 2] + w * v1[:, 2]
                dz = np.abs(p[2] - zplane)
                cand = np.nonzero(inside & (dz <= DOOR_DZ))[0]
                if len(cand): j = cand[int(dz[cand].argmin())]; hit = (int(sel[j]), 0.0)
                else:
                    # nearest point on the edges of every triangle (a point inside a triangle's footprint was the containing case above); the point must be within DOOR_DZ in height and DOOR_NEAR horizontally
                    bestd = None
                    for e0, e1 in ((a, b), (b, c), (c, a)):
                        q = _closest_on_segments(p, e0, e1); d3 = np.linalg.norm(q - p, axis=1)
                        good = (np.abs(q[:, 2] - p[2]) <= DOOR_DZ) & (np.linalg.norm(q[:, :2] - p[:2], axis=1) <= DOOR_NEAR)
                        d3 = np.where(good, d3, np.inf); k = int(d3.argmin())
                        if np.isfinite(d3[k]) and (bestd is None or d3[k] < bestd[0]): bestd = (float(d3[k]), int(sel[k]))
                    if bestd: hit = (bestd[1], bestd[0])
        if hit: mi, ti = owner[hit[0]]; out.append((ri, mi, ti, hit[1], *tail))
        else: out.append((ri, -1, -1, 0.0, *tail))
    return out


def unit_cells(unit, win):
    if unit[0] == 'i': return [unit]
    return [('x', unit[1], unit[2] + dx, unit[3] + dy) for dx in range(-win, win + 1) for dy in range(-win, win + 1)]


def export(sc, unit, groups, by, cls, feat, path, setname, win=1, name=None):
    env = dict(plug={}); eid = _ids(sc, env); S = fmt.Strings()
    units = unit_cells(unit, win)
    mkeys = sorted(k for u in units for k in groups.get(u, ()))
    keyidx = {k: i for i, k in enumerate(mkeys)}
    meshes = []; verts = []; tris = []; links = []; doors = []; cellinfo = {}
    vbase = tbase = lbase = dbase = 0
    cells = sc['cells']; cache = {}
    def pm(k):
        if k not in cache: cache[k] = F.parse_np(sc['navm'][k][1])
        return cache[k]
    ext = {}
    for ck, c in cells.items():
        if c['grid'] is not None and not c['flags'] & 1 and not c['persist']: ext[(c['world'], c['grid'][0], c['grid'][1])] = ck
    refrows = []; refidx = {}; nstart_off = 0; doorinfo = []; ncut = 0
    for u in units:
        for rk, (kind, sig, bmin, bmax) in by.get(u, ()):
            ref = sc['refs'][rk]; (ck, fl, isactor, nm, pos, rot, scale, ep, tel, lock, cnt) = ref[:11]
            owner = ref[11] if len(ref) > 11 else 0; xact = ref[12] if len(ref) > 12 else 0; inherited = 0
            if not owner: owner = cells.get(ck, {}).get('owner', 0); inherited = 1 if owner else 0
            ownbit = (128 if owner and sc['bases'].get(owner, ('',))[0] == 'NPC_' else 0) | (256 if inherited else 0)
            b = sc['bases'][nm]
            off = R.start_disabled(sc, rk); nstart_off += off
            flags = (1 if fl & 0x400 else 0) | (2 if sig == 'LVLN' else 0) | (4 if ep else 0) | (8 if lock else 0) | (16 if tel else 0) | \
                    (32 if ep and ep[1] & 1 else 0) | (64 if off else 0) | ownbit
            cid = ck if u[0] == 'i' else ext.get((u[1], u[2], u[3]), ck)
            tcell = 0
            if tel and tel[0] in sc['refs']: tcell = sc['refs'][tel[0]][0]
            refidx[rk] = len(refrows)
            refrows.append((eid(rk), eid(nm), eid(cid), kind, R.SIG_ID[sig], flags, pos[0], pos[1], pos[2], rot[0], rot[1], rot[2], scale,
                            bmin[0], bmin[1], bmin[2], bmax[0], bmax[1], bmax[2],
                            eid(lock[1]) if lock else 0, lock[0] if lock else 0, lock[2] if lock else 0, min(max(cnt, 0), 65535),
                            eid(tel[0]) if tel else 0, *(tel[1:7] if tel else (0.0,) * 6), tel[7] if tel else 0, eid(tcell) if tcell else 0,
                            eid(ep[0]) if ep else 0, S.add(b[1]), S.add(b[3] or ''), eid(owner) if owner else 0))
            if kind == R.K_DOOR: doorinfo.append((len(refrows) - 1, pos, (xact & 8) != 0, lock[0] if lock else 0, 1 if tel else 0, 1 if off else 0, eid(lock[1]) if lock else 0))
    tri_base_of = []
    for mi, k in enumerate(mkeys):
        pi, nv = sc['navm'][k]; m = pm(k); rk = _res(sc, pi)
        nt = len(m['tris']); nvx = len(m['verts']); v = m['verts']
        tri_base_of.append(tbase)
        for x, y, z in v.tolist(): verts.append((x, y, z))
        owner = {}; ow, ns, _int = AN.edge_scan(m); cut = set(ow) | set(ns)
        for ti, t in enumerate(m['tris'].tolist()):
            v0, v1, v2, n0, n1, n2, fl, cv = t[0][0], t[0][1], t[0][2], t[1][0], t[1][1], t[1][2], t[2], t[3]
            nb = [n0, n1, n2]
            for e in range(3):
                if fl & (1 << e): owner[nb[e]] = (ti, e); nb[e] = -1
                elif (ti, e) in cut: nb[e] = -1; fl |= TF_CUT0 << e; ncut += 1
            tris.append((v0, v1, v2, nb[0], nb[1], nb[2], fl, cv))
        nl = 0
        if m['links'] is not None:
            for li, (kind, tm, tt) in enumerate(m['links'].tolist()):
                tk = rk(tm); lf = 0
                if li in owner: ti, e = owner[li]
                else: ti, e = 0, 0xFF; lf |= LF_NO_OWNER_EDGE      # a link row no triangle edge points at (counted by validate)
                dz = 0.0
                if tk in sc['navm'] and e != 0xFF:
                    B = pm(tk)
                    if tt >= len(B['tris']): lf |= LF_STALE_TARGET | LF_DZ_UNKNOWN       # stale: the neighbour was edited, the row was not
                    else:
                        t = m['tris'][ti]; a = v[t['v'][e]]; b = v[t['v'][(e + 1) % 3]]
                        c = B['verts'][B['tris'][tt]['v'].astype(int)]
                        dz = float(c[:, 2].mean() - (a[2] + b[2]) / 2)
                else: lf |= LF_DZ_UNKNOWN
                links.append((mi, ti, e, kind, eid(tk), keyidx.get(tk, -1), tt, lf, dz)); nl += 1
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
    walk = components(tris, links, tri_base_of, (0,))
    weak = components(tris, links, tri_base_of, (0, 1, 2))
    tcmp = list(zip(walk, weak))
    dref = door_rows(doorinfo, doors, meshes, verts, tris)
    seam = seam_rows(links, meshes, verts, tris)
    land_rows = []
    if unit[0] == 'x':
        for u in units:
            ck = ext.get((u[1], u[2], u[3]))
            if ck in sc.get('land', {}):
                land_rows.append((u[2], u[3], *[h for row in land.decode_vhgt(sc['land'][ck]) for h in row]))
    # component summary (area from the triangles)
    area = {}; ntri = {}
    mi = 0
    for g, t in enumerate(tris):
        while mi + 1 < len(tri_base_of) and tri_base_of[mi + 1] <= g: mi += 1
        vb = meshes[mi][5]; a, b, c = verts[vb + t[0]], verts[vb + t[1]], verts[vb + t[2]]
        ux, uy, uz, wx, wy, wz = b[0] - a[0], b[1] - a[1], b[2] - a[2], c[0] - a[0], c[1] - a[1], c[2] - a[2]
        ar = 0.5 * ((uy * wz - uz * wy) ** 2 + (uz * wx - ux * wz) ** 2 + (ux * wy - uy * wx) ** 2) ** 0.5
        k = walk[g]; area[k] = area.get(k, 0.0) + ar; ntri[k] = ntri.get(k, 0) + 1
    comp_meta = [dict(id=k, tris=ntri[k], areaUnits2=round(area[k])) for k in sorted(ntri)[:16]]
    cell_meta = []
    for ck, c in sorted(cellinfo.items()):
        wh, src = water_height(sc, c)
        cell_meta.append(dict(id='%08X' % eid(ck), edid=c['edid'], interior=bool(c['flags'] & 1), gridX=c['grid'][0] if c['grid'] else None,
                              gridY=c['grid'][1] if c['grid'] else None, flags=c['flags'], quadHide=c.get('quad', 0), waterHeight=wh, waterSource=src, owner=eid(c['owner']) if c.get('owner') else 0))
    world_meta = []
    for wk in sorted({u[1] for u in units if u[0] == 'x'}):
        wd = sc['worlds'].get(wk)
        par = wd[2] if wd and len(wd) > 2 else 0; pf = wd[3] if wd and len(wd) > 3 else 0
        world_meta.append(dict(id='%08X' % eid(wk), edid=wd[0] if wd else '', parent='%08X' % eid(par) if par else None, parentFlags=pf,
                               useParentLand=bool(par and pf & 1)))      # a child world that uses its parent's land data: ignore the LAND section for it
    order = sorted(env['plug'].items(), key=lambda kv: kv[1]); plugs = [S.add(n) for n, _ in order]
    kinds = {}
    for r in refrows: kinds[r[3]] = kinds.get(r[3], 0) + 1
    import library
    bad = library.displaced_meshes(sc)
    ndisp = sum(1 for k in bad if library.unit_of_mesh(sc, k) in set(units))
    meta = dict(tool='envextract', formatVersion=fmt.VERSION, toolVersion=TOOL_VERSION, name=name or '', set=setname, **{'class': cls},
                interior=unit[0] == 'i', unit=list(unit[1:]) if unit[0] == 'x' else ['%08X' % unit[1]],
                plugins=[n for n, _ in order], cells=cell_meta, worlds=world_meta, features=feat, refCountsByKind={str(k): v for k, v in sorted(kinds.items())},
                counts=dict(landCells=len(land_rows), doorRefs=len(dref), seams=len(seam), meshes=len(meshes), verts=len(verts), tris=len(tris), links=len(links), doors=len(doors), refs=len(refrows),
                            refsStartDisabled=nstart_off, edgesCut=ncut, displacedMeshesDropped=ndisp, components=len(set(walk)), weakComponents=len(set(weak))),
                walkComponents=comp_meta)
    fmt.write(path, meta, plugs, S, meshes, verts, tris, links, doors, refrows, tcmp, dref, seam, land_rows)
    return meta
