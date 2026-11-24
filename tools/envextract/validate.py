"""Validators.  `validate --set NAME`: every NAVM of the set's plugins decoded strictly (consumed size == subrecord
size, every index in range), neighbour reciprocity (index and geometry), edge links, flag cross-checks.
`validate FILE...`: structural + semantic checks of written .mfoenv files.  stdlib only."""
import os, sys, json, struct, time
import plugin as P, navm as N, lists, fmt

CACHE = os.path.expanduser('~/.cache/mfo-env')


def _geo_equal(a, b, eps=0.51):
    return abs(a[0] - b[0]) <= eps and abs(a[1] - b[1]) <= eps and abs(a[2] - b[2]) <= eps


def check_mesh(m, st):
    """update stats dict st with one decoded mesh (navm.parse result)"""
    V = m['verts']; T = m['tris']
    st['meshes'] += 1; st['tris'] += len(T); st['verts'] += len(V)
    owner = {}
    for ti, t in enumerate(T):
        for e in range(3):
            if t[6] & (1 << e): owner[t[3 + e]] = (ti, e)
    for li in range(len(m['links'])):
        st['links'] += 1
        if li not in owner: st['link_without_edge'] += 1
    dset = {d[0] for d in m['doors']}
    for ti, t in enumerate(T):
        fl = t[6]
        if bool(fl & N.TF_DOOR) != (ti in dset): st['door_flag_mismatch'] += 1; st.setdefault('door_mismatch_kinds', []).append('flag=%d row=%d' % (bool(fl & N.TF_DOOR), ti in dset))
        else:
            if fl & N.TF_DOOR: st['door_flag_ok'] += 1
        for e in range(3):
            ext = bool(fl & (1 << e)); n = t[3 + e]
            a = V[t[e]]; b = V[t[(e + 1) % 3]]
            if ext: st['edges_external'] += 1; continue
            if n == -1: st['edges_open'] += 1; continue
            st['edges_internal'] += 1
            u = T[n]; found = False; geo = False; same_dir = False
            for e2 in range(3):
                if not u[6] & (1 << e2) and u[3 + e2] == ti:
                    found = True
                    c = V[u[e2]]; d = V[u[(e2 + 1) % 3]]
                    if _geo_equal(a, d) and _geo_equal(b, c): geo = True; break         # swapped edge: the same two points, opposite direction
                    if _geo_equal(a, c) and _geo_equal(b, d): same_dir = True
            if same_dir and not geo: st['internal_same_direction'] = st.get('internal_same_direction', 0) + 1
            if found: st['internal_reciprocal'] += 1
            else: st['internal_one_way'] += 1
            if geo: st['internal_geometry_match'] += 1
            elif found: st['internal_geometry_mismatch'] += 1      # reciprocal by index but not the swapped edge
    return owner


def navmesh_walk(pg, name):
    for lab, s, e in pg.top_groups():
        if lab in (b'CELL', b'WRLD'):
            for sig, fl, fid, o, size, c in pg.walk(s, e):
                if sig == b'NAVM': yield fid, fl, o, size


def water_check(setname):
    """how well triangle flag 0x200 tracks the water level: needs the scan pickle (envextract.py scan)."""
    import numpy as np, scan, features as F
    p = os.path.join(CACHE, '_scan', setname + '.pkl')
    if not os.path.exists(p): return None
    sc = scan.load(p); ext = {}
    for ck, c in sc['cells'].items():
        if c['grid'] is not None and not c['flags'] & 1 and not c['persist']: ext[(c['world'],) + tuple(c['grid'])] = c
    tot = dict(tri=0, flagged=0, flagged_below=0, below=0, below_unflagged=0, flagged_above_plus50=0)
    for k, (pi, nv) in sc['navm'].items():
        ni = sc['navinfo'][k]
        if not ni[0]: continue
        c = ext.get((ni[0], ni[1], ni[2]))
        if c is None: continue
        import export
        w, _src = export.water_height(sc, c)
        if w is None: continue
        m = F.parse_np(nv); fl = m['tris']['fl']; z = m['verts'][m['tris']['v'].astype(np.int64)][:, :, 2].mean(1); wf = (fl & 0x200) != 0
        tot['tri'] += len(fl); tot['flagged'] += int(wf.sum()); tot['flagged_below'] += int((wf & (z < w)).sum()); tot['below'] += int((z < w).sum())
        tot['below_unflagged'] += int(((~wf) & (z < w)).sum()); tot['flagged_above_plus50'] += int((wf & (z > w + 50)).sum())
    return tot


def land_check(setname, sample=300):
    """LAND heights against the navmesh: decoded terrain should lie at or under the navmesh (the navmesh sits on terrain, rocks and
    buildings); reports the share of navmesh vertices within 16 units of the terrain and the shares where the terrain is above."""
    import numpy as np, scan, features as F, land, random
    p = os.path.join(CACHE, '_scan', setname + '.pkl')
    if not os.path.exists(p): return None
    sc = scan.load(p)
    if not sc.get('land'): return None
    ext = {}
    for ck, c in sc['cells'].items():
        if c['grid'] is not None and not c['flags'] & 1 and not c['persist']: ext[(c['world'],) + tuple(c['grid'])] = ck
    ks = [k for k, v in sc['navinfo'].items() if v[0] and (v[0], v[1], v[2]) in ext and ext[(v[0], v[1], v[2])] in sc['land']]
    random.seed(1); err = []
    for k in random.sample(ks, min(sample, len(ks))):
        ni = sc['navinfo'][k]; g = land.decode_vhgt(sc['land'][ext[(ni[0], ni[1], ni[2])]]); v = F.parse_np(sc['navm'][k][1])['verts']
        for i in range(len(v)):
            lx = v[i, 0] - ni[1] * 4096; ly = v[i, 1] - ni[2] * 4096
            if 0 <= lx <= 4096 and 0 <= ly <= 4096: err.append(land.height_at(g, lx, ly) - float(v[i, 2]))
    e = np.array(err)
    return dict(vertices=len(e), within16=float((np.abs(e) < 16).mean()), terrainAbove16=float((e > 16).mean()), terrainAbove64=float((e > 64).mean()), landCells=len(sc['land']))


def run_set(setname, out=None):
    order, where, info = lists.resolve_set(setname)
    st = dict(plugins=len(order), records=0, deleted=0, parsed=0, failed=0, meshes=0, tris=0, verts=0, links=0, link_without_edge=0, door_flag_mismatch=0,
              door_flag_ok=0, edges_external=0, edges_open=0, edges_internal=0, internal_reciprocal=0, internal_one_way=0,
              internal_geometry_match=0, internal_geometry_mismatch=0)
    fails = []; win = {}; t0 = time.time()
    for name in order:
        pg = P.Plugin(where[name.lower()])
        for fid, fl, o, size in navmesh_walk(pg, name):
            st['records'] += 1
            key = pg.resolve(fid, name)
            if fl & P.F_DELETED: st['deleted'] += 1; win.pop(key, None); continue
            nv = dict(P.subrecords(pg.data, o, size, fl)).get(b'NVNM')
            if nv is None: continue
            try: m = N.parse(nv)
            except N.NavmeshError as ex:
                st['failed'] += 1; fails.append((name, '%08X' % fid, str(ex))); continue
            st['parsed'] += 1; check_mesh(m, st)       # every version, overridden or not
            rk = lambda raw, pg=pg, name=name: pg.resolve(raw, name) if raw else None
            win[key] = (m, rk)
    # external links over the WINNING meshes: target exists, reverse link exists, geometry meets
    ext = dict(links=0, target_missing=0, target_tri_out_of_range=0, reverse_found=0, reverse_missing=0, geometry_within_4=0, geometry_within_32=0, geometry_far_kind0=0, geometry_far_kind12=0)
    for key, (m, rk) in win.items():
        owner = {}
        for ti, t in enumerate(m['tris']):
            for e in range(3):
                if t[6] & (1 << e): owner[t[3 + e]] = (ti, e)
        for li, (kind, tm, tt) in enumerate(m['links']):
            ext['links'] += 1; tk = rk(tm)
            if tk not in win: ext['target_missing'] += 1; continue
            B = win[tk][0]
            if tt >= len(B['tris']): ext['target_tri_out_of_range'] += 1; continue
            ti, e = owner.get(li, (None, None))
            if ti is None: continue
            # reverse: B has a link whose target is (key, ti)?
            brk = win[tk][1]; rev = False
            bo = {}
            for bti, bt in enumerate(B['tris']):
                for be in range(3):
                    if bt[6] & (1 << be): bo[bt[3 + be]] = (bti, be)
            for bli, (k2, tm2, tt2) in enumerate(B['links']):
                if brk(tm2) == key and tt2 == ti and bo.get(bli, (None,))[0] == tt: rev = True; break
            ext['reverse_found' if rev else 'reverse_missing'] += 1
            a = m['verts'][m['tris'][ti][e]]; b = m['verts'][m['tris'][ti][(e + 1) % 3]]
            # the target triangle must have a vertex pair that touches this edge's line at both ends (loose 4 unit match)
            tv = [B['verts'][i] for i in B['tris'][tt][:3]]
            dmin = 1e9
            for j in range(3):   # nearest approach between this edge and the target triangle's edges
                c, d = tv[j], tv[(j + 1) % 3]
                dmin = min(dmin, _geo_dist_seg(a, c, d), _geo_dist_seg(b, c, d), _geo_dist_seg(c, a, b), _geo_dist_seg(d, a, b))
            ext['geometry_within_4' if dmin < 4 else 'geometry_within_32' if dmin < 32 else 'geometry_far_kind%s' % ('0' if kind == 0 else '12')] += 1
    try: st['land'] = land_check(setname)
    except ImportError: st['land'] = None
    try: st['water'] = water_check(setname)
    except ImportError: st['water'] = None
    st['external'] = ext; st['seconds'] = round(time.time() - t0, 1); st['failures'] = fails[:20]
    pct = lambda a, b: (100.0 * a / b) if b else 100.0
    print('set %s: %d plugins, %d NAVM records (%d deleted), %d decoded, %d FAILED' % (setname, st['plugins'], st['records'], st['deleted'], st['parsed'], st['failed']))
    print('  strict decode: consumed size == subrecord size and all indices in range for %.4f%% of records' % pct(st['parsed'], st['parsed'] + st['failed']))
    print('  door flag 0x400 <-> door row: %d agree, %d mismatch;  link rows with no flagged edge: %d' % (st['door_flag_ok'], st['door_flag_mismatch'], st['link_without_edge']))
    print('  internal edges: %d, reciprocal by index %.3f%%, shared edge is the swapped edge (same two points, opposite direction) %.3f%% (of internal)' % (
        st['edges_internal'], pct(st['internal_reciprocal'], st['edges_internal']), pct(st['internal_geometry_match'], st['edges_internal'])))
    print('  external links of winning meshes: %d, target mesh exists %.2f%%, reverse link back to us %.2f%%, edge within 4 units of target tri %.2f%%, within 32 %.2f%%, farther: kind0 %d, kind1/2 %d' % (
        ext['links'], pct(ext['links'] - ext['target_missing'], ext['links']), pct(ext['reverse_found'], ext['links']), pct(ext['geometry_within_4'], ext['links']),
        pct(ext['geometry_within_4'] + ext['geometry_within_32'], ext['links']), ext['geometry_far_kind0'], ext['geometry_far_kind12']))
    if st.get('land'): l = st['land']; print('  LAND heights vs navmesh (%d vertices): within 16 units %.1f%%, terrain more than 16 above the navmesh %.2f%%, more than 64 above %.3f%%' % (l['vertices'], 100 * l['within16'], 100 * l['terrainAbove16'], 100 * l['terrainAbove64']))
    if st.get('water'): w = st['water']; print('  water flag 0x200: %.2f%% of flagged triangles lie below the cell water height, %.2f%% of triangles below water are flagged' % (100.0 * w['flagged_below'] / max(w['flagged'], 1), 100.0 * w['below'] and 100.0 * (w['below'] - w['below_unflagged']) / w['below']))
    if out or True:
        p = out or os.path.join(CACHE, 'validate-%s.json' % setname); os.makedirs(os.path.dirname(p), exist_ok=True); json.dump(st, open(p, 'w'), indent=1); print('  stats ->', p)
    return 0 if st['failed'] == 0 else 1


def _geo_dist_seg(p, a, b):
    ax, ay, az = a; bx, by, bz = b; px, py, pz = p
    dx, dy, dz = bx - ax, by - ay, bz - az; L = dx * dx + dy * dy + dz * dz
    t = 0.0 if L == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy + (pz - az) * dz) / L))
    return ((px - ax - t * dx) ** 2 + (py - ay - t * dy) ** 2 + (pz - az - t * dz) ** 2) ** 0.5


def run_file(path):
    e = fmt.read(path); bad = []; warn = 0; swapbad = 0; stale = noowner = 0
    M, V, T, L, D, R = e['meshes'], e['verts'], e['tris'], e['links'], e['doors'], e['refs']
    for mi, m in enumerate(M):
        vb, nv, tb, nt, lb, nl, db, nd = m[5:13]
        if vb + nv > len(V) or tb + nt > len(T) or lb + nl > len(L) or db + nd > len(D): bad.append('mesh %d section range' % mi); continue
        for ti in range(nt):
            t = T[tb + ti]
            if max(t[0:3]) >= nv: bad.append('mesh %d tri %d vertex' % (mi, ti)); continue
            for k in range(3):
                n = t[3 + k]
                if n >= nt or n < -1: bad.append('mesh %d tri %d nb' % (mi, ti)); continue
                if n < 0: continue
                u = T[tb + n]; ea = (V[vb + t[k]], V[vb + t[(k + 1) % 3]])
                back = [j for j in range(3) if u[3 + j] == ti]
                if not back: warn += 1; continue                     # one-way neighbours exist in the game data (README)
                if max(u[0:3]) >= nv: continue
                if not any(_geo_equal(ea[0], V[vb + u[(j + 1) % 3]]) and _geo_equal(ea[1], V[vb + u[j]]) for j in back): swapbad += 1
    if len(e['tcmp']) != len(T): bad.append('component table size')
    for l in L:
        if l[7] & 1: stale += 1
        if l[7] & 2: noowner += 1
        if l[7] & 2 and l[2] != 0xFF: bad.append('owner-less link edge')
        if l[0] >= len(M): bad.append('link owner')
        elif l[5] >= len(M): bad.append('link target idx')
        elif l[5] >= 0 and l[6] >= M[l[5]][8] and not l[7] & 1: bad.append('link target tri')
    for d in D:
        if d[0] >= len(M): bad.append('door mesh')
        if d[5] >= len(R): bad.append('door ref idx')
        elif d[5] >= 0 and R[d[5]][0] != d[4]: bad.append('door ref id')
    for r in R:
        if any(r[13 + i] > r[16 + i] for i in range(3)): bad.append('ref bounds')
        if r[3] > 8: bad.append('ref kind')
        if (r[0] >> 24) >= len(e['plugins']): bad.append('ref plugin idx')
    for d in e.get('dref', []):
        if d[0] >= len(R): bad.append('door row ref idx')
        elif R[d[0]][3] != 0: bad.append('door row not a door ref')
        if d[1] >= len(M) or (d[2] >= 0 and (d[1] < 0 or d[2] >= M[d[1]][8])): bad.append('door row triangle')
    for s in e.get('seam', []):
        if s[0] >= len(L): bad.append('seam link idx')
        elif L[s[0]][3] != 0: bad.append('seam on a non-portal link')
        if not s[1] > 0: bad.append('seam gap')
    for l in e.get('land', []):
        if any(h != h or abs(h) > 1e6 for h in l[2:]): bad.append('land height')
    for r in R:
        if r[35] and (r[35] >> 24) >= len(e['plugins']): bad.append('owner plugin idx')
    print('%s: %d meshes %d tris %d links %d doors %d refs -> %s' % (os.path.basename(path), len(M), len(T), len(L), len(D), len(R), ('OK' if not bad else 'BAD %s' % bad[:5]) + (' (%d one-way neighbour edges, as in the game data)' % warn if warn else '') + (' [%d neighbour pairs not the swapped edge]' % swapbad if swapbad else '')
          + (' [%d stale link targets, %d owner-less link rows]' % (stale, noowner) if stale or noowner else '')))
    return 0 if not bad else 1


def run(a):
    if a.set: return run_set(a.set)
    files = a.files
    if not files:
        for d, _, fs in os.walk(CACHE):
            files += [os.path.join(d, f) for f in fs if f.endswith('.mfoenv')]
    rc = 0
    for f in sorted(files): rc |= run_file(f)
    return rc
