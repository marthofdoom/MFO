"""Builders for synthetic plugin bytes and NVNM blobs (no game data)."""
import struct, zlib

def sub(sig, data): return sig + struct.pack('<H', len(data)) + data
def rec(sig, fid, subs, flags=0, compress=False):
    body = b''.join(sub(s, d) for s, d in subs)
    if compress: body = struct.pack('<I', len(body)) + zlib.compress(body); flags |= 0x40000
    return sig + struct.pack('<IIIIHH', len(body), flags, fid, 0, 44, 0) + body
def group(label, gtype, content):
    lab = label if isinstance(label, bytes) else struct.pack('<I', label)
    return b'GRUP' + struct.pack('<I', 24 + len(content)) + lab + struct.pack('<iIHH', gtype, 0, 0, 0) + content
def plugin(masters, records_bytes, flags=0):
    subs = [(b'HEDR', struct.pack('<fII', 1.7, 0, 0x800))] + [(b'MAST', m.encode() + b'\0') for m in masters]
    return rec(b'TES4', 0, subs, flags) + records_bytes

def nvnm(verts, tris, links=(), doors=(), cover=(), world=0, cell=0, grid=(0, 0), div=1, flagsfix=True):
    """tris: (v0,v1,v2,e01,e12,e20,flags,cover); links: (kind, navmesh, tri); doors: (tri, crc, ref)"""
    b = struct.pack('<III', 12, 0xA5E9A03C, world)
    b += struct.pack('<hh', grid[1], grid[0]) if world else struct.pack('<I', cell)
    b += struct.pack('<I', len(verts)) + b''.join(struct.pack('<fff', *v) for v in verts)
    b += struct.pack('<I', len(tris)) + b''.join(struct.pack('<HHHhhhHH', *t) for t in tris)
    b += struct.pack('<I', len(links)) + b''.join(struct.pack('<IIH', *l) for l in links)
    b += struct.pack('<I', len(doors)) + b''.join(struct.pack('<HII', *d) for d in doors)
    b += struct.pack('<I', len(cover)) + b''.join(struct.pack('<H', c) for c in cover)
    b += struct.pack('<I', div) + struct.pack('<8f', 4096, 4096, 0, 0, 0, 4096, 4096, 100)
    b += b''.join(struct.pack('<I', len(tris)) + b''.join(struct.pack('<H', i) for i in range(len(tris))) for _ in range(div * div))
    return b
