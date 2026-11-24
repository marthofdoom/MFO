#!/usr/bin/env python3
"""Hand-made test environments for the IAUS simulator (sim v4 batch W). NOT game data: every fixture is drawn by hand below
its name in a .txt file of this folder and turned into a real MFOENV v2 file (tools/envextract/README.md) by this script,
so the simulator loads fixtures and extracted environments through the same reader.

    python3 tools/iaus_sim/envs/make_fixtures.py            # rewrite every <name>.mfoenv from <name>.txt
    python3 tools/iaus_sim/envs/make_fixtures.py --check    # exit 1 when a committed .mfoenv differs from its .txt

THE .txt FORMAT
    cell <units>            size of one map square (default 128)
    exterior                the environment is outside (default: interior)
    class <name>            environment class written to META (default fixture)
    map                     then one line per row, top row first. One character per square:
        '#' or ' '  nothing walkable          '.'  floor
        'P'  floor + a load door (an entry)   'F'  floor + an actor spawn point
        'D'  floor + a closed door            'L'  floor + a locked door
        '~'  floor under water                '='  floor on the preferred path
        'a'-'z' floor, named (for the lines below)
    end
    z X0 Y0 X1 Y1 H                  squares X0..X1, Y0..Y1 stand at height H (x from the left, y from the top, 0 based)
    ramp X0 Y0 X1 Y1 H0 H1 x|y       a slope over those squares, H0 at the low x (or y) side, H1 at the high side
    drop a b                         a one-way drop from square a down to square b (link kind 2, and kind 1 back)
    portal a b                       an enable / disable portal between a and b (link kind 3, closed)
    seam X                           squares with x >= X are a second navmesh, joined by kind 0 links
    nopool                           scenario only: never drawn for an ordinary fight of the built-in world
    defect a b                       TRUTH: no way from a to b. The mesh a planner reads still has it (a sim-only DFCT section)

Two squares share an edge when both of the edge's corners stand at the same height (within 8 units) in both. So a ramp joins
the floor at its foot and at its top, and a platform beside a lower floor is a ledge."""
import os, struct, sys, json
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'envextract'))
import fmt   # noqa: E402  (the extractor's own writer structs)


def parse(path):
    f = {'cell': 128.0, 'interior': True, 'class': 'fixture', 'rows': [], 'z': [], 'ramp': [], 'drop': [], 'portal': [], 'seam': None, 'defect': [], 'pool': True}
    inmap = False
    for raw in open(path):
        line = raw.rstrip('\n')
        if inmap:
            if line.strip() == 'end': inmap = False
            else: f['rows'].append(line)
            continue
        t = line.split('#', 1)[0].split()
        if not t: continue
        if t[0] == 'cell': f['cell'] = float(t[1])
        elif t[0] == 'exterior': f['interior'] = False
        elif t[0] == 'class': f['class'] = t[1]
        elif t[0] == 'map': inmap = True
        elif t[0] == 'z': f['z'].append([int(v) for v in t[1:5]] + [float(t[5])])
        elif t[0] == 'ramp': f['ramp'].append([int(v) for v in t[1:5]] + [float(t[5]), float(t[6]), t[7]])
        elif t[0] in ('drop', 'portal', 'defect'): f[t[0]].append((t[1], t[2]))
        elif t[0] == 'seam': f['seam'] = int(t[1])
        elif t[0] == 'nopool': f['pool'] = False
        else: raise SystemExit('%s: unknown line %r' % (path, line))
    return f


def build(name, f):
    rows = f['rows']; H = len(rows); W = max(len(r) for r in rows); C = f['cell']
    at = lambda x, y: rows[y][x] if 0 <= y < H and 0 <= x < len(rows[y]) else '#'
    floor = lambda x, y: at(x, y) not in '# '
    # corner heights per square: [nw, ne, se, sw] in MAP terms (y grows down the page). World y = (H - y) * C so the map reads as drawn.
    hz = {}
    for y in range(H):
        for x in range(W):
            if floor(x, y): hz[(x, y)] = [0.0, 0.0, 0.0, 0.0]
    for x0, y0, x1, y1, h in f['z']:
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                if (x, y) in hz: hz[(x, y)] = [h, h, h, h]
    for x0, y0, x1, y1, h0, h1, ax in f['ramp']:
        n = (x1 - x0 + 1) if ax == 'x' else (y1 - y0 + 1)
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                if (x, y) not in hz: continue
                i = (x - x0) if ax == 'x' else (y - y0); lo = h0 + (h1 - h0) * i / n; hi = h0 + (h1 - h0) * (i + 1) / n
                hz[(x, y)] = [lo, hi, hi, lo] if ax == 'x' else [lo, lo, hi, hi]
    mesh_of = lambda x, y: 1 if f['seam'] is not None and x >= f['seam'] else 0
    nmesh = 2 if f['seam'] is not None else 1
    # vertices: one per (mesh, grid point, height bucket)
    verts = [[] for _ in range(nmesh)]; vid = [{} for _ in range(nmesh)]
    def vertex(m, gx, gy, z):
        for (kx, ky, kz), i in vid[m].items():
            if kx == gx and ky == gy and abs(kz - z) <= 8.0: return i
        vid[m][(gx, gy, z)] = len(verts[m]); verts[m].append((gx * C, (H - gy) * C, z)); return len(verts[m]) - 1
    corner = lambda x, y: [(x, y), (x + 1, y), (x + 1, y + 1), (x, y + 1)]   # nw ne se sw grid points
    sq = {}    # (x, y) -> (mesh, [4 vertex ids])
    for (x, y), h in sorted(hz.items(), key=lambda k: (k[0][1], k[0][0])):
        m = mesh_of(x, y); sq[(x, y)] = (m, [vertex(m, gx, gy, h[i]) for i, (gx, gy) in enumerate(corner(x, y))])
    # triangles: square -> A = (nw, sw, se), B = (nw, se, ne)   (counter-clockwise in world space, where y is up the page)
    tris = [[] for _ in range(nmesh)]; tid = {}
    for (x, y), (m, v) in sq.items():
        tid[(x, y)] = len(tris[m]); nw, ne, se, sw = v
        tris[m].append({'v': [nw, sw, se], 'nb': [-1, -1, len(tris[m]) + 1], 'sq': (x, y)})   # edges: west, south, diagonal
        tris[m].append({'v': [nw, se, ne], 'nb': [len(tris[m]) - 1, -1, -1], 'sq': (x, y)})   # edges: diagonal, east, north
    # side -> (triangle offset, edge, the two corner slots of that side)
    SIDE = {'w': (0, 0, (0, 3)), 's': (0, 1, (3, 2)), 'e': (1, 1, (2, 1)), 'n': (1, 2, (1, 0))}
    OPP = {'w': 'e', 'e': 'w', 'n': 's', 's': 'n'}; STEP = {'w': (-1, 0), 'e': (1, 0), 'n': (0, -1), 's': (0, 1)}
    links = [[] for _ in range(nmesh)]
    def side_between(a, b):
        for s, (dx, dy) in STEP.items():
            if (a[0] + dx, a[1] + dy) == b: return s
        raise SystemExit('%s: squares %r and %r are not side by side' % (name, a, b))
    def same_edge(a, b, s):   # both corners of the shared side at the same height in both squares
        ha, hb = hz[a], hz[b]; ca = SIDE[s][2]; cb = SIDE[OPP[s]][2]
        return abs(ha[ca[0]] - hb[cb[1]]) <= 8.0 and abs(ha[ca[1]] - hb[cb[0]]) <= 8.0
    def link(a, s, b, kind):
        ma = mesh_of(*a); mb = mesh_of(*b); ta = tid[a] + SIDE[s][0]; ea = SIDE[s][1]; tb = tid[b] + SIDE[OPP[s]][0]
        ca = SIDE[s][2]; zmid = 0.5 * (hz[a][ca[0]] + hz[a][ca[1]]); zb = sum(hz[b]) / 4.0
        links[ma].append((ma, ta, ea, kind, 0x10 + mb, mb, tb, 0, zb - zmid)); tris[ma][ta]['ext'] = tris[ma][ta].get('ext', 0) | (1 << ea)
    for (x, y) in sq:
        for s, (dx, dy) in STEP.items():
            o = (x + dx, y + dy)
            if o not in sq or not same_edge((x, y), o, s): continue
            if mesh_of(x, y) == mesh_of(*o): tris[mesh_of(x, y)][tid[(x, y)] + SIDE[s][0]]['nb'][SIDE[s][1]] = tid[o] + SIDE[OPP[s]][0]
            else: link((x, y), s, o, 0)
    named = {}
    for y in range(H):
        for x in range(W):
            if at(x, y).islower(): named[at(x, y)] = (x, y)
    def cut(a, b):   # remove the walk edge a -> b if one was made (a drop or a portal replaces it)
        s = side_between(a, b); t = tris[mesh_of(*a)][tid[a] + SIDE[s][0]]; t['nb'][SIDE[s][1]] = -1
    for a, b in f['drop']:
        A, B = named[a], named[b]; s = side_between(A, B); cut(A, B); cut(B, A); link(A, s, B, 2); link(B, OPP[s], A, 1)
    for a, b in f['portal']:
        A, B = named[a], named[b]; s = side_between(A, B); cut(A, B); cut(B, A); link(A, s, B, 3); link(B, OPP[s], A, 3)
    dfct = []
    for a, b in f['defect']:
        A, B = named[a], named[b]; s = side_between(A, B); dfct.append((mesh_of(*A), tid[A] + SIDE[s][0], SIDE[s][1], 0))
    # refs and doors
    S = fmt.Strings(); plug = [S.add('fixture.esm')]; refs = []; doors = [[] for _ in range(nmesh)]
    def ref(kind, sig, x, y, flags=0, lock=0):
        h = sum(hz[(x, y)]) / 4.0; px = (x + 0.5) * C; py = (H - y - 0.5) * C
        refs.append((len(refs) + 1, 0x100 + kind, 1, kind, sig, flags, px, py, h, 0.0, 0.0, 0.0, 1.0, px - 32, py - 32, h, px + 32, py + 32, h + 128,
                     0, lock, 0, 0, (0x900 + len(refs)) if flags & 16 else 0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0, 0, 0, S.add('Fixture%d' % kind), 0, 0))
        return len(refs) - 1
    for (x, y) in sorted(sq, key=lambda k: (k[1], k[0])):
        ch = at(x, y); m = mesh_of(x, y); t = tris[m][tid[(x, y)]]; t2 = tris[m][tid[(x, y)] + 1]
        if ch in 'DLP':
            r = ref(0, 1, x, y, flags=(8 if ch == 'L' else 0) | (16 if ch == 'P' else 0), lock=(50 if ch == 'L' else 0))
            for k, tt in enumerate((t, t2)): tt['door'] = True; doors[m].append((m, tid[(x, y)] + k, 0, 0, refs[r][0], r))
        elif ch == 'F': ref(5, 15, x, y)
        if ch == '~': t['water'] = t2['water'] = True
        if ch == '=': t['pref'] = t2['pref'] = True
    meshes = []; allv = []; allt = []; alll = []; alld = []
    for m in range(nmesh):
        vs = verts[m]; mn = [min(v[i] for v in vs) for i in range(3)]; mx = [max(v[i] for v in vs) for i in range(3)]
        meshes.append((0x10 + m, 1, 0, fmt.NOGRID, fmt.NOGRID, len(allv), len(vs), len(allt), len(tris[m]), len(alll), len(links[m]), len(alld), len(doors[m]), *mn, *mx))
        allv += vs; alll += sorted(links[m], key=lambda l: (l[1], l[2])); alld += doors[m]
        for t in tris[m]:
            fl = t.get('ext', 0) | (0x400 if t.get('door') else 0) | (0x200 if t.get('water') else 0) | (0x40 if t.get('pref') else 0) | 0x800
            allt.append((*t['v'], *t['nb'], fl, 0))
    meta = {'name': name, 'set': 'Fixture', 'class': f['class'], 'interior': f['interior'], 'plugins': ['fixture.esm'], 'cells': [], 'pool': f['pool'], 'counts': {'tris': len(allt)}}
    return meta, plug, S, meshes, allv, allt, alll, alld, refs, dfct


def write(path, built):
    meta, plug, S, meshes, verts, tris, links, doors, refs, dfct = built
    if not dfct: fmt.write(path, meta, plug, S, meshes, verts, tris, links, doors, refs); return
    # the same file plus a DFCT section (u32 meshIdx; u16 tri; u8 edge; u8 truthKind 0 = wall). Readers that do not know the tag skip it.
    tmp = path + '.tmp'; fmt.write(tmp, meta, plug, S, meshes, verts, tris, links, doors, refs); d = open(tmp, 'rb').read(); os.remove(tmp)
    n = struct.unpack_from('<I', d, 12)[0]; old_hdr = 16 + 16 * n; old_off = (old_hdr + 3) & ~3; body = d[old_off:]
    new_hdr = 16 + 16 * (n + 1); new_off = (new_hdr + 3) & ~3; table = b''
    for i in range(n):
        tag, off, cnt, rec = struct.unpack_from('<4sIII', d, 16 + 16 * i); table += struct.pack('<4sIII', tag, off - old_off + new_off, cnt, rec)
    pad = b'\0' * (-len(body) % 4); table += struct.pack('<4sIII', b'DFCT', new_off + len(body) + len(pad), len(dfct), 8)
    with open(path, 'wb') as fh:
        fh.write(d[:12] + struct.pack('<I', n + 1) + table + b'\0' * (new_off - new_hdr) + body + pad + b''.join(struct.pack('<IHBB', *r) for r in dfct))


def main():
    check = '--check' in sys.argv; bad = 0
    for fn in sorted(os.listdir(HERE)):
        if not fn.endswith('.txt'): continue
        name = fn[:-4]; out = os.path.join(HERE, name + '.mfoenv'); built = build(name, parse(os.path.join(HERE, fn)))
        if check:
            tmp = out + '.chk'; write(tmp, built); same = os.path.exists(out) and open(tmp, 'rb').read() == open(out, 'rb').read(); os.remove(tmp)
            if not same: print('STALE', name); bad = 1
        else: write(out, built); print('%-22s %4d tris %3d refs %d defects' % (name, len(built[5]), len(built[8]), len(built[9])))
    sys.exit(bad)


if __name__ == '__main__': main()
