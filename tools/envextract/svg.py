"""Top-down SVG dump of one .mfoenv (stdlib only).  Triangles shaded by height, flags coloured, refs as markers."""
import fmt

KIND_STYLE = {0: ('#d33', 'rect'), 1: ('#d9a400', 'rect'), 2: ('#09b', 'circle'), 3: ('#a5c', 'rect'), 4: ('#d9a400', 'dot'),
              5: ('#1a1', 'circle'), 6: ('#888', 'box'), 7: ('#6a6', 'dot'), 8: ('#585', 'box')}


def render(path, out, px=1200):
    e = fmt.read(path); V = e['verts']; T = e['tris']; M = e['meshes']
    xs = [v[0] for v in V]; ys = [v[1] for v in V]
    if not xs: raise SystemExit('no navmesh in ' + path)
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys); w = max(x1 - x0, 1.0); h = max(y1 - y0, 1.0)
    s = (px - 40) / max(w, h); W = w * s + 40; H = h * s + 40
    X = lambda x: 20 + (x - x0) * s; Y = lambda y: 20 + (y1 - y) * s       # north up
    zs = [v[2] for v in V]; z0, z1 = min(zs), max(zs); zr = max(z1 - z0, 1.0)
    p = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" style="background:#fff">' % (W, H, W, H)]
    tri = []
    for m in M:
        vb, tb, nt = m[5], m[7], m[8]
        for i in range(nt):
            t = T[tb + i]; a, b, c = V[vb + t[0]], V[vb + t[1]], V[vb + t[2]]
            tri.append(((a[2] + b[2] + c[2]) / 3, a, b, c, t[6]))
    tri.sort(key=lambda t: t[0])
    for z, a, b, c, fl in tri:
        g = int(235 - 140 * (z - z0) / zr)
        col = 'rgb(%d,%d,%d)' % (g, g, g)
        if fl & 0x200: col = 'rgb(%d,%d,255)' % (g // 2, g // 2 + 40)
        if fl & 0x40: col = 'rgb(%d,255,%d)' % (g // 2, g // 2)
        if fl & 0x400: col = '#f55'
        p.append('<polygon points="%.1f,%.1f %.1f,%.1f %.1f,%.1f" fill="%s" stroke="#fff" stroke-width="0.3"/>' % (X(a[0]), Y(a[1]), X(b[0]), Y(b[1]), X(c[0]), Y(c[1]), col))
    for l in e['links']:       # external edges
        m = M[l[0]]; t = T[m[7] + l[1]]; vb = m[5]; ed = l[2]
        a = V[vb + t[ed]]; b = V[vb + t[(ed + 1) % 3]]
        p.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#c0c" stroke-width="2"/>' % (X(a[0]), Y(a[1]), X(b[0]), Y(b[1])))
    for r in e['refs']:
        kind = r[3]; st = KIND_STYLE.get(kind, ('#000', 'dot')); col, shape = st; x, y = X(r[6]), Y(r[7])
        if shape == 'box' and r[16] > r[13]:
            if (r[16] - r[13]) > 0.4 * w or (r[17] - r[14]) > 0.4 * h: continue   # city-sized pieces would hide everything
            p.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s" fill-opacity="0.07" stroke="%s" stroke-width="0.5"/>' % (X(r[13]), Y(r[17]), (r[16] - r[13]) * s, (r[17] - r[14]) * s, col, col))
        elif shape == 'rect': p.append('<rect x="%.1f" y="%.1f" width="7" height="7" fill="%s"/>' % (x - 3.5, y - 3.5, col))
        elif shape == 'circle': p.append('<circle cx="%.1f" cy="%.1f" r="4.5" fill="%s" stroke="#000" stroke-width="0.8"/>' % (x, y, col))
        else: p.append('<circle cx="%.1f" cy="%.1f" r="2" fill="%s"/>' % (x, y, col))
    p.append('<text x="24" y="16" font-size="12" font-family="monospace">%s  %s  tris %d  refs %d  (grey=height, blue=water, green=preferred, red=door tri, magenta=external edge; red sq=door, yellow=container/item, green circle=actor)</text>' % (
        e['meta'].get('class', ''), e['meta'].get('name', ''), len(T), len(e['refs'])))
    p.append('</svg>')
    open(out, 'w').write('\n'.join(p))
