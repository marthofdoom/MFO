"""MFOENV v1 writer and reader (format documented in README.md; C++ reader in envfile.h)."""
import struct, json

MAGIC = b'MFOENV\0\0'; VERSION = 1
MESH = struct.Struct('<IIIhhIIIIIIII6f')
VERT = struct.Struct('<3f')
TRI = struct.Struct('<3H3hHH')
LINK = struct.Struct('<IHBBIiHH')
DOOR = struct.Struct('<IHHIIi')
REF = struct.Struct('<IIIBBH3f3ff3f3fIBBHI3f3fIIIIII')
PLUG = struct.Struct('<I')
for _s, _n in ((MESH, 72), (VERT, 12), (TRI, 16), (LINK, 20), (DOOR, 20), (REF, 128)): assert _s.size == _n, (_s.format, _s.size)
NOGRID = 0x7FFF


class Strings:
    def __init__(self): self.buf = bytearray(b'\0'); self.at = {'': 0}
    def add(self, s):
        if not s: return 0
        if s not in self.at: self.at[s] = len(self.buf); self.buf += s.encode('utf-8', 'replace') + b'\0'
        return self.at[s]


def write(path, meta, plugins, strings, meshes, verts, tris, links, doors, refs):
    """meshes/verts/tris/links/doors/refs: lists of tuples matching the structs; plugins: list of str offsets (u32)."""
    secs = [(b'META', json.dumps(meta, separators=(',', ':')).encode('utf-8'), 1, len(json.dumps(meta, separators=(',', ':')).encode('utf-8'))),
            (b'STRS', bytes(strings.buf), 1, len(strings.buf)),
            (b'PLUG', b''.join(PLUG.pack(o) for o in plugins), 4, len(plugins)),
            (b'MESH', b''.join(MESH.pack(*r) for r in meshes), 72, len(meshes)),
            (b'VERT', b''.join(VERT.pack(*r) for r in verts), 12, len(verts)),
            (b'TRIS', b''.join(TRI.pack(*r) for r in tris), 16, len(tris)),
            (b'LINK', b''.join(LINK.pack(*r) for r in links), 20, len(links)),
            (b'DOOR', b''.join(DOOR.pack(*r) for r in doors), 20, len(doors)),
            (b'REFS', b''.join(REF.pack(*r) for r in refs), 128, len(refs))]
    hdr = 16 + 16 * len(secs); off = (hdr + 3) & ~3; table = []; body = bytearray()
    for tag, data, rec, cnt in secs:
        table.append(struct.pack('<4sIII', tag, off + len(body), cnt, rec)); body += data; body += b'\0' * (-len(body) % 4)
    with open(path, 'wb') as f:
        f.write(MAGIC + struct.pack('<II', VERSION, len(secs)) + b''.join(table)); f.write(b'\0' * (off - hdr)); f.write(body)


def read(path):
    d = open(path, 'rb').read()
    if d[:8] != MAGIC: raise ValueError('bad magic')
    ver, n = struct.unpack_from('<II', d, 8)
    if ver != VERSION: raise ValueError('unsupported version %d' % ver)
    out = {'version': ver}; sec = {}
    for i in range(n):
        tag, off, cnt, rec = struct.unpack_from('<4sIII', d, 16 + 16 * i); sec[tag] = (off, cnt, rec)
    def rows(tag, st): off, cnt, rec = sec[tag]; return [st.unpack_from(d, off + rec * i) for i in range(cnt)]
    off, cnt, _ = sec[b'META']; out['meta'] = json.loads(d[off:off + cnt].decode('utf-8'))
    off, cnt, _ = sec[b'STRS']; strs = d[off:off + cnt]
    def s(o): return strs[o:strs.index(b'\0', o)].decode('utf-8', 'replace')
    out['str'] = s; out['plugins'] = [s(r[0]) for r in rows(b'PLUG', PLUG)]
    out['meshes'] = rows(b'MESH', MESH); out['verts'] = rows(b'VERT', VERT); out['tris'] = rows(b'TRIS', TRI)
    out['links'] = rows(b'LINK', LINK); out['doors'] = rows(b'DOOR', DOOR); out['refs'] = rows(b'REFS', REF)
    return out
