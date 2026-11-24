"""Candidate environments, their features, classification and selection."""
import math, random, time
from multiprocessing import Pool
import numpy as np
import features as F, refs as R

WIN = 1                    # exterior window = (2*WIN+1)^2 grid cells around the centre cell
MIN_WINDOW_CELLS = 3000    # raster cells (64u) of walkable area a window needs to be a candidate (~12.3M units^2)
_S = {}


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
    groups = {}
    for k in sc['navm']:
        if sc['navinfo'].get(k) is None or sc['navinfo'][k][3] is None and not sc['navinfo'][k][0]: continue
        groups.setdefault(unit_of_mesh(sc, k), []).append(k)
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
    log('candidates: %d (%d interior) in %.0fs' % (len(cands), sum(1 for u in cands if u[0] == 'i'), time.time() - t0))
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
                f[names[kind]] += 1
                ref = sc['refs'][rk]
                if kind == R.K_ACTOR and sig == 'LVLN': f['nLeveled'] += 1
                if kind == R.K_DOOR:
                    if ref[8]: f['nTele'] += 1
                    if ref[9]: f['nLocked'] += 1
    for f in cands.values(): f['nStuff'] = f['nCont'] + f['nFurn']
    return by
