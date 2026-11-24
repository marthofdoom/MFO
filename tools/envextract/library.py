"""Candidate environments, their features, classification and selection."""
import math, random, time
from multiprocessing import Pool
import numpy as np
import features as F, refs as R, anomaly as AN

WIN = 1                    # exterior window = (2*WIN+1)^2 grid cells around the centre cell
MAX_WATER = 0.5            # exterior windows with at least this share of water-flagged walkable area are not candidates
MIN_WINDOW_CELLS = 3000    # raster cells (64u) of walkable area a window needs to be a candidate (~12.3M units^2)
_S = {}


DISPLACED_GAP = 3000.0     # a mesh this far below its neighbours / its own original is a "removed" navmesh parked under the map
MEDIAN_GAP = 12000.0       # rule 2 needs a bigger gap: ordinary mountain terrain differs by several thousand units from its neighbours
FLAT = 16.0                # a parked mesh is flat: highest minus lowest vertex under this many units


def displaced_meshes(sc):
    """Navmeshes a plugin 'removed' by moving them far below where they belong.  Relative rules, no absolute height, no list value.
    (1) OVERRIDE rule: the winning record comes from a plugin that is not the record's owner, it is flat (height range < FLAT) and it
        sits more than DISPLACED_GAP away from the first record of that mesh (the owner's version, kept by the scan).  No neighbour
        count needed, and parked meshes cannot shield each other because the comparison is with the mesh's own history.
    (2) MEDIAN rule (meshes parked by their own plugin, or with no history): the mesh's highest vertex is more than MEDIAN_GAP (12000) under
        the MEDIAN of the lowest vertices of the other meshes of its cell (interior, >= 2 others) or of its 3x3 cells (exterior, >= 3
        others), where meshes already caught by rule 1 and flat meshes do not count as others."""
    if '_displaced' in sc: return sc['_displaced']
    from collections import defaultdict
    info = {}; grp = defaultdict(list)
    for k, (pi, nv) in sc['navm'].items():
        ni = sc['navinfo'].get(k)
        if ni is None: continue
        v = F.parse_np(nv)['verts']
        if not len(v): continue
        info[k] = (float(v[:, 2].min()), float(v[:, 2].max()))
        if ni[0]: grp[('x', ni[0], ni[1], ni[2])].append(k)
        elif ni[3]: grp[('i', ni[3])].append(k)
    out = set(); orig = sc.get('navorig', {})
    for k, (zlo, zhi) in info.items():
        pi = sc['navm'][k][0]
        if (k >> 24) != pi and zhi - zlo < FLAT and k in orig:
            olo, ohi = orig[k]
            if abs((zlo + zhi) / 2 - (olo + ohi) / 2) > DISPLACED_GAP: out.add(k)
    r1 = len(out)
    def others_min(ks, k): return [info[j][0] for j in ks if j != k and j not in out and info[j][1] - info[j][0] >= FLAT]
    for u, ks in grp.items():
        if u[0] == 'i':
            for k in ks:
                if k in out: continue
                o = others_min(ks, k)
                if len(o) >= 2 and info[k][1] < float(np.median(o)) - MEDIAN_GAP: out.add(k)
        else:
            nb = [j for dx in (-1, 0, 1) for dy in (-1, 0, 1) for j in grp.get(('x', u[1], u[2] + dx, u[3] + dy), ())]
            for k in ks:
                if k in out: continue
                o = others_min(nb, k)
                if len(o) >= 3 and info[k][1] < float(np.median(o)) - MEDIAN_GAP: out.add(k)
    sc['_displaced'] = out; sc['_displaced_rule1'] = r1; sc['_info'] = info; sc['_grp'] = grp
    return out


def doubtful_meshes(sc):
    """Meshes that look parked but are not certain: flat within 100 units and more than DISPLACED_GAP (3000) under the median of the lowest
    vertices of their non-flat neighbours (the displaced rules need 12000 or a history), and not already displaced.  The coverage library
    rejects places holding one."""
    if '_doubtful' in sc: return sc['_doubtful']
    out_d = displaced_meshes(sc); info = sc['_info']; grp = sc['_grp']; out = set()
    def others(ks, k): return [info[j][0] for j in ks if j != k and j not in out_d and info[j][1] - info[j][0] >= FLAT]
    for u, ks in grp.items():
        nb = ks if u[0] == 'i' else [j for dx in (-1, 0, 1) for dy in (-1, 0, 1) for j in grp.get(('x', u[1], u[2] + dx, u[3] + dy), ())]
        need = 2 if u[0] == 'i' else 3
        for k in ks:
            if k in out_d: continue
            o = others(nb, k)
            if len(o) >= need and info[k][1] - info[k][0] < 100.0 and info[k][1] < float(np.median(o)) - DISPLACED_GAP: out.add(k)
    sc['_doubtful'] = out
    return out


def make_groups(sc):
    """unit -> [navmesh keys], without the displaced ones"""
    bad = displaced_meshes(sc); groups = {}; sc['_noflag'] = 0
    for k in sc['navm']:
        ni = sc['navinfo'].get(k)
        if ni is None or (ni[3] is None and not ni[0]) or k in bad: continue
        if not ni[0] and not sc['cells'].get(ni[3], {}).get('flags', 0) & 1:    # an "interior" mesh whose cell is not flagged interior: META would contradict the cell
            sc['_noflag'] += 1; continue
        groups.setdefault(unit_of_mesh(sc, k), []).append(k)
    return groups


def reachable_cells(sc):
    """cells a character can walk to from an exterior cell through load doors (teleport doors that do not start
    disabled), over the refs the scan kept.  -> set of cell keys (every exterior cell plus the interiors reached)."""
    if '_reach' in sc: return sc['_reach']
    adj = {}
    for rk, r in sc['refs'].items():
        tel = r[8]
        if not tel or R.start_disabled(sc, rk): continue
        dest = sc['refs'].get(tel[0])
        if dest is None: continue
        adj.setdefault(r[0], set()).add(dest[0])
    seen = {ck for ck, c in sc['cells'].items() if c['world']}      # every cell of a worldspace; interiors have no world
    todo = list(seen)
    while todo:
        c = todo.pop()
        for d in adj.get(c, ()):
            if d not in seen: seen.add(d); todo.append(d)
    sc['_reach'] = seen
    return seen


def unit_of_mesh(sc, k):
    wk, gx, gy, ck = sc['navinfo'][k]
    if wk: return ('x', wk, gx, gy)
    return ('i', ck)


def _resolver(sc, k):
    rs = _S.setdefault('rs', {}); pi = sc['navm'][k][0]
    if pi not in rs:
        import export
        rs[pi] = export._res(sc, pi)
    return rs[pi]


def _mesh_stat(sc, k, pm):
    rk = _resolver(sc, k)
    def tm(key):
        return pm(key) if key in sc['navm'] else None
    st = AN.analyze_mesh(pm(k), rk, tm)
    st['verts'] = len(pm(k)['verts']); st['area3d'] = st['a0'] + st['a1'] + st['a2'] + st['a3']
    return st


def _add(a, b):
    for k, v in b.items():
        a[k] = max(a.get(k, 0), v) if k == 'steepRise' else a.get(k, 0) + v


def _place(sc, keys, pm):
    """component facts of one place (union of meshes)"""
    meshes = [(k, pm(k)) for k in keys]; keyidx = {k: i for i, k in enumerate(keys)}
    def rt(k, raw): return _resolver(sc, k)(raw)
    share, islands, _, _ = AN.place_components(meshes, keyidx, rt)
    return dict(mainShare=share, islands=islands)


def _world_task(args):
    kind, ident = args; sc = _S['sc']; groups = _S['groups']
    out = []
    if kind == 'x':
        w = ident
        cells = [u for u in groups if u[0] == 'x' and u[1] == w]
        gxs = [u[2] for u in cells]; gys = [u[3] for u in cells]
        gx0, gy0 = min(gxs) - WIN, min(gys) - WIN
        W = (max(gxs) - min(gxs) + 1 + 2 * WIN) * 64; H = (max(gys) - min(gys) + 1 + 2 * WIN) * 64
        Rz = F.Raster(gx0 * 4096.0, gy0 * 4096.0, W, H); stats = {}; pcache = {}
        def pm(k):
            if k not in pcache: pcache[k] = F.parse_np(sc['navm'][k][1])
            return pcache[k]
        for u in cells:
            st = {}
            for k in groups[u]:
                m = pm(k); Rz.add(m['verts'], m['tris']); _add(st, _mesh_stat(sc, k, pm))
            stats[(u[2], u[3])] = st
        Rz.finish()
        have = set(stats)
        for (gx, gy) in stats:
            win = Rz.window((gx - WIN) * 4096.0, (gy - WIN) * 4096.0, (gx + WIN + 1) * 4096.0, (gy + WIN + 1) * 4096.0)
            if win is None or win['cells'] < MIN_WINDOW_CELLS: continue
            tot = {}
            for dx in range(-WIN, WIN + 1):
                for dy in range(-WIN, WIN + 1):
                    if (gx + dx, gy + dy) in have: _add(tot, stats[(gx + dx, gy + dy)])
            if win['water'] >= MAX_WATER: continue      # mostly water: a lake, not a place to fight or loot
            win.update(tot)
            keys = [k for dx in range(-WIN, WIN + 1) for dy in range(-WIN, WIN + 1) for k in groups.get(('x', w, gx + dx, gy + dy), ())]
            win.update(_place(sc, keys, pm)); win['placeKeys'] = len(keys); win['interior'] = False
            out.append((('x', w, gx, gy), win))
    else:
        for ck in ident:
            u = ('i', ck); pcache = {k: F.parse_np(sc['navm'][k][1]) for k in groups[u]}; ms = [pcache[k] for k in groups[u]]
            def pm(k, pcache=pcache):
                if k not in pcache: pcache[k] = F.parse_np(sc['navm'][k][1])
                return pcache[k]
            allv = np.concatenate([m['verts'] for m in ms])
            ox, oy = math.floor(allv[:, 0].min() / F.S) * F.S - F.S, math.floor(allv[:, 1].min() / F.S) * F.S - F.S
            W = int((allv[:, 0].max() - ox) / F.S) + 3; H = int((allv[:, 1].max() - oy) / F.S) + 3
            if W * H > 40_000_000: continue
            Rz = F.Raster(ox, oy, W, H); st = {}
            for k in groups[u]: Rz.add(pm(k)['verts'], pm(k)['tris']); _add(st, _mesh_stat(sc, k, pm))
            Rz.finish(); win = Rz.window(ox, oy, ox + W * F.S, oy + H * F.S)
            if win is None: continue
            win.update(st); win.update(_place(sc, groups[u], pm)); win['placeKeys'] = len(groups[u]); win['interior'] = True; out.append((u, win))
    return out


def build_candidates(sc, procs=6, log=print):
    groups = make_groups(sc); stats = dict(displaced=len(displaced_meshes(sc)))
    _S['sc'] = sc; _S['groups'] = groups
    worlds = {}
    for u in groups:
        if u[0] == 'x': worlds[u[1]] = worlds.get(u[1], 0) + 1
    tasks = [('x', w) for w, n in sorted(worlds.items(), key=lambda x: -x[1]) if n >= 4]
    ints = [u[1] for u in groups if u[0] == 'i']
    step = max(1, len(ints) // (procs * 8)); tasks += [('i', ints[i:i + step]) for i in range(0, len(ints), step)]
    t0 = time.time(); cands = {}
    with Pool(procs) as p:
        for res in p.imap_unordered(_world_task, tasks, chunksize=1):
            for u, win in res: cands[u] = win
    reach = reachable_cells(sc); n0 = len(cands)
    for u in [u for u in cands if u[0] == 'i' and u[1] not in reach]: del cands[u]
    stats['unreachable_interiors'] = n0 - len(cands)
    log('candidates: %d (%d interior) in %.0fs; displaced meshes dropped %d, interiors not reachable from an exterior cell dropped %d; meshes of cells without the interior flag dropped %d' % (
        len(cands), sum(1 for u in cands if u[0] == 'i'), time.time() - t0, stats['displaced'], stats['unreachable_interiors'], sc.get('_noflag', 0)))
    return groups, cands


def ref_features(sc, cands, min_static=R.DEFAULT_MIN_STATIC):
    """add placed-ref counts and the exported ref lists to each candidate.  -> {unit: [ (ref key, classify result) ]}"""
    by = {}
    cells = sc['cells']
    for rk, ref in sc['refs'].items():
        c = R.classify(sc, ref, min_static)
        if c is None: continue
        ck = ref[0]; cell = cells.get(ck)
        if cell is None: continue
        if cell['flags'] & 1 and not cell['persist']: u = ('i', ck)
        else:
            pos = ref[4]; u = ('x', cell['world'], int(math.floor(pos[0] / 4096.0)), int(math.floor(pos[1] / 4096.0)))
        by.setdefault(u, []).append((rk, c))
    names = {R.K_DOOR: 'nDoor', R.K_CONT: 'nCont', R.K_ACTI: 'nActi', R.K_FURN: 'nFurn', R.K_ITEM: 'nItem', R.K_ACTOR: 'nActor',
             R.K_STATIC: 'nLarge', R.K_FLORA: 'nFlora', R.K_TREE: 'nTree'}
    for u, f in cands.items():
        units = [u] if u[0] == 'i' else [('x', u[1], u[2] + dx, u[3] + dy) for dx in range(-WIN, WIN + 1) for dy in range(-WIN, WIN + 1)]
        for n in names.values(): f[n] = 0
        f['nLeveled'] = 0; f['nTele'] = 0; f['nLocked'] = 0; f['nStuff'] = 0
        for uu in units:
            for rk, (kind, sig, bmin, bmax) in by.get(uu, ()):
                if R.start_disabled(sc, rk): continue      # starts disabled: kept in the file, not counted
                f[names[kind]] += 1
                ref = sc['refs'][rk]
                if kind == R.K_ACTOR and sig == 'LVLN': f['nLeveled'] += 1
                if kind == R.K_DOOR:
                    if ref[8]: f['nTele'] += 1
                    if ref[9]: f['nLocked'] += 1
    for f in cands.values():
        f['nRefs'] = sum(f[n] for n in names.values())
        f['nStuff'] = f['nCont'] + f['nFurn']; mu = max(f.get('areaUnits2', 1e6) / 1e6, 0.01)
        f['clutter'] = (f['nItem'] + f['nCont'] + f['nActi'] + f['nFlora'] + f['nFurn']) / mu     # small things to loot / bump into, per million units^2
        f['occl'] = (f['nLarge'] + f['nTree']) / mu                                                # large statics and trees (occluders), per million units^2
    return by


def exterior_reach(cands, content, ring=(2, 3)):
    """sets f['reach'] (0/1) on every candidate.  Interiors: 1 (they were already filtered by reachable_cells).  An exterior window is reachable when it
    holds a teleport door, or a candidate of the same worldspace at Chebyshev distance in `ring` (a neighbouring window, touching or one cell apart) has
    content.  content(f) -> bool."""
    have = {u for u, f in cands.items() if u[0] == 'x' and content(f)}
    for u, f in cands.items():
        if u[0] == 'i': f['reach'] = 1; continue
        near = any(('x', u[1], u[2] + dx, u[3] + dy) in have for dx in range(-ring[1], ring[1] + 1) for dy in range(-ring[1], ring[1] + 1)
                   if ring[0] <= max(abs(dx), abs(dy)) <= ring[1])
        f['reach'] = 1 if f.get('nTele', 0) > 0 or near else 0


def place_flags(sc, units):
    """unit -> set of reason names (coverage.REASONS) that come from meshes / refs: displaced, doubtful, plugin-moved refs.  A place is flagged when
    any mesh of its cells (3x3 for an exterior window) or any moved ref falls in it."""
    gk = lambda k: unit_of_mesh(sc, k)
    def cellset(ks):
        s = set()
        for k in ks:
            ni = sc['navinfo'].get(k)
            if ni is None: continue
            s.add(('x', ni[0], ni[1], ni[2]) if ni[0] else ('i', ni[3]))
        return s
    disp = cellset(displaced_meshes(sc)); doubt = cellset(doubtful_meshes(sc)); moved = set()
    cells = sc['cells']
    for ck, n in sc.get('movedfrom', {}).items():
        c = cells.get(ck)
        if c is None: continue
        if c['flags'] & 1 and not c['persist']: moved.add(('i', ck))
        elif c['grid'] is not None: moved.add(('x', c['world'], c['grid'][0], c['grid'][1]))
    out = {}
    for u in units:
        cs = [u] if u[0] == 'i' else [('x', u[1], u[2] + dx, u[3] + dy) for dx in range(-WIN, WIN + 1) for dy in range(-WIN, WIN + 1)]
        fl = set()
        if any(c in disp for c in cs): fl.add('displaced_mesh')
        if any(c in doubt for c in cs): fl.add('doubtful_mesh')
        if any(c in moved for c in cs): fl.add('plugin_moved_refs')
        out[u] = fl
    return out


def _graph_task(units):
    sc = _S['sc']; groups = _S['groups']; out = []
    pcache = {}
    def pm(k):
        if k not in pcache: pcache[k] = F.parse_np(sc['navm'][k][1])
        return pcache[k]
    for u in units:
        cs = [u] if u[0] == 'i' else [('x', u[1], u[2] + dx, u[3] + dy) for dx in range(-WIN, WIN + 1) for dy in range(-WIN, WIN + 1)]
        keys = [k for c in cs for k in groups.get(c, ())]
        meshes = [(k, pm(k)) for k in keys]; keyidx = {k: i for i, k in enumerate(keys)}
        g = AN.coarse_graph_features(meshes, keyidx, lambda k, raw: _resolver(sc, k)(raw))
        out.append((u, dict(gnodes=g['nodes'], loops=g['loops'], deadEnds=g['deadEnds'], chokes=g['chokes'], span=g['span'], pathy=g['pathy'])))
        if len(pcache) > 400: pcache.clear()
    return out


def graph_features(sc, groups, units, procs=6):
    _S['sc'] = sc; _S['groups'] = groups
    units = sorted(units); step = max(1, len(units) // (procs * 6)); res = {}
    with Pool(procs) as p:
        for chunk in p.imap_unordered(_graph_task, [units[i:i + step] for i in range(0, len(units), step)]):
            for u, g in chunk: res[u] = g
    return res
