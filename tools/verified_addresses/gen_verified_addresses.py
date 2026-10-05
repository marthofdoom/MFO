#!/usr/bin/env python3
"""Generate native/VerifiedAddresses.h and Docs/VERIFIED-ADDRESSES.md from tools/verified_addresses/spec.json.

mit-3.7 F1 (2026-09-24). This is the seat-verification audit made reproducible. Every row of the
spec is a hook-critical or seat-critical Address Library id this DLL uses. For each exact runtime
the script DERIVES the expected RVA from our own copy of that game executable, without asking the
Address Library:

  vtable    - RTTI walk: the TypeDescriptor named `rtti` -> the CompleteObjectLocator(s) that point
              at it with the row's `col_offset` -> the vtable that points at that locator. Exactly
              one vtable must match.
  rtti      - the TypeDescriptor whose name string is `rtti` (TD = name - 0x10). Exactly one.
  function  - the byte signature `sig[runtime]` (hex, ?? = wildcard) must match exactly ONE place in
              .text. The signatures were cut from our disassembly at the RVAs our address-table pass
              confirmed (Docs/ADDRESS-TABLE-2026-09-15.md), with every rel32 branch target and every
              RIP-relative displacement wildcarded (see --make-sig).
  callsite  - a function row plus `offset[runtime]`: the bytes there must be `bytes` (E8 for a call),
              and when `target` is given the rel32 must land on that function id's derived RVA.

Only after the RVA is derived does the script look the id up in the Address Library file for that
runtime, and it FAILS (exit 1, nothing written) if the library disagrees. So the generated table is
"our disassembly and the library agree, here is the value", never "the library said so".
The Address Library files are read for the id key only. Nothing from them is redistributed beyond
the (id, RVA) pairs our own derivation reproduced.

Also recorded per vtable row (documentation, not runtime-checked): the function each hooked slot
holds on each runtime. Runtime slot contents are NOT checked: another DLL may hook a slot first,
and that must not refuse our seat.

1.7.104 (added 2026-10-04): there is NO Address Library for 1.7.104. Its column is resolved from OUR
own id map (--idmap, _research/1.7.104-idmap/idmap-1.7.104.csv: key = the 1.6.1170/AE id, value = the
1.7.104 RVA, each with its own evidence) and then VERIFIED against the plaintext 1.7.104 executable by
the same row-kind checks, instead of a library comparison:
  vtable    - the vtable is identified by the NAME of the base subobject it belongs to (the first
              BaseClassArray entry whose displacement equals the COL offset, read from the 1.6.1170 image),
              because 1.7.104 adds bases to some classes and shifts COL offsets. Exactly one vtable of
              that RTTI class must carry that base, and it must be the idmap's RVA.
  rtti      - exactly one TypeDescriptor, equal to the idmap's RVA.
  function  - the 1.6.1170 signature (or an explicit sig["1.7.104"]) must match exactly one place in
              .text and it must be the idmap's RVA.
  callsite  - as function, then `bytes` at `offset` (offset["1.7.104"] if given) and the call target
              (target["1.6.1170"] id looked up in the idmap) as for the other runtimes.
  ripref    - as function; the use-site bytes must equal the 1.6.1170 bytes with the RIP displacement
              wildcarded, and the displacement's target must equal the idmap's RVA for the global.
A row whose id is unresolved, whose idmap row disagrees with our 1.6.1170 derivation, or that fails any
check above gets NO 1.7.104 entry: the header omits it (listed in a comment), so SelfCheck::IsVerifiedAddress
is false for it and the seat is refused. A 1.7.104 failure never fails the generation; a 1.6.1170 or
1.5.97 failure still does.

Usage:
  gen_verified_addresses.py --spec tools/verified_addresses/spec.json \
      --bin-1.6.1170 <SkyrimSE.unpacked.exe> --al-1.6.1170 <versionlib-1-6-1170-0.bin> \
      --bin-1.5.97   <SkyrimSE.unpacked.exe> --al-1.5.97   <version-1-5-97-0.bin> \
      --bin-1.7.104  <binaries/1.7.104/SkyrimSE.exe>       --idmap <idmap-1.7.104.csv> \
      --header native/VerifiedAddresses.h --doc Docs/VERIFIED-ADDRESSES.md
  gen_verified_addresses.py --make-sig <1.6.1170|1.5.97|1.7.104> <rva> [same --bin/--al/--idmap args]

Needs python3 + capstone. The executables are the SteamStub-unpacked ones (binaries/README.md);
the 1.6.1170 library MUST be versionlib-1-6-1170-0.bin (id 38048 -> 0x6A35F0 is asserted).
KEEP THIS FILE IDENTICAL in MFO and APMF (tools/verified_addresses/).
"""
import argparse
import csv
import hashlib
import json
import re
import struct
import sys

import capstone

BASE = 0x140000000
RUNTIMES = {
    '1.6.1170': {'version': (1, 6, 1170, 0), 'sanity': (38048, 0x6A35F0), 'fmt': 2},
    '1.5.97': {'version': (1, 5, 97, 0), 'sanity': (37020, 0x60F240), 'fmt': 1},
    # no Address Library: resolved from our own idmap (see the docstring), keyed by the 1.6.1170 id
    '1.7.104': {'version': (1, 7, 104, 0), 'idmap': True, 'md5': '113faeb71fd8f62b26d0c8627299ab40'},
}
AL_RUNTIMES = [rt for rt, m in RUNTIMES.items() if not m.get('idmap')]
IDMAP_RT = '1.7.104'
AE_RT = '1.6.1170'   # the idmap is keyed by this runtime's ids


# ---------------------------------------------------------------- PE image
class Image:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.d = d
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        opt = struct.unpack_from('<H', d, pe + 20)[0]
        sec = pe + 24 + opt
        self.secs = []
        for i in range(nsec):
            name = d[sec + i * 40:sec + i * 40 + 8].rstrip(b'\0').decode(errors='replace')
            vs, va, rs, ro = struct.unpack_from('<IIII', d, sec + i * 40 + 8)
            self.secs.append((name, va, vs, ro, rs))
        self.text = next(s for s in self.secs if s[0] == '.text')

    def off(self, rva):
        for n, va, vs, ro, rs in self.secs:
            if va <= rva < va + rs:
                return ro + rva - va
        raise ValueError('rva 0x%X not in a file-backed section' % rva)

    def rva(self, off):
        for n, va, vs, ro, rs in self.secs:
            if ro <= off < ro + rs:
                return va + off - ro, n
        return None, None

    def r(self, rva, n):
        o = self.off(rva)
        return self.d[o:o + n]

    def u32(self, rva):
        return struct.unpack('<I', self.r(rva, 4))[0]

    def u64(self, rva):
        return struct.unpack('<Q', self.r(rva, 8))[0]

    def text_bytes(self):
        n, va, vs, ro, rs = self.text
        return va, self.d[ro:ro + rs]


# ---------------------------------------------------------------- Address Library (formats 1 and 2)
def load_addrlib(path, want_fmt, want_version):
    d = open(path, 'rb').read()
    off = 0

    def rd(f):
        nonlocal off
        v = struct.unpack_from(f, d, off)
        off += struct.calcsize(f)
        return v

    (fmt,) = rd('<i')
    if fmt != want_fmt:
        sys.exit('%s: format %d, expected %d' % (path, fmt, want_fmt))
    ver = rd('<4i')
    if tuple(ver) != tuple(want_version):
        sys.exit('%s: header version %s, expected %s' % (path, ver, want_version))
    (nlen,) = rd('<i')
    off += nlen
    (psz,) = rd('<i')
    (count,) = rd('<i')
    m = {}
    pid = 0
    poff = 0
    for _ in range(count):
        (t,) = rd('<B')
        lo, hi = t & 0xF, t >> 4
        if lo == 0: (i,) = rd('<Q')
        elif lo == 1: i = pid + 1
        elif lo == 2: i = pid + rd('<B')[0]
        elif lo == 3: i = pid - rd('<B')[0]
        elif lo == 4: i = pid + rd('<H')[0]
        elif lo == 5: i = pid - rd('<H')[0]
        elif lo == 6: (i,) = rd('<H')
        elif lo == 7: (i,) = rd('<I')
        else: sys.exit('bad id encoding')
        tmp = poff // psz if hi & 8 else poff
        h = hi & 7
        if h == 0: (o,) = rd('<Q')
        elif h == 1: o = tmp + 1
        elif h == 2: o = tmp + rd('<B')[0]
        elif h == 3: o = tmp - rd('<B')[0]
        elif h == 4: o = tmp + rd('<H')[0]
        elif h == 5: o = tmp - rd('<H')[0]
        elif h == 6: (o,) = rd('<H')
        elif h == 7: (o,) = rd('<I')
        if hi & 8:
            o *= psz
        m[i] = o
        pid, poff = i, o
    return m


# ---------------------------------------------------------------- derivations (no Address Library)
def td_rvas(img, mangled):
    out = []
    for m in re.finditer(re.escape(mangled.encode() + b'\0'), img.d):
        rva, sec = img.rva(m.start() - 0x10)
        if rva is not None and sec == '.data':
            out.append(rva)
    return out


def vtable_entries(img, mangled):
    """[(vtable_rva, col_offset, col_rva)] for every vtable of the class, any COL offset."""
    found = []
    for td in td_rvas(img, mangled):
        for cm in re.finditer(re.escape(struct.pack('<I', td)), img.d):
            coloff = cm.start() - 12
            if coloff < 0:
                continue
            sig, off, _cd = struct.unpack_from('<III', img.d, coloff)
            colrva, sec = img.rva(coloff)
            if sig != 1 or sec != '.rdata':
                continue
            if struct.unpack_from('<I', img.d, coloff + 20)[0] != colrva:
                continue
            for pm in re.finditer(re.escape(struct.pack('<Q', BASE + colrva)), img.d):
                prva, psec = img.rva(pm.start())
                if psec == '.rdata':
                    found.append((prva + 8, off, colrva))
    return sorted(set(found))


def vtables_for(img, mangled, col_offset):
    return sorted(set(vt for vt, off, _col in vtable_entries(img, mangled) if off == col_offset))


def base_name(img, col):
    """Name of the base subobject a COL's vtable belongs to: the first BaseClassArray entry whose
    non-virtual displacement equals the COL offset (entry 0 = the class itself = the primary)."""
    off = img.u32(col + 4)
    chd = img.u32(col + 16)
    n = img.u32(chd + 8)
    bca = img.u32(chd + 12)
    for i in range(n):
        bcd = img.u32(bca + 4 * i)
        mdisp, pdisp = struct.unpack('<ii', img.r(bcd + 8, 8))
        if pdisp == -1 and mdisp == off:
            name = img.r(img.u32(bcd) + 0x10, 160).split(b'\0')[0].decode(errors='replace')
            return name + (' (primary)' if i == 0 else '')
    return None


def sig_regex(sig):
    parts = []
    for tok in sig.split():
        parts.append(b'.' if tok == '??' else re.escape(bytes([int(tok, 16)])))
    return re.compile(b''.join(parts), re.DOTALL)


def sig_scan(img, sig):
    va, text = img.text_bytes()
    return [va + m.start() for m in sig_regex(sig).finditer(text)]


def follow_thunk(img, rva):
    b = img.r(rva, 5)
    if b[0] == 0xE9:
        return rva + 5 + struct.unpack('<i', b[1:5])[0]
    return rva


_md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
_md.detail = True


def make_sig(img, rva, max_len=512):
    """Minimal unique signature starting at rva: rel32 targets and RIP displacements wildcarded."""
    code = img.r(rva, max_len + 16)
    toks = []
    for ins in _md.disasm(code, rva):
        b = ['%02X' % x for x in ins.bytes]
        rip = any(op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP
                  for op in ins.operands)
        if rip and ins.disp_size:
            for k in range(ins.disp_offset, ins.disp_offset + ins.disp_size):
                b[k] = '??'
        if ins.group(capstone.x86.X86_GRP_BRANCH_RELATIVE) and ins.imm_size >= 4:
            for k in range(ins.imm_offset, ins.imm_offset + ins.imm_size):
                b[k] = '??'
        toks += b
        if len(toks) >= 16:
            hits = sig_scan(img, ' '.join(toks))
            if hits == [rva]:
                return ' '.join(toks)
        if len(toks) >= max_len:
            break
    return None


# ---------------------------------------------------------------- generation
def hexs(v):
    return '0x%X' % v


def derive(spec, imgs, libs):
    """returns rows_by_rt {rt: [ {label, id, rva, bytesOffset, bytes, method, slots, row} ]}, errors"""
    errors = []
    out = {rt: [] for rt in AL_RUNTIMES}
    derived = {rt: {} for rt in AL_RUNTIMES}   # (id) -> rva, for callsite targets
    for row in spec['rows']:
        for rt in AL_RUNTIMES:
            ids = row.get('ids', {})
            if rt not in ids:
                continue
            rid = ids[rt]
            img = imgs[rt]
            kind = row['kind']
            ent = {'label': row['seat'], 'id': rid, 'bytesOffset': 0, 'bytes': '', 'row': row, 'slots': []}
            if kind == 'vtable':
                v = vtables_for(img, row['rtti'], row.get('col_offset', 0))
                if len(v) != 1:
                    errors.append('%s %s: RTTI %s col_offset %d -> %d vtables %s' % (
                        rt, row['seat'], row['rtti'], row.get('col_offset', 0), len(v), [hexs(x) for x in v]))
                    continue
                ent['rva'] = v[0]
                ent['method'] = 'RTTI %s (COL offset %d)' % (row['rtti'], row.get('col_offset', 0))
                for s in row.get('slots', []):
                    si = int(s, 0)
                    tgt = img.u64(v[0] + 8 * si) - BASE
                    ent['slots'].append((si, tgt, follow_thunk(img, tgt)))
            elif kind == 'rtti':
                t = td_rvas(img, row['rtti'])
                if len(t) != 1:
                    errors.append('%s %s: TypeDescriptor %s -> %d hits' % (rt, row['seat'], row['rtti'], len(t)))
                    continue
                ent['rva'] = t[0]
                ent['method'] = 'TypeDescriptor %s' % row['rtti']
            elif kind in ('function', 'callsite'):
                sig = row['sig'][rt]
                hits = sig_scan(img, sig)
                if len(hits) != 1:
                    errors.append('%s %s: signature matches %d places %s' % (rt, row['seat'], len(hits), [hexs(x) for x in hits[:5]]))
                    continue
                ent['rva'] = hits[0]
                ent['method'] = 'signature (%d bytes, unique in .text)' % len(sig.split())
                if kind == 'callsite':
                    off = int(row['offset'][rt], 0)
                    want = bytes.fromhex(row['bytes'])
                    have = img.r(hits[0] + off, len(want))
                    if have != want:
                        errors.append('%s %s: bytes at +0x%X are %s, spec says %s' % (rt, row['seat'], off, have.hex(), want.hex()))
                        continue
                    ent['bytesOffset'] = off
                    ent['bytes'] = row['bytes']
                    ent['method'] += '; +0x%X = %s' % (off, row['bytes'].upper())
                    if row.get('target') and rt in row['target']:
                        tgt_rva = hits[0] + off + 5 + struct.unpack('<i', img.r(hits[0] + off + 1, 4))[0]
                        ent['target'] = (row['target'][rt], tgt_rva)
            elif kind == 'ripref':
                # A DATA GLOBAL, proven through the engine code that reads it: the function
                # (unique signature, its own id) must hold, at a fixed offset, exactly `bytes`
                # (per runtime: the RIP-relative instruction WITH its displacement, plus the
                # use-site bytes after it), and that instruction's RIP-relative target must be
                # the RVA the Address Library gives the global's id. Emits two runtime rows:
                # the function (id + byte check) and the global (id only).
                sig = row['sig'][rt]
                hits = sig_scan(img, sig)
                if len(hits) != 1:
                    errors.append('%s %s: signature matches %d places %s' % (rt, row['seat'], len(hits), [hexs(x) for x in hits[:5]]))
                    continue
                ent['rva'] = hits[0]
                off = int(row['offset'][rt], 0)
                want = bytes.fromhex(row['bytes'][rt])
                have = img.r(hits[0] + off, len(want))
                if have != want:
                    errors.append('%s %s: bytes at +0x%X are %s, spec says %s' % (rt, row['seat'], off, have.hex(), want.hex()))
                    continue
                ins = next(_md.disasm(want, hits[0] + off), None)
                ripmem = [op for op in (ins.operands if ins else [])
                          if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP]
                if not ins or len(ripmem) != 1:
                    errors.append('%s %s: +0x%X is not a single RIP-relative memory operand' % (rt, row['seat'], off))
                    continue
                gtgt = ins.address + ins.size + ripmem[0].mem.disp
                gid = row['global'][rt]
                glib = libs[rt].get(gid)
                if glib is None or glib != gtgt:
                    errors.append('%s %s: +0x%X references 0x%X, global id %d is %s' % (
                        rt, row['seat'], off, gtgt, gid, hexs(glib) if glib is not None else 'NOT in the library'))
                    continue
                ent['bytesOffset'] = off
                ent['bytes'] = row['bytes'][rt]
                ent['method'] = 'signature (%d bytes, unique in .text); +0x%X = %s -> global 0x%X' % (
                    len(sig.split()), off, row['bytes'][rt].upper(), gtgt)
                grow = dict(row)
                grow['kind'] = 'ripref global'
                ent['global'] = {'label': row['global_seat'], 'id': gid, 'rva': gtgt, 'bytesOffset': 0,
                                 'bytes': '', 'row': grow, 'slots': [],
                                 'method': 'RIP-relative reference at %s +0x%X (id %d)' % (row['seat'], off, rid)}
            else:
                errors.append('unknown kind %s' % kind)
                continue
            # only now: does the Address Library agree?
            lib = libs[rt].get(rid)
            if lib is None:
                errors.append('%s %s: id %d is NOT in the Address Library' % (rt, row['seat'], rid))
                continue
            if lib != ent['rva']:
                errors.append('%s %s: id %d -> library 0x%X, our derivation 0x%X' % (rt, row['seat'], rid, lib, ent['rva']))
                continue
            derived[rt][rid] = ent['rva']
            g = ent.pop('global', None)
            out[rt].append(ent)
            if g:
                derived[rt][g['id']] = g['rva']
                out[rt].append(g)
    # callsite target cross-check (target ids must be rows too, or be resolvable by the library)
    for rt in AL_RUNTIMES:
        for ent in out[rt]:
            if 'target' in ent:
                tid, trva = ent['target']
                want = derived[rt].get(tid, libs[rt].get(tid))
                if want != trva:
                    errors.append('%s %s: call at +0x%X lands on 0x%X, target id %d is 0x%X' % (
                        rt, ent['label'], ent['bytesOffset'], trva, tid, want or 0))
    return out, errors


def hx(b):
    return ' '.join('%02X' % x for x in b)


def load_idmap(path):
    """idmap CSV -> {1.6.1170 id (int) or 'rva:0x...' (str, a raw 1.6.1170 RVA): row}."""
    m = {}
    for r in csv.DictReader(open(path, newline='')):
        m[int(r['id']) if r['id'].isdigit() else r['id']] = r
    return m


def derive_idmap(spec, img, img16, idmap, out16):
    """1.7.104 column: the idmap's RVA, VERIFIED against the 1.7.104 image. Returns (rows, unverified);
    never fatal (a row that does not verify simply gets no 1.7.104 entry)."""
    rt = IDMAP_RT
    rows, unver = [], []
    by_id16 = {e['id']: e for e in out16}
    for row in spec['rows']:
        ids = row.get('ids', {})
        kind = row['kind']
        rid = ids.get(AE_RT)

        def skip(reason, rid=rid, row=row):
            unver.append({'label': row['seat'], 'id': rid, 'row': row, 'reason': reason})
            if row['kind'] == 'ripref':   # a ripref row is two table rows: the function and its global
                unver.append({'label': row['global_seat'], 'id': row['global'].get(AE_RT), 'row': row,
                              'reason': 'proven through %s, which did not verify: %s' % (row['seat'], reason)})

        if rid is None:
            skip('no 1.6.1170 (AE) id: the 1.7.104 map is keyed by AE id (1.5.97-only seat)')
            continue
        m = idmap.get(rid)
        if m is None:
            skip('id %d is not in the idmap' % rid)
            continue
        if not m['rva_1_7_104'].startswith('0x'):
            skip('UNRESOLVED in the idmap (%s)' % m['confidence'])
            continue
        want = int(m['rva_1_7_104'], 16)
        e16 = by_id16.get(rid)
        if e16 is None or int(m['rva_1_6_1170'], 16) != e16['rva']:
            skip('idmap rva_1_6_1170 %s disagrees with our 1.6.1170 derivation %s' % (
                m['rva_1_6_1170'], hexs(e16['rva']) if e16 else 'none'))
            continue
        ent = {'label': row['seat'], 'id': rid, 'bytesOffset': 0, 'bytes': '', 'row': row, 'slots': []}
        if kind == 'vtable':
            col16 = img16.u64(e16['rva'] - 8) - BASE
            bn = base_name(img16, col16)
            if bn is None:
                skip('no base subobject name for the 1.6.1170 COL')
                continue
            cands = [(vt, off) for vt, off, col in vtable_entries(img, row['rtti']) if base_name(img, col) == bn]
            if len(cands) != 1:
                skip('%s subobject [%s] -> %d vtables in 1.7.104 %s' % (row['rtti'], bn, len(cands), [hexs(c[0]) for c in cands]))
                continue
            vt, off = cands[0]
            if vt != want:
                skip('RTTI walk finds 0x%X for [%s], the idmap says 0x%X' % (vt, bn, want))
                continue
            ent['rva'] = vt
            ent['method'] = 'RTTI %s (COL offset %d, base %s)' % (row['rtti'], off, bn)
            if off != row.get('col_offset', 0):
                ent['method'] += ' [COL offset differs from the spec\'s %d: layout shift]' % row.get('col_offset', 0)
            for sl in row.get('slots', []):
                si = int(sl, 0)
                tgt = img.u64(vt + 8 * si) - BASE
                ent['slots'].append((si, tgt, follow_thunk(img, tgt)))
        elif kind == 'rtti':
            t = td_rvas(img, row['rtti'])
            if len(t) != 1 or t[0] != want:
                skip('TypeDescriptor %s -> %s, the idmap says 0x%X' % (row['rtti'], [hexs(x) for x in t], want))
                continue
            ent['rva'] = t[0]
            ent['method'] = 'TypeDescriptor %s' % row['rtti']
        elif kind in ('function', 'callsite', 'ripref'):
            sig = row['sig'].get(rt) or row['sig'][AE_RT]
            hits = sig_scan(img, sig)
            if hits != [want]:
                skip('signature %s matches %d places %s, the idmap says 0x%X' % (
                    'sig[1.7.104]' if row['sig'].get(rt) else 'sig[1.6.1170]', len(hits), [hexs(x) for x in hits[:4]], want))
                continue
            ent['rva'] = want
            ent['method'] = 'signature (%d bytes, unique in .text, at the idmap RVA)' % len(sig.split())
            if kind == 'callsite':
                off = int((row['offset'].get(rt) or row['offset'][AE_RT]), 0)
                wantb = bytes.fromhex(row['bytes'])
                have = img.r(want + off, len(wantb))
                if have != wantb:
                    skip('bytes at +0x%X are %s, spec says %s' % (off, have.hex(), wantb.hex()))
                    continue
                ent['bytesOffset'] = off
                ent['bytes'] = row['bytes']
                ent['method'] += '; +0x%X = %s' % (off, row['bytes'].upper())
                tid = (row.get('target') or {}).get(AE_RT)
                if tid is not None:
                    tm = idmap.get(tid)
                    if tm is None or not tm['rva_1_7_104'].startswith('0x'):
                        skip('call target id %d has no idmap RVA' % tid)
                        continue
                    tgt_rva = want + off + 5 + struct.unpack('<i', img.r(want + off + 1, 4))[0]
                    if tgt_rva != int(tm['rva_1_7_104'], 16):
                        skip('call at +0x%X lands on 0x%X, target id %d is %s in the idmap' % (off, tgt_rva, tid, tm['rva_1_7_104']))
                        continue
            elif kind == 'ripref':
                off = int((row['offset'].get(rt) or row['offset'][AE_RT]), 0)
                tmpl = bytes.fromhex(row['bytes'].get(rt) or row['bytes'][AE_RT])
                have = img.r(want + off, len(tmpl))
                # the RIP displacement differs per build; everything else must match the 1.6.1170 bytes
                ins16 = next(_md.disasm(tmpl, 0), None)
                if not ins16 or not ins16.disp_size:
                    skip('1.6.1170 ripref bytes do not start with a RIP-relative instruction')
                    continue
                dlo, dhi = ins16.disp_offset, ins16.disp_offset + ins16.disp_size
                if have[:dlo] != tmpl[:dlo] or have[dhi:] != tmpl[dhi:]:
                    skip('bytes at +0x%X are %s, 1.6.1170 template is %s (displacement wildcarded)' % (off, have.hex(), tmpl.hex()))
                    continue
                ins = next(_md.disasm(have, want + off), None)
                ripmem = [op for op in (ins.operands if ins else [])
                          if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP]
                if not ins or len(ripmem) != 1:
                    skip('+0x%X is not a single RIP-relative memory operand' % off)
                    continue
                gtgt = ins.address + ins.size + ripmem[0].mem.disp
                gid = row['global'][AE_RT]
                gm = idmap.get(gid)
                if gm is None or not gm['rva_1_7_104'].startswith('0x') or int(gm['rva_1_7_104'], 16) != gtgt:
                    skip('+0x%X references 0x%X, global id %d is %s in the idmap' % (off, gtgt, gid, gm['rva_1_7_104'] if gm else 'absent'))
                    continue
                ent['bytesOffset'] = off
                ent['bytes'] = hx(have)
                ent['method'] = 'signature (%d bytes, unique in .text, at the idmap RVA); +0x%X = %s -> global 0x%X' % (
                    len(sig.split()), off, hx(have), gtgt)
                grow = dict(row)
                grow['kind'] = 'ripref global'
                rows.append(ent)
                rows.append({'label': row['global_seat'], 'id': gid, 'rva': gtgt, 'bytesOffset': 0, 'bytes': '',
                             'row': grow, 'slots': [],
                             'method': 'RIP-relative reference at %s +0x%X (id %d)' % (row['seat'], off, rid)})
                continue
        else:
            skip('unknown kind %s' % kind)
            continue
        rows.append(ent)
    # raw RVAs the code holds per runtime (spec "raw_rvas", keyed by the 1.6.1170 RVA, no Address Library id):
    # the 1.6.1170 byte signature is cut at that RVA here (make_sig) and must hit exactly the idmap's RVA.
    # Emitted with id 0 (SelfCheck verifies base+rva, no library lookup). 1.6.1170/1.5.97 tables are unchanged.
    for raw in spec.get('raw_rvas', []):
        r16 = int(raw['rva'], 16)
        m = idmap.get('rva:' + raw['rva'].upper().replace('0X', '0x'))
        if m is None or not m['rva_1_7_104'].startswith('0x'):
            unver.append({'label': raw['seat'], 'id': None, 'row': raw, 'reason': 'raw RVA %s is %s in the idmap' % (
                raw['rva'], 'absent' if m is None else 'UNRESOLVED')})
            continue
        want = int(m['rva_1_7_104'], 16)
        sig = make_sig(img16, r16)
        if sig is None:
            unver.append({'label': raw['seat'], 'id': None, 'row': raw, 'reason': 'no unique 1.6.1170 signature at raw RVA %s' % raw['rva']})
            continue
        hits = sig_scan(img, sig)
        if hits != [want]:
            unver.append({'label': raw['seat'], 'id': None, 'row': raw, 'reason': 'signature cut at 1.6.1170 %s matches %d places %s, the idmap says 0x%X' % (
                raw['rva'], len(hits), [hexs(x) for x in hits[:4]], want)})
            continue
        rows.append({'label': raw['seat'], 'id': 0, 'rva': want, 'bytesOffset': 0, 'bytes': '',
                     'row': {'kind': 'raw rva', 'source': raw.get('source', ''), 'doc': raw.get('doc', '')}, 'slots': [],
                     'method': 'raw RVA: 1.6.1170 %s signature (%d bytes) unique at the idmap RVA' % (raw['rva'], len(sig.split()))})
    return rows, unver


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def write_header(spec, rows, unver, path):
    ns = spec['namespace']
    L = []
    L.append('#pragma once')
    L.append('')
    L.append('// GENERATED by tools/verified_addresses/gen_verified_addresses.py from')
    L.append('// tools/verified_addresses/spec.json. DO NOT EDIT BY HAND: change the spec and re-run')
    L.append('// (see Docs/VERIFIED-ADDRESSES.md, "Regenerating").')
    L.append('//')
    L.append('// One row per hook-critical / seat-critical Address Library id, per EXACT runtime. Each')
    L.append('// expected RVA was derived from our own copy of that executable (RTTI walk or a unique')
    L.append('// byte signature) and then matched against the Address Library for that build; the')
    L.append('// generator refuses to write this file if they disagree. REL::SelfCheck::Run() compares the')
    L.append('// library in use at runtime against these rows. See REL/SelfCheck.h.')
    L.append('//')
    L.append('// 1.7.104.0 has no Address Library: its rows are the ids of OUR OWN id map (keyed by the 1.6.1170')
    L.append('// id), each re-verified against the 1.7.104 executable. A seat that did not verify has NO row in')
    L.append('// that table (listed in a comment there), so IsVerifiedAddress() is false for it and the seat is')
    L.append('// refused. The Row/Table shapes are the same for every runtime.')
    L.append('')
    L.append('#include "REL/SelfCheck.h"')
    L.append('')
    L.append('namespace %s' % ns)
    L.append('{')
    names = {}
    for rt in RUNTIMES:
        var = 'kRows_' + rt.replace('.', '_')
        names[rt] = var
        L.append('\t// %s.0: %d rows' % (rt, len(rows[rt])))
        if rt == IDMAP_RT:
            L.append('\t// %d NOT VERIFIED on %s.0, deliberately absent from this table (the seat is refused):' % (len(unver), rt))
            for u in unver:
                L.append('\t//   %s (1.6.1170 id %s): %s' % (u['label'], u['id'] if u['id'] is not None else '-', u['reason']))
        L.append('\tinline constexpr REL::SelfCheck::Row %s[] = {' % var)
        for e in rows[rt]:
            # runtime_bytes: false -> the byte fact is checked here offline only; at runtime the row
            # verifies the function's own address (for sites no existing hook byte-checks today).
            rtb = e['row'].get('runtime_bytes', True) and e['bytes']
            bl = len(bytes.fromhex(e['bytes'])) if rtb else 0
            bb = ', '.join('0x%02X' % x for x in bytes.fromhex(e['bytes'])) if bl else ''
            L.append('\t\t{ %d, 0x%X, 0x%X, %d, { %s }, %s },' % (e['id'], e['rva'], e['bytesOffset'] if bl else 0, bl, bb, cstr(e['label'])))
        L.append('\t};')
        L.append('')
    L.append('\tinline constexpr REL::SelfCheck::Table kTables[] = {')
    for rt, meta in RUNTIMES.items():
        v = meta['version']
        L.append('\t\t{ REL::Version(%d, %d, %d, %d), %s, std::size(%s) },' % (v[0], v[1], v[2], v[3], names[rt], names[rt]))
    L.append('\t};')
    L.append('}')
    L.append('')
    open(path, 'w', newline='\n').write('\n'.join(L))


def write_doc(spec, rows, unver, path, args):
    L = []
    L.append('# Verified addresses: %s self-check table' % spec['repo'])
    L.append('')
    L.append('GENERATED by `tools/verified_addresses/gen_verified_addresses.py` from `tools/verified_addresses/spec.json`.')
    L.append('Do not edit by hand. The committed `native/VerifiedAddresses.h` is the same data as C++.')
    L.append('')
    L.append('Every row below is an Address Library id that %s hooks, calls or classifies with. For each exact' % spec['repo'])
    L.append('runtime the expected RVA was DERIVED FROM OUR OWN COPY OF THE EXECUTABLE (column "derived by"), and only')
    L.append('then compared with the Address Library for that build. All rows here agreed; the generator writes')
    L.append('nothing when one does not. At startup `REL::SelfCheck::Run` repeats the comparison against the library the')
    L.append('game actually loaded. A row that fails refuses THAT seat by name in the log; the expected RVA is never')
    L.append('used in place of the library\'s answer.')
    L.append('')
    L.append('Offline result of this generation: ' + ', '.join('%s.0 %d/%d verified, 0 refused' % (rt, len(rows[rt]), len(rows[rt])) for rt in AL_RUNTIMES) + '.')
    L.append('')
    L.append('1.7.104.0 has NO Address Library. Its column is OUR OWN id map (`_research/1.7.104-idmap/idmap-1.7.104.csv`, keyed by')
    L.append('the 1.6.1170 id) re-verified against the plaintext 1.7.104 executable by the same row-kind checks (see the script')
    L.append('docstring). Offline result: 1.7.104.0 %d/%d verified, %d NOT verified. A not-verified seat has no row in the 1.7.104 table,' % (
        len(rows[IDMAP_RT]), len(rows[IDMAP_RT]) + len(unver), len(unver)))
    L.append('so `IsVerifiedAddress` is false and the seat is refused by name. The table of `Not verified on 1.7.104` is below.')
    L.append('')
    L.append('## Rows')
    L.append('')
    L.append('| seat | kind | id 1.6.1170 | RVA 1.6.1170 | id 1.5.97 | RVA 1.5.97 | id 1.7.104 | RVA 1.7.104 | derived by | hooked slots -> engine function (1.6.1170 / 1.5.97 / 1.7.104) | code | doc |')
    L.append('|---|---|---|---|---|---|---|---|---|---|---|---|')
    by = {}
    for rt in RUNTIMES:
        for e in rows[rt]:
            by.setdefault(e['label'], {})[rt] = e
    for label, d in by.items():
        e0 = next(iter(d.values()))
        row = e0['row']
        cells = [label, row['kind']]
        for rt in RUNTIMES:
            if rt in d:
                cells += [str(d[rt]['id']) if d[rt]['id'] else '-', hexs(d[rt]['rva']) + (' +0x%X %s' % (d[rt]['bytesOffset'], d[rt]['bytes'].upper()) if d[rt]['bytes'] else '')]
            else:
                cells += ['-', '-']
        cells.append(e0['method'].split(';')[0])
        slots = []
        if e0['slots']:
            a = d.get('1.6.1170', {}).get('slots', [])
            s = d.get('1.5.97', {}).get('slots', [])
            n = d.get('1.7.104', {}).get('slots', [])
            for i in range(max(len(a), len(s), len(n))):
                sa = a[i] if i < len(a) else None
                ss = s[i] if i < len(s) else None
                sn = n[i] if i < len(n) else None
                idx = (sa or ss or sn)[0]
                slots.append('0x%02X: %s / %s / %s' % (idx, hexs(sa[1]) if sa else '-', hexs(ss[1]) if ss else '-', hexs(sn[1]) if sn else '-'))
        cells.append('<br>'.join(slots) if slots else '')
        cells.append(row.get('source', ''))
        cells.append(row.get('doc', ''))
        L.append('| ' + ' | '.join(cells) + ' |')
    L.append('')
    L.append('## Not verified on 1.7.104')
    L.append('')
    L.append('These seats have NO row in the 1.7.104 table, so on 1.7.104 `SelfCheck::IsVerifiedAddress` / `IsVerifiedLabel` is false')
    L.append('for them and the seat is refused. Nothing here is a failure on 1.6.1170 or 1.5.97.')
    L.append('')
    if unver:
        L.append('| seat | 1.6.1170 id | reason |')
        L.append('|---|---|---|')
        for u in unver:
            L.append('| %s | %s | %s |' % (u['label'], u['id'] if u['id'] is not None else '-', u['reason'].replace('|', '/')))
    else:
        L.append('(none)')
    L.append('')
    if spec.get('layout_facts'):
        L.append('## Slots and offsets that are not Address Library ids')
        L.append('')
        L.append('These cannot be checked by id at runtime. Each is a layout fact of an exact build, verified by the')
        L.append('source named here, and reached only on the runtimes its gate lets through. They are listed so the')
        L.append('audit covers everything the DLL installs or reads.')
        L.append('')
        L.append('| what | value | where used | verified by | runtime gate today |')
        L.append('|---|---|---|---|---|')
        for f in spec['layout_facts']:
            L.append('| %s | %s | %s | %s | %s |' % (f['what'], f['value'], f['where'], f['verified_by'], f['gate']))
        L.append('')
    L.append('## Regenerating')
    L.append('')
    L.append('```')
    L.append('python3 tools/verified_addresses/gen_verified_addresses.py --spec tools/verified_addresses/spec.json \\')
    L.append('    --bin-1.6.1170 <binaries/1.6.1170/SkyrimSE.unpacked.exe> --al-1.6.1170 <versionlib-1-6-1170-0.bin> \\')
    L.append('    --bin-1.5.97 <binaries/1.5.97/SkyrimSE.unpacked.exe> --al-1.5.97 <version-1-5-97-0.bin> \\')
    L.append('    --bin-1.7.104 <binaries/1.7.104/SkyrimSE.exe> --idmap <_research/1.7.104-idmap/idmap-1.7.104.csv> \\')
    L.append('    --header native/VerifiedAddresses.h --doc Docs/VERIFIED-ADDRESSES.md')
    L.append('```')
    L.append('')
    L.append('- The executables are gitignored (`binaries/README.md` in MFO: SteamStub-unpacked, raw = 0x400 + rva - 0x1000).')
    L.append('  CI never runs this; the output is committed and reviewed.')
    L.append('- The 1.6.1170 library must be `versionlib-1-6-1170-0.bin` (the script asserts id 38048 -> 0x6A35F0).')
    L.append('- Adding a row: add it to `spec.json`. A vtable row needs the mangled RTTI name (`.?AV...@@`); a function row')
    L.append('  needs a signature per runtime, cut with `--make-sig <runtime> <rva>` at an RVA confirmed by the address-table')
    L.append('  method (skeleton + neighbour delta). The generator then re-finds it by unique scan and checks the library.')
    L.append('- A `ripref` row proves a DATA GLOBAL through the engine function that reads it: the function\'s signature and')
    L.append('  id, `offset` and per-runtime `bytes` (the RIP-relative instruction with its displacement, then the use-site')
    L.append('  bytes, 15 at most), and `global` (the global\'s id per runtime). The instruction\'s RIP target must equal the')
    L.append('  library\'s answer for `global`. It emits two rows: the function (id + byte check) and `global_seat` (id only).')
    L.append('- 1.7.104 has no library: `--idmap` is our own id map (md5 of the exe is asserted: 113faeb71fd8f62b26d0c8627299ab40).')
    L.append('  Its rows are looked up by the 1.6.1170 id. A new spec row needs no 1.7.104 input: it is verified with its')
    L.append('  1.6.1170 signature at the idmap RVA, or it lands in the table above. An optional `sig["1.7.104"]`,')
    L.append('  `offset["1.7.104"]` or `bytes["1.7.104"]` overrides the 1.6.1170 one for that runtime only.')
    L.append('- Optional spec list `raw_rvas` ({seat, rva = the raw 1.6.1170 RVA the code holds, source}): code constants that are not')
    L.append('  Address Library ids. They exist only in the 1.7.104 table (id 0 = base+rva is verified, no library lookup): the')
    L.append('  1.6.1170 signature is cut at the RVA and must hit exactly the idmap RVA (`rva:0x...` rows). The 1.6.1170 and')
    L.append('  1.5.97 tables do not get them.')
    L.append('- Keep `gen_verified_addresses.py` identical in MFO and APMF.')
    L.append('')
    open(path, 'w', newline='\n').write('\n'.join(L))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--spec')
    ap.add_argument('--header')
    ap.add_argument('--doc')
    ap.add_argument('--make-sig', nargs=2, metavar=('RUNTIME', 'RVA'))
    for rt, meta in RUNTIMES.items():
        ap.add_argument('--bin-' + rt, dest='bin_' + rt.replace('.', '_'), required=True)
        if not meta.get('idmap'):
            ap.add_argument('--al-' + rt, dest='al_' + rt.replace('.', '_'), required=True)
    ap.add_argument('--idmap', required=True, help='idmap-1.7.104.csv (our own id map; there is no 1.7.104 Address Library)')
    a = ap.parse_args()
    imgs, libs = {}, {}
    for rt, meta in RUNTIMES.items():
        k = rt.replace('.', '_')
        imgs[rt] = Image(getattr(a, 'bin_' + k))
        if meta.get('idmap'):
            if hashlib.md5(imgs[rt].d).hexdigest() != meta['md5']:
                sys.exit('%s: executable md5 is not %s (wrong 1.7.104 build?)' % (rt, meta['md5']))
            continue
        libs[rt] = load_addrlib(getattr(a, 'al_' + k), meta['fmt'], meta['version'])
        sid, srva = meta['sanity']
        if libs[rt].get(sid) != srva:
            sys.exit('%s: library sanity id %d -> %s, expected 0x%X (wrong library file for this exe?)' % (
                rt, sid, libs[rt].get(sid), srva))
    if a.make_sig:
        rt, rva = a.make_sig[0], int(a.make_sig[1], 0)
        print(make_sig(imgs[rt], rva))
        return
    spec = json.load(open(a.spec))
    rows, errors = derive(spec, imgs, libs)
    if errors:
        print('REFUSED: %d row(s) did not verify; nothing written.' % len(errors))
        for e in errors:
            print('  ' + e)
        sys.exit(1)
    rows[IDMAP_RT], unver = derive_idmap(spec, imgs[IDMAP_RT], imgs[AE_RT], load_idmap(a.idmap), rows[AE_RT])
    write_header(spec, rows, unver, a.header)
    write_doc(spec, rows, unver, a.doc, a)
    for rt in RUNTIMES:
        print('%s.0: %d rows verified' % (rt, len(rows[rt])))
    print('%s.0: %d NOT verified (no row emitted):' % (IDMAP_RT, len(unver)))
    for u in unver:
        print('  %s (id %s): %s' % (u['label'], u['id'], u['reason']))


if __name__ == '__main__':
    main()
