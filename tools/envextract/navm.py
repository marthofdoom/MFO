"""NVNM (NAVM navmesh data) decoder for Skyrim SE, NVNM version 12.

Layout (all little endian), verified against every NAVM in the vanilla masters (see validate.py):
  u32 version (12)             u32 magic (0xA5E9A03C, a CRC of the format, always this value)
  u32 worldspace formid        0 = interior mesh.
  interior: u32 parent cell formid          exterior: i16 gridY, i16 gridX  (cell grid, the worldspace's own)
  u32 nVert, nVert x { f32 x,y,z }
  u32 nTri,  nTri  x { u16 v0,v1,v2; i16 e01,e12,e20; u16 flags; u16 cover }          (16 bytes)
  u32 nLink, nLink x { u32 kind; u32 navmesh formid; u16 triangle }                   (10 bytes) edge links
  u32 nDoor, nDoor x { u16 triangle; u32 crc; u32 door ref formid }                   (10 bytes)
  u32 nCover, nCover x u16 triangle  (triangles that carry cover; the cover bits are in each triangle's `cover`)
  u32 gridDivisor; f32 maxX,maxY (cell size); f32 min x,y,z; f32 max x,y,z;
  gridDivisor^2 x { u32 n; n x u16 triangle }                                          (spatial index)
"""
import struct

MAGIC = 0xA5E9A03C
# triangle flags (documented community values; the ones this tool relies on are cross-checked in validate.py)
TF_EDGE01_LINK, TF_EDGE12_LINK, TF_EDGE20_LINK = 0x1, 0x2, 0x4
TF_DELETED, TF_NO_LARGE, TF_OVERLAPPING, TF_PREFERRED = 0x8, 0x10, 0x20, 0x40
TF_WATER, TF_DOOR, TF_FOUND = 0x200, 0x400, 0x800   # re-measured in validate.py, see README


class NavmeshError(Exception): pass


def parse(nv):
    """-> dict; raises NavmeshError unless the whole subrecord is consumed exactly and every index is in range."""
    n = len(nv); o = 0
    def need(k):
        if o + k > n: raise NavmeshError('truncated at %d need %d of %d' % (o, k, n))
    need(16); ver, magic, world = struct.unpack_from('<III', nv, 0); o = 12
    if ver != 12: raise NavmeshError('NVNM version %d' % ver)
    if magic != MAGIC: raise NavmeshError('magic %08X' % magic)
    r = dict(version=ver, world=world)
    if world:
        gy, gx = struct.unpack_from('<hh', nv, o); r['gridY'], r['gridX'] = gy, gx; r['cell'] = 0
    else:
        r['cell'] = struct.unpack_from('<I', nv, o)[0]; r['gridY'] = r['gridX'] = None
    o += 4
    need(4); nvert = struct.unpack_from('<I', nv, o)[0]; o += 4
    need(12 * nvert); r['verts'] = [struct.unpack_from('<fff', nv, o + 12 * i) for i in range(nvert)]; o += 12 * nvert
    need(4); ntri = struct.unpack_from('<I', nv, o)[0]; o += 4
    need(16 * ntri); r['tris'] = [struct.unpack_from('<HHHhhhHH', nv, o + 16 * i) for i in range(ntri)]; o += 16 * ntri
    need(4); nl = struct.unpack_from('<I', nv, o)[0]; o += 4
    need(10 * nl); r['links'] = [struct.unpack_from('<IIH', nv, o + 10 * i) for i in range(nl)]; o += 10 * nl
    need(4); nd = struct.unpack_from('<I', nv, o)[0]; o += 4
    need(10 * nd); r['doors'] = [struct.unpack_from('<HII', nv, o + 10 * i) for i in range(nd)]; o += 10 * nd
    need(4); nc = struct.unpack_from('<I', nv, o)[0]; o += 4
    need(2 * nc); r['cover'] = list(struct.unpack_from('<%dH' % nc, nv, o)); o += 2 * nc
    need(4 + 32); div = struct.unpack_from('<I', nv, o)[0]; o += 4
    r['gridDiv'] = div; r['gridBox'] = struct.unpack_from('<8f', nv, o); o += 32
    cells = []
    if div > 64: raise NavmeshError('grid divisor %d' % div)
    for _ in range(div * div):
        need(4); c = struct.unpack_from('<I', nv, o)[0]; o += 4
        need(2 * c); cells.append(struct.unpack_from('<%dH' % c, nv, o)); o += 2 * c
    r['grid'] = cells
    if o != n: raise NavmeshError('consumed %d of %d' % (o, n))
    for t in r['tris']:
        if max(t[0:3]) >= nvert: raise NavmeshError('vertex index out of range')
        for k, e in enumerate(t[3:6]):
            if t[6] & (1 << k):      # external edge: the value indexes the edge-link table
                if not 0 <= e < nl: raise NavmeshError('edge link index out of range')
            elif e >= ntri or e < -1: raise NavmeshError('neighbour index out of range')
    for t in r['cover']:
        if t >= ntri: raise NavmeshError('cover tri out of range')
    for t, _, _ in r['doors']:
        if t >= ntri: raise NavmeshError('door tri out of range')
    for t, _, _ in r['links']:
        pass
    for c in cells:
        for t in c:
            if t >= ntri: raise NavmeshError('grid tri out of range')
    return r
