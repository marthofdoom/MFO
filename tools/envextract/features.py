"""Navmesh raster features for the library selection (numpy; used only by `library` / `sample`)."""
import struct, math
import numpy as np
import navm as N

S = 64.0                 # raster cell, game units
LAYER_GAP = 120.0        # two walkable surfaces closer than this in z are one layer (a character is ~128 tall)
STEEP_NZ = 0.85          # normal.z below this = steeper than ~31.8 degrees
NARROW_LVL = 2           # erosion level <= 2 means local width under ~200 units
WIDE_LVL = 8             # erosion level >= 8 means local width over ~960 units
WATER_FLAG, DOOR_FLAG = 0x200, 0x400


def parse_np(nv):
    """Fast NVNM decode to numpy arrays (same layout as navm.parse; no range checks, call navm.parse for validation)."""
    ver, magic, world = struct.unpack_from('<III', nv, 0)
    if world: gy, gx = struct.unpack_from('<hh', nv, 12); cell = 0
    else: gy = gx = None; cell = struct.unpack_from('<I', nv, 12)[0]
    o = 16; nvert = struct.unpack_from('<I', nv, o)[0]; o += 4
    verts = np.frombuffer(nv, dtype='<f4', count=nvert * 3, offset=o).reshape(-1, 3); o += 12 * nvert
    ntri = struct.unpack_from('<I', nv, o)[0]; o += 4
    t = np.frombuffer(nv, dtype=np.dtype([('v', '<u2', 3), ('nb', '<i2', 3), ('fl', '<u2'), ('cv', '<u2')]), count=ntri, offset=o); o += 16 * ntri
    nl = struct.unpack_from('<I', nv, o)[0]; o += 4
    links = np.frombuffer(nv, dtype=np.dtype([('kind', '<u4'), ('mesh', '<u4'), ('tri', '<u2')]), count=nl, offset=o) if nl else None; o += 10 * nl
    nd = struct.unpack_from('<I', nv, o)[0]; o += 4
    doors = np.frombuffer(nv, dtype=np.dtype([('tri', '<u2'), ('crc', '<u4'), ('ref', '<u4')]), count=nd, offset=o) if nd else None
    return dict(world=world, cell=cell, gridX=gx, gridY=gy, verts=verts, tris=t, links=links, doors=doors, nlinks=nl, ndoors=nd)


class Raster:
    def __init__(self, ox, oy, w, h):
        self.ox, self.oy, self.w, self.h = ox, oy, w, h
        self.zmin = np.full((h, w), np.inf, np.float32); self.zmax = np.full((h, w), -np.inf, np.float32)
        self.steep = np.zeros((h, w), np.uint8); self.water = np.zeros((h, w), np.uint8)

    def add(self, verts, tris):
        v = verts; tv = tris['v']; fl = tris['fl']
        P = v[tv.astype(np.int64)]                       # (m,3,3)
        nrm = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
        ln = np.linalg.norm(nrm, axis=1); ln[ln == 0] = 1
        nz = np.abs(nrm[:, 2]) / ln
        area = 0.5 * np.linalg.norm(np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0]), axis=1)
        gx = (P[:, :, 0] - self.ox) / S; gy = (P[:, :, 1] - self.oy) / S
        x0 = np.floor(gx.min(1)).astype(int); x1 = np.floor(gx.max(1)).astype(int)
        y0 = np.floor(gy.min(1)).astype(int); y1 = np.floor(gy.max(1)).astype(int)
        zmin, zmax, steep, water = self.zmin, self.zmax, self.steep, self.water
        W, H = self.w, self.h
        for i in range(len(P)):
            ax, ay = gx[i, 0], gy[i, 0]; bx, by = gx[i, 1], gy[i, 1]; cx, cy = gx[i, 2], gy[i, 2]
            az, bz, cz = P[i, 0, 2], P[i, 1, 2], P[i, 2, 2]
            d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
            st = nz[i] < STEEP_NZ; wa = bool(fl[i] & WATER_FLAG)
            xa, xb, ya, yb = max(x0[i], 0), min(x1[i], W - 1), max(y0[i], 0), min(y1[i], H - 1)
            hit = False
            if abs(d) > 1e-9:
                for iy in range(ya, yb + 1):
                    py = iy + 0.5
                    for ix in range(xa, xb + 1):
                        px = ix + 0.5
                        l1 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / d
                        l2 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / d
                        l3 = 1 - l1 - l2
                        if l1 >= 0 and l2 >= 0 and l3 >= 0:
                            z = l1 * az + l2 * bz + l3 * cz; hit = True
                            if z < zmin[iy, ix]: zmin[iy, ix] = z
                            if z > zmax[iy, ix]: zmax[iy, ix] = z
                            if st: steep[iy, ix] = 1
                            if wa: water[iy, ix] = 1
            if not hit:   # sliver / tiny triangle: mark the cell of its centroid
                ix = int((ax + bx + cx) / 3); iy = int((ay + by + cy) / 3)
                if 0 <= ix < W and 0 <= iy < H:
                    z = (az + bz + cz) / 3
                    zmin[iy, ix] = min(zmin[iy, ix], z); zmax[iy, ix] = max(zmax[iy, ix], z)
                    if st: steep[iy, ix] = 1
                    if wa: water[iy, ix] = 1

    def finish(self):
        occ = self.zmax > -np.inf
        self.occ = occ
        lvl = np.zeros(occ.shape, np.uint8); cur = occ.copy()
        for L in range(1, 40):
            if not cur.any(): break
            lvl[cur] = L
            p = np.pad(cur, 1)
            nxt = cur.copy()
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    nxt &= p[1 + dy:1 + dy + cur.shape[0], 1 + dx:1 + dx + cur.shape[1]]
            cur = nxt
        self.lvl = lvl
        self.layered = occ & ((self.zmax - self.zmin) > LAYER_GAP)

    def window(self, x0, y0, x1, y1):
        """feature dict of the world rectangle [x0,x1) x [y0,y1)."""
        ia, ib = int((x0 - self.ox) // S), int(math.ceil((x1 - self.ox) / S)); ja, jb = int((y0 - self.oy) // S), int(math.ceil((y1 - self.oy) / S))
        ia, ja = max(ia, 0), max(ja, 0); ib, jb = min(ib, self.w), min(jb, self.h)
        if ib <= ia or jb <= ja: return None
        occ = self.occ[ja:jb, ia:ib]; n = int(occ.sum())
        if n == 0: return None
        lv = self.lvl[ja:jb, ia:ib][occ]
        zl = self.zmin[ja:jb, ia:ib][occ]
        return dict(cells=n, areaUnits2=n * S * S,
                    layer=float(self.layered[ja:jb, ia:ib].sum()) / n,
                    narrow=float((lv <= NARROW_LVL).sum()) / n, wide=float((lv >= WIDE_LVL).sum()) / n,
                    medWidth=float(np.median(lv)) * 2 * S - S,
                    steep=float(self.steep[ja:jb, ia:ib][occ].sum()) / n,
                    water=float(self.water[ja:jb, ia:ib][occ].sum()) / n,
                    zSpan=float(np.percentile(zl, 98) - np.percentile(zl, 2)),
                    extX=float(np.any(occ, axis=0).sum()) * S, extY=float(np.any(occ, axis=1).sum()) * S)
