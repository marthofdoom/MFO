"""Per-mesh and per-place anomaly counts and coverage features read off the navmesh (numpy for the arrays, python for the graph
walks).  Used by library.py; nothing here writes files."""
import math
import numpy as np

STEEP_DEG = 25.0           # triangles steeper than this form "steep runs" (stairs, ramps, slopes)
SLOPE_BANDS = (15.0, 30.0, 45.0)
DROP_BANDS = (100.0, 300.0)   # kind 2 drop heights: low < 100 <= mid < 300 <= high
ISLAND_MIN_TRIS = 30
COARSE = 256.0             # coarse graph bin, units
LAYER_Z = 200.0            # coarse nodes of one bin are separated into floors by this much z


def _geo_eq(a, b): return abs(a[0] - b[0]) <= 0.51 and abs(a[1] - b[1]) <= 0.51 and abs(a[2] - b[2]) <= 0.51


def edge_scan(m):
    """Internal neighbour edges of one mesh that are not a clean pair.  -> (oneway, notswapped, internal): the first two are lists of
    (triangle, edge), `internal` counts every internal (not external, not open) edge.  oneway = the neighbour has no edge back; notswapped =
    the neighbour points back but its edge is not the same two points in the opposite direction.  Cached in m['_es']."""
    es = m.get('_es')
    if es is not None: return es
    T = m['tris']; V = m['verts']; n = len(T)
    nb = T['nb'].tolist(); fl = T['fl'].tolist(); vv = T['v'].tolist(); Vl = V.tolist()
    oneway = []; notswapped = []; internal = 0
    for t in range(n):
        f = fl[t]
        for e in range(3):
            if f & (1 << e): continue
            u = nb[t][e]
            if u < 0: continue
            internal += 1
            back = [j for j in range(3) if nb[u][j] == t and not (fl[u] & (1 << j))]
            if not back: oneway.append((t, e)); continue
            a = Vl[vv[t][e]]; b = Vl[vv[t][(e + 1) % 3]]
            if not any(_geo_eq(a, Vl[vv[u][(j + 1) % 3]]) and _geo_eq(b, Vl[vv[u][j]]) for j in back): notswapped.append((t, e))
    m['_es'] = es = (oneway, notswapped, internal)
    return es


def clean_nb(m):
    """neighbour table (n, 3) with the bad internal edges of edge_scan cut (-1).  The export cuts the same edges and marks them (TRIS flags 0x1000 << edge)."""
    c = m.get('_nb')
    if c is not None: return c
    nb = m['tris']['nb'].astype(np.int64).copy(); ow, ns, _ = edge_scan(m)
    for t, e in ow + ns: nb[t, e] = -1
    m['_nb'] = nb
    return nb


def analyze_mesh(m, resolve_target, target_mesh):
    """m: features.parse_np result.  resolve_target(raw) -> key (links), target_mesh(key) -> parse_np result or None.
    -> dict of counts / areas for this mesh."""
    T = m['tris']; V = m['verts']; n = len(T)
    nb = clean_nb(m).tolist(); fl = T['fl'].tolist(); vv = T['v'].tolist(); cv = T['cv']
    Vl = V.tolist()
    ow, ns, internal = edge_scan(m); oneway = len(ow); notswapped = len(ns)
    P = V[T['v'].astype(np.int64)]
    nrm = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0]); ln = np.linalg.norm(nrm, axis=1)
    area = 0.5 * ln; ln[ln == 0] = 1.0
    deg = np.degrees(np.arccos(np.clip(np.abs(nrm[:, 2]) / ln, 0, 1)))
    bands = [float(area[deg < SLOPE_BANDS[0]].sum()), float(area[(deg >= SLOPE_BANDS[0]) & (deg < SLOPE_BANDS[1])].sum()),
             float(area[(deg >= SLOPE_BANDS[1]) & (deg < SLOPE_BANDS[2])].sum()), float(area[deg >= SLOPE_BANDS[2]].sum())]
    # steep runs: connected steep triangles, rise = z span of their centroids
    cz = P[:, :, 2].mean(1); steep = (deg >= STEEP_DEG)
    par = list(range(n))
    def find(x):
        while par[x] != x: par[x] = par[par[x]]; x = par[x]
        return x
    sl = steep.tolist()
    for t in range(n):
        if not sl[t]: continue
        for e in range(3):
            u = nb[t][e]
            if u >= 0 and not fl[t] & (1 << e) and sl[u]:
                a, b = find(t), find(u)
                if a != b: par[a] = b
    lo = {}; hi = {}
    for t in range(n):
        if sl[t]:
            r = find(t); z = float(cz[t]); lo[r] = min(lo.get(r, z), z); hi[r] = max(hi.get(r, z), z)
    rise = max((hi[r] - lo[r] for r in lo), default=0.0)
    out = dict(tris=n, edges=internal, oneway=oneway, notswapped=notswapped, a0=bands[0], a1=bands[1], a2=bands[2], a3=bands[3], steepRise=rise,
               coverTris=int((cv != 0).sum()), doorTris=m['ndoors'], ownerless=0, stale=0, kind3=0, dropLow=0, dropMid=0, dropHigh=0, links=m['nlinks'],
               kind0=0, kind12=0)
    if m['links'] is not None:
        owner = {}
        for t in range(n):
            for e in range(3):
                if fl[t] & (1 << e): owner[nb[t][e]] = (t, e)
        for li, (kind, tm, tt) in enumerate(m['links'].tolist()):
            if kind == 3: out['kind3'] += 1
            elif kind == 0: out['kind0'] += 1
            else: out['kind12'] += 1
            if li not in owner: out['ownerless'] += 1; continue
            B = target_mesh(resolve_target(tm))
            if B is None: out['stale'] += 1; continue          # the target mesh does not exist
            if tt >= len(B['tris']): out['stale'] += 1; continue
            if kind == 2:
                t, e = owner[li]; a = Vl[vv[t][e]]; b = Vl[vv[t][(e + 1) % 3]]
                c = B['verts'][B['tris'][tt]['v'].astype(np.int64)]
                dz = float(c[:, 2].mean() - (a[2] + b[2]) / 2); h = -dz
                if h < DROP_BANDS[0]: out['dropLow'] += 1
                elif h < DROP_BANDS[1]: out['dropMid'] += 1
                else: out['dropHigh'] += 1
    return out


def place_components(meshes, keyidx, resolve_target):
    """meshes: [(key, parse_np result)] of one place.  Union-find over triangles (internal neighbours + kind 0 links to meshes of the
    place).  -> (main share of walkable area, number of islands with >= ISLAND_MIN_TRIS triangles, comp id per global tri, tri offset per mesh)"""
    off = []; n = 0
    for k, m in meshes: off.append(n); n += len(m['tris'])
    par = list(range(n))
    def find(x):
        while par[x] != x: par[x] = par[par[x]]; x = par[x]
        return x
    area = np.zeros(n)
    for (k, m), o in zip(meshes, off):
        T = m['tris']; fl = T['fl'].tolist(); nb = clean_nb(m).tolist()
        P = m['verts'][T['v'].astype(np.int64)]
        area[o:o + len(T)] = 0.5 * np.linalg.norm(np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0]), axis=1)
        for t in range(len(T)):
            for e in range(3):
                u = nb[t][e]
                if u >= 0 and not fl[t] & (1 << e):
                    a, b = find(o + t), find(o + u)
                    if a != b: par[a] = b
        if m['links'] is not None:
            owner = {}
            for t in range(len(T)):
                for e in range(3):
                    if fl[t] & (1 << e): owner[nb[t][e]] = (t, e)
            for li, (kind, tm, tt) in enumerate(m['links'].tolist()):
                if kind != 0 or li not in owner: continue
                j = keyidx.get(resolve_target(k, tm))
                if j is None or tt >= len(meshes[j][1]['tris']): continue
                a, b = find(o + owner[li][0]), find(off[j] + tt)
                if a != b: par[a] = b
    ca = {}; cn = {}
    for g in range(n):
        r = find(g); ca[r] = ca.get(r, 0.0) + area[g]; cn[r] = cn.get(r, 0) + 1
    tot = sum(ca.values()) or 1.0; big = max(ca, key=ca.get) if ca else None
    islands = sum(1 for r in cn if r != big and cn[r] >= ISLAND_MIN_TRIS)
    return (ca[big] / tot if ca else 0.0), islands, [find(g) for g in range(n)], off


LATTICE = COARSE / 2       # sampling step inside a triangle that is bigger than a coarse bin


def coarse_graph_features(meshes, keyidx, resolve_target):
    """Coarse graph of a place: nodes = (256 unit bin, floor) cells that a walkable TRIANGLE touches (rasterised, not the centroid: a big
    flat triangle covers every bin under it); edges = lattice neighbours inside one triangle, and triangle adjacency (internal, kind 0/1/2
    links inside the place).  -> dict: nodes, loops (cyclomatic number of the main component), deadEnds (degree 1 nodes), chokes
    (articulation points), span (longest shortest path found by a double sweep, units), pathy (share of nodes with degree <= 2)."""
    node = {}; adj = []
    def nid(x, y, z):
        key = (int(math.floor(x / COARSE)), int(math.floor(y / COARSE)), int(math.floor(z / LAYER_Z)))
        i = node.get(key)
        if i is None: i = node[key] = len(adj); adj.append(set())
        return i
    ids = []          # per mesh: node id of each triangle's centroid (the triangle's anchor)
    for k, m in meshes:
        T = m['tris']; P = m['verts'][T['v'].astype(np.int64)].astype(np.float64); c = P.mean(1)
        span = np.maximum(P[:, :, 0].max(1) - P[:, :, 0].min(1), P[:, :, 1].max(1) - P[:, :, 1].min(1))
        I = [nid(*row) for row in c.tolist()]
        for t in np.nonzero(span > LATTICE)[0].tolist():       # a big triangle: sample it on a lattice, connect lattice neighbours
            a, b, cc = P[t]; kk = min(120, 3 * int(math.ceil(float(span[t]) / (3 * LATTICE))))     # multiple of 3: the centroid is a lattice point
            ii, jj = np.meshgrid(np.arange(kk + 1), np.arange(kk + 1), indexing='ij'); ok = (ii + jj) <= kk
            w1 = ii / kk; w2 = jj / kk; w0 = 1.0 - w1 - w2
            pts = w0[..., None] * a + w1[..., None] * b + w2[..., None] * cc
            grid = -np.ones((kk + 1, kk + 1), dtype=np.int64)
            for x, y in zip(*np.nonzero(ok)): grid[x, y] = nid(*pts[x, y].tolist())
            for da, db in ((1, 0), (0, 1), (1, -1)):
                for x, y in zip(*np.nonzero(ok)):
                    x2, y2 = x + da, y + db
                    if 0 <= x2 <= kk and 0 <= y2 <= kk and ok[x2, y2]:
                        u, v = int(grid[x, y]), int(grid[x2, y2])
                        if u != v: adj[u].add(v); adj[v].add(u)
        ids.append(I)
    for mi, (k, m) in enumerate(meshes):
        T = m['tris']; fl = T['fl'].tolist(); nb = clean_nb(m).tolist(); I = ids[mi]
        for t in range(len(T)):
            for e in range(3):
                u = nb[t][e]
                if u >= 0 and not fl[t] & (1 << e):
                    a, b = I[t], I[u]
                    if a != b: adj[a].add(b); adj[b].add(a)
        if m['links'] is not None:
            owner = {}
            for t in range(len(T)):
                for e in range(3):
                    if fl[t] & (1 << e): owner[nb[t][e]] = (t, e)
            for li, (kind, tm, tt) in enumerate(m['links'].tolist()):
                if kind == 3 or li not in owner: continue
                j = keyidx.get(resolve_target(k, tm))
                if j is None or tt >= len(ids[j]): continue
                a, b = I[owner[li][0]], ids[j][tt]
                if a != b: adj[a].add(b); adj[b].add(a)
    N = len(adj)
    if N == 0: return dict(nodes=0, loops=0, deadEnds=0, chokes=0, span=0.0, pathy=0.0)
    comp = [-1] * N; sizes = []
    for s in range(N):
        if comp[s] >= 0: continue
        cid = len(sizes); comp[s] = cid; st = [s]; cnt = 0
        while st:
            x = st.pop(); cnt += 1
            for y in adj[x]:
                if comp[y] < 0: comp[y] = cid; st.append(y)
        sizes.append(cnt)
    main = max(range(len(sizes)), key=sizes.__getitem__)
    nodes = [i for i in range(N) if comp[i] == main]
    E = sum(len(adj[i]) for i in nodes) // 2
    loops = E - len(nodes) + 1
    dead = sum(1 for i in nodes if len(adj[i]) == 1)
    pathy = sum(1 for i in nodes if len(adj[i]) <= 2) / len(nodes)
    def bfs(s):
        d = {s: 0}; q = [s]; h = 0
        while h < len(q):
            x = q[h]; h += 1
            for y in adj[x]:
                if y not in d: d[y] = d[x] + 1; q.append(y)
        far = max(d, key=d.get); return far, d[far]
    a, _ = bfs(nodes[0]); b, hops = bfs(a)
    # articulation points (iterative Tarjan) on the main component
    disc = {nodes[0]: 0}; low = {nodes[0]: 0}; t = 1; root = nodes[0]; rootkids = 0; artset = set()
    st = [(root, -1, iter(adj[root]))]
    while st:
        x, p, it = st[-1]; adv = False
        for y in it:
            if y == p: continue
            if y in disc: low[x] = min(low[x], disc[y])
            else:
                disc[y] = low[y] = t; t += 1; st.append((y, x, iter(adj[y]))); adv = True
                if x == root: rootkids += 1
                break
        if adv: continue
        st.pop()
        if st:
            px = st[-1][0]; low[px] = min(low[px], low[x])
            if px != root and low[x] >= disc[px]: artset.add(px)
    art = len(artset) + (1 if rootkids > 1 else 0)
    return dict(nodes=len(nodes), loops=int(loops), deadEnds=int(dead), chokes=int(art), span=float(hops * COARSE), pathy=float(pathy))
