"""Candidate environments, their features, classification and selection."""
import math, random, time
from multiprocessing import Pool
import numpy as np
import features as F, refs as R

WIN = 1                    # exterior window = (2*WIN+1)^2 grid cells around the centre cell
MAX_WATER = 0.5            # exterior windows with at least this share of water-flagged walkable area are not candidates
MIN_WINDOW_CELLS = 3000    # raster cells (64u) of walkable area a window needs to be a candidate (~12.3M units^2)
_S = {}


DISPLACED_GAP = 3000.0     # a mesh this far below its neighbours is a "removed" navmesh parked under the map (relative rule)


def displaced_meshes(sc):
    """Navmeshes a plugin 'removed' by moving them far below the rest of their cell.  Relative rule, no absolute height:
    an interior mesh whose highest vertex is more than DISPLACED_GAP under the lowest vertex of every other mesh of the cell
    (cell has >= 3 meshes), or an exterior mesh more than DISPLACED_GAP under the 5th percentile of the lowest vertices
    of the meshes in its 3x3 cells (>= 3 neighbours)."""
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
    out = set()
    for u, ks in grp.items():
        if u[0] == 'i':
            for k in ks:
                o = [info[j][0] for j in ks if j != k]
                if len(o) >= 2 and info[k][1] < min(o) - DISPLACED_GAP: out.add(k)
        else:
            nb = [j for dx in (-1, 0, 1) for dy in (-1, 0, 1) for j in grp.get(('x', u[1], u[2] + dx, u[3] + dy), ())]
            for k in ks:
                o = [info[j][0] for j in nb if j != k]
                if len(o) >= 3 and info[k][1] < float(np.percentile(o, 5)) - DISPLACED_GAP: out.add(k)
    sc['_displaced'] = out
    return out


def make_groups(sc):
    """unit -> [navmesh keys], without the displaced ones"""
    bad = displaced_meshes(sc); groups = {}
    for k in sc['navm']:
        ni = sc['navinfo'].get(k)
        if ni is None or (ni[3] is None and not ni[0]) or k in bad: continue
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


def _mesh_stat(m):
    P = m['verts'][m['tris']['v'].astype(np.int64)]
    area = 0.5 * np.linalg.norm(np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0]), axis=1).sum()
    return dict(tris=len(m['tris']), verts=len(m['verts']), area3d=float(area), doorTris=m['ndoors'], links=m['nlinks'])


def _add(a, b):
    for k, v in b.items(): a[k] = a.get(k, 0) + v


def _world_task(args):
    kind, ident = args; sc = _S['sc']; groups = _S['groups']
    out = []
    if kind == 'x':
        w = ident
        cells = [u for u in groups if u[0] == 'x' and u[1] == w]
        gxs = [u[2] for u in cells]; gys = [u[3] for u in cells]
        gx0, gy0 = min(gxs) - WIN, min(gys) - WIN
        W = (max(gxs) - min(gxs) + 1 + 2 * WIN) * 64; H = (max(gys) - min(gys) + 1 + 2 * WIN) * 64
        Rz = F.Raster(gx0 * 4096.0, gy0 * 4096.0, W, H); stats = {}
        for u in cells:
            st = {}
            for k in groups[u]:
                m = F.parse_np(sc['navm'][k][1]); Rz.add(m['verts'], m['tris']); _add(st, _mesh_stat(m))
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
            win.update(tot); out.append((('x', w, gx, gy), win))
    else:
        for ck in ident:
            u = ('i', ck); ms = [F.parse_np(sc['navm'][k][1]) for k in groups[u]]
            allv = np.concatenate([m['verts'] for m in ms])
            ox, oy = math.floor(allv[:, 0].min() / F.S) * F.S - F.S, math.floor(allv[:, 1].min() / F.S) * F.S - F.S
            W = int((allv[:, 0].max() - ox) / F.S) + 3; H = int((allv[:, 1].max() - oy) / F.S) + 3
            if W * H > 40_000_000: continue
            Rz = F.Raster(ox, oy, W, H); st = {}
            for m in ms: Rz.add(m['verts'], m['tris']); _add(st, _mesh_stat(m))
            Rz.finish(); win = Rz.window(ox, oy, ox + W * F.S, oy + H * F.S)
            if win is None: continue
            win.update(st); out.append((u, win))
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
    log('candidates: %d (%d interior) in %.0fs; displaced meshes dropped %d, interiors not reachable from an exterior cell dropped %d' % (
        len(cands), sum(1 for u in cands if u[0] == 'i'), time.time() - t0, stats['displaced'], stats['unreachable_interiors']))
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
    for f in cands.values(): f['nStuff'] = f['nCont'] + f['nFurn']
    return by
