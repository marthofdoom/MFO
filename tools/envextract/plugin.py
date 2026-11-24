"""Minimal read-only Skyrim SE plugin walker (own code, no third-party parser).

Only what envextract needs: header, masters, GRUP tree with the cell/world context, record subrecords.
Record header 24 bytes: sig4 size4 flags4 formid4 vc4 ver2 unk2.  GRUP header 24 bytes: 'GRUP' size4(incl header)
label4 type4(i32) stamp4 unk4.  Group types: 0 top, 1 world children, 2 interior block, 3 interior sub-block,
4 exterior block, 5 exterior sub-block, 6 cell children, 7 topic children, 8 cell persistent children,
9 cell temporary children, 10 cell visible-distant children.
"""
import struct, zlib, mmap

F_ESM, F_LOCAL, F_ESL, F_COMPRESSED, F_DELETED, F_DISABLED, F_PERSISTENT = 0x1, 0x80, 0x200, 0x40000, 0x20, 0x800, 0x400
_u32 = struct.Struct('<I').unpack_from
_hdr = struct.Struct('<4sIIIIHH').unpack_from


def subrecords(data, o, size, flags):
    """list of (sig bytes, payload bytes) of the record body at data[o:o+size] (handles zlib and XXXX)."""
    body = data[o:o + size]
    if flags & F_COMPRESSED:
        body = zlib.decompress(body[4:])
    out = []; p = 0; n = len(body); big = None
    while p + 6 <= n:
        sig = body[p:p + 4]; sz = struct.unpack_from('<H', body, p + 4)[0]; p += 6
        if sig == b'XXXX':
            big = struct.unpack_from('<I', body, p)[0]; p += sz; continue
        if big is not None:
            sz = big; big = None
        out.append((bytes(sig), body[p:p + sz])); p += sz
    return out


class Plugin:
    def __init__(self, path):
        self.path = path
        f = open(path, 'rb'); self.data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ); f.close()
        d = self.data
        sig, size, flags, fid, _, _, _ = _hdr(d, 0)
        if sig != b'TES4': raise ValueError('not a plugin: ' + path)
        self.flags = flags; self.light = bool(flags & F_ESL) or path.lower().endswith('.esl')
        self.masters = []
        for s, p in subrecords(d, 24, size, flags):
            if s == b'MAST': self.masters.append(p.split(b'\0')[0].decode('latin-1'))
        self.body = 24 + size

    def resolve(self, raw, own):
        """raw FormID as stored in this plugin -> (owner plugin name lowercase, local id).  own = this plugin's name."""
        mi = raw >> 24
        if mi < len(self.masters):
            return (self.masters[mi].lower(), raw & 0xFFFFFF)
        return (own.lower(), raw & (0xFFF if self.light else 0xFFFFFF))

    def top_groups(self):
        """yield (label bytes, start, end) of every top-level GRUP."""
        d = self.data; o = self.body; n = len(d)
        while o + 24 <= n:
            sig, size = d[o:o + 4], _u32(d, o + 4)[0]
            if sig != b'GRUP': o += 24 + size; continue
            yield bytes(d[o + 8:o + 12]), o, o + size
            o += size

    def walk(self, start, end, ctx=None):
        """depth-first over a GRUP body.  yields ('rec', sig, flags, fid, off, size, ctx) per record and nothing for
        group headers; ctx is a dict {world, cell, gtype} (raw FormIDs of the enclosing WRLD / CELL and the innermost
        cell-children group type 8/9/10 or None)."""
        d = self.data
        o = start + 24
        stack = [(o, end, dict(world=0, cell=0, ctype=None, block=None))]
        while stack:
            o, e, c = stack.pop()
            while o + 24 <= e:
                sig = d[o:o + 4]; size = _u32(d, o + 4)[0]
                if sig == b'GRUP':
                    gt = struct.unpack_from('<i', d, o + 12)[0]; lab = _u32(d, o + 8)[0]; c2 = dict(c)
                    if gt == 1: c2['world'] = lab
                    elif gt == 6: c2['cell'] = lab; c2['ctype'] = None
                    elif gt in (8, 9, 10): c2['ctype'] = gt
                    elif gt == 4: c2['block'] = ('x', struct.unpack_from('<hh', d, o + 8))
                    if size < 24: break
                    stack.append((o + size, e, c)); stack.append((o + 24, o + size, c2)); break
                _, size, fl, fid, _, _, _ = _hdr(d, o)
                yield sig, fl, fid, o + 24, size, c
                o += 24 + size
