#!/usr/bin/env python3
"""splitcheck -- prove a source move/split left the compiled code unchanged,
function by function.

Compares two builds (DLL + PDB) of MFO. Every function the PDB knows about is
matched by NAME + SIGNATURE, its bytes are disassembled in lockstep, and every
address-carrying field (RIP-relative displacements, rel8/rel32 branch targets,
image-relative displacements, abs64 relocations) is compared by the SYMBOL it
resolves to rather than by its raw value, because a split legitimately moves
code and data around the image. See README.md for the full contract.

Needs: llvm-pdbutil on PATH, python packages capstone + pefile.
"""
import argparse
import bisect
import json
import re
import subprocess
import sys
from collections import defaultdict

import capstone
from capstone import x86_const as X
import pefile

ANON = "`anonymous namespace'::"
# MSVC spells the SAME anonymous namespace two ways inside one PDB: as
# "`anonymous namespace'" and, inside template arguments, as "A0x<hash>" where
# the hash is per-TU and not even stable across rebuilds of an unchanged TU.
ANON_HASH = re.compile(r"\bA0x[0-9a-f]{8}::")


DATA_SCOPE = re.compile(r"^(const )?(`)?MFO::")


def canon(name):
    return ANON_HASH.sub(ANON, name)
PATHLIT = re.compile(rb"[A-Za-z]:\\.*\\native\\.*\.(cpp|h)$", re.I)


# --------------------------------------------------------------------------
# PDB + PE loading
# --------------------------------------------------------------------------
class Image:
    def __init__(self, dll, pdb):
        self.dll = dll
        self.pe = pefile.PE(dll, fast_load=False)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = self.pe.get_memory_mapped_image()
        self.secs = []  # (index1, name, va, vsize)
        for i, s in enumerate(self.pe.sections, 1):
            self.secs.append((i, s.Name.rstrip(b"\0").decode(), s.VirtualAddress,
                              max(s.Misc_VirtualSize, s.SizeOfRawData)))
        self.relocs = set()  # RVAs of DIR64 relocations
        if hasattr(self.pe, "DIRECTORY_ENTRY_BASERELOC"):
            for blk in self.pe.DIRECTORY_ENTRY_BASERELOC:
                for e in blk.entries:
                    if e.type == 10:  # IMAGE_REL_BASED_DIR64
                        self.relocs.add(e.rva)
        self.procs = []           # dicts: name, sig, rva, size
        self.names_at = defaultdict(set)  # rva -> set(names)  (procs, data, publics)
        self.proc_at = {}         # rva -> size (for containment)
        self.datas = []           # (name, rva) of named variables/constants
        self._load_pdb(pdb)
        self.sym_rvas = sorted(self.names_at)
        self.proc_starts = sorted(self.proc_at)

    def sec_rva(self, sec, off):
        for i, _n, va, _sz in self.secs:
            if i == sec:
                return va + off
        return None

    def sec_of(self, rva):
        for _i, n, va, sz in self.secs:
            if va <= rva < va + sz:
                return n
        return None

    def _load_pdb(self, pdb):
        out = subprocess.run(["llvm-pdbutil", "dump", "-symbols", "-globals", "-publics", pdb],
                             check=True, capture_output=True, text=True, errors="replace").stdout
        lines = out.splitlines()
        hdr = re.compile(r"^\s*\d+ \| (S_[A-Z0-9_]+) \[size = \d+\] `(.*)`\s*$")
        addr = re.compile(r"addr = (\d+):(\d+)")
        csize = re.compile(r"code size = (\d+)")
        typ = re.compile(r"type = `?0x[0-9A-F]+ \((.*)\)`?")
        seen = set()
        i = 0
        n = len(lines)
        while i < n:
            m = hdr.match(lines[i])
            if not m:
                i += 1
                continue
            kind, name = m.group(1), canon(m.group(2))
            body = " ".join(lines[i + 1:i + 3])
            i += 1
            a = addr.search(body)
            if not a:
                continue
            rva = self.sec_rva(int(a.group(1)), int(a.group(2)))
            if rva is None:
                continue
            if kind in ("S_GPROC32", "S_LPROC32", "S_GPROC32_ID", "S_LPROC32_ID"):
                s = csize.search(body)
                t = typ.search(body)
                size = int(s.group(1)) if s else 0
                sig = canon(t.group(1)) if t else "<no type>"
                if size == 0:
                    continue
                key = (name, sig, rva)
                if key in seen:
                    continue
                seen.add(key)
                self.procs.append({"name": name, "sig": sig, "rva": rva, "size": size})
                self.names_at[rva].add(name)
                self.proc_at[rva] = max(self.proc_at.get(rva, 0), size)
            elif kind in ("S_GDATA32", "S_LDATA32", "S_GTHREAD32", "S_LTHREAD32", "S_PUB32"):
                self.names_at[rva].add(name)
                if kind in ("S_GDATA32", "S_LDATA32"):
                    self.datas.append((name, rva))

    def read(self, rva, n):
        return bytes(self.data[rva:rva + n])

    def cstring(self, rva, limit=512):
        b = self.read(rva, limit)
        z = b.find(b"\0")
        return b if z < 0 else b[:z]

    def resolve(self, rva):
        """-> (kind, names(frozenset), offset). kind in proc/sym/none."""
        if rva in self.proc_at:
            return ("proc", frozenset(self.names_at[rva]), 0)
        j = bisect.bisect_right(self.proc_starts, rva) - 1
        if j >= 0:
            st = self.proc_starts[j]
            if st <= rva < st + self.proc_at[st]:
                return ("proc", frozenset(self.names_at[st]), rva - st)
        j = bisect.bisect_right(self.sym_rvas, rva) - 1
        if j >= 0:
            st = self.sym_rvas[j]
            if self.sec_of(st) == self.sec_of(rva) and rva - st < 0x10000:
                return ("sym", frozenset(self.names_at[st]), rva - st)
        return ("none", frozenset(), rva)


def norm(name):
    return name.replace(ANON, "")


# --------------------------------------------------------------------------
# comparison
# --------------------------------------------------------------------------
class Cmp:
    def __init__(self, A, B, line_slack=None):
        self.A, self.B = A, B
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True
        self.memo = {}
        self.notes = defaultdict(int)   # accepted-by-rule counters
        self.path_pairs = set()

    def disasm(self, img, rva, size):
        return list(self.md.disasm(img.read(rva, size), rva))

    def fields(self, img, ins, fstart, fsize):
        """Address-carrying fields of one instruction:
        list of (offset_in_insn, size, kind, target_rva)."""
        out = []
        d = ins
        # abs64 relocations (mov r64, imm64 of an address, or data in code)
        for k in range(ins.size - 7):
            if ins.address + k in img.relocs:
                val = int.from_bytes(ins.bytes[k:k + 8], "little") - img.base
                out.append((k, 8, "abs", val))
        if any(f[2] == "abs" for f in out):
            return out
        for op in d.operands:
            if op.type == X.X86_OP_MEM:
                if op.mem.base == X.X86_REG_RIP:
                    tgt = ins.address + ins.size + op.mem.disp
                    # RIP-relative displacements are always 32-bit; capstone
                    # misreports disp_size as 2 under a 0x66 prefix (movdqa)
                    out.append((d.disp_offset, 4, "rip", tgt))
                elif op.mem.disp >= 0x1000 \
                        and (op.mem.base != 0 or op.mem.index != 0) \
                        and img.sec_of(op.mem.disp) is not None:
                    # image-relative disp32 (lea base,__ImageBase ; [base+idx*4+TABLE])
                    out.append((d.disp_offset, 4, "imgrel", op.mem.disp))
            elif op.type == X.X86_OP_IMM and ins.group(capstone.CS_GRP_JUMP) or \
                    op.type == X.X86_OP_IMM and ins.group(capstone.CS_GRP_CALL):
                out.append((d.imm_offset, d.imm_size, "rel", op.imm))
        return out

    def masked(self, ins, flds):
        b = bytearray(ins.bytes)
        for off, sz, _k, _t in flds:
            for q in range(off, off + sz):
                if q < len(b):
                    b[q] = 0
        return bytes(b)

    def tok(self, img, f, kind, tgt):
        fstart, fsize = f["rva"], f["size"]
        if fstart <= tgt < fstart + fsize and kind in ("rel", "rip", "imgrel"):
            return ("local", tgt - fstart)
        if tgt < img.secs[0][2]:
            return ("hdr", frozenset(), tgt, tgt)   # PE header (__ImageBase, e_lfanew ...)
        r = img.resolve(tgt)
        return r + (tgt,)

    def same_target(self, ta, tb, depth):
        if ta[0] == "local" or tb[0] == "local":
            return ta == tb
        ka, na, oa, ra = ta
        kb, nb, ob, rb = tb
        if ka == "hdr" or kb == "hdr":
            return ta == tb
        if oa == ob and na and nb and (na == nb or {norm(x) for x in na} & {norm(x) for x in nb}):
            if na != nb:
                self.notes["target name-set differs only by ICF/anon (intersection accepted)"] += 1
            return True
        # both untyped: compare contents
        if ka == "none" and kb == "none":
            sa, sb = self.A.sec_of(ra), self.B.sec_of(rb)
            if sa == sb and sa not in (".text", ".data", ".bss") \
                    and self.A.read(ra, 16) == self.B.read(rb, 16):
                self.notes["unnamed read-only data equal by content"] += 1
                return True
            return False
        # string literals: content compare, source-path literals tolerated
        if oa == ob and all(x.startswith("??_C@") for x in na | nb) and na and nb:
            sa, sb = self.A.cstring(ra), self.B.cstring(rb)
            if sa == sb:
                return True
            if PATHLIT.search(sa) and PATHLIT.search(sb):
                self.path_pairs.add((sa.decode(errors="replace"), sb.decode(errors="replace")))
                self.notes["source-path string literal moved file"] += 1
                return True
            return False
        # different named functions: equal if structurally identical (ICF-like)
        if ka == "proc" and kb == "proc" and oa == 0 and ob == 0 and depth < 4:
            fa = {"rva": ra, "size": self.A.proc_at[ra], "name": min(na)}
            fb = {"rva": rb, "size": self.B.proc_at[rb], "name": min(nb)}
            if self.compare(fa, fb, depth + 1) is None:
                self.notes["callee names differ, callee bodies identical"] += 1
                return True
        return False

    def compare(self, fa, fb, depth=0):
        """None when identical after normalisation, else a diff description."""
        key = (fa["rva"], fb["rva"])
        if key in self.memo:
            return self.memo[key]
        self.memo[key] = None  # coinductive: assume equal while in progress
        res = self._compare(fa, fb, depth)
        self.memo[key] = res
        return res

    def _compare(self, fa, fb, depth):
        A, B = self.A, self.B
        if fa["size"] != fb["size"]:
            return f"size {fa['size']:#x} vs {fb['size']:#x}" + self._first(fa, fb, depth)
        return self._first(fa, fb, depth, only=True)

    def tables(self, img, f):
        """Offsets (function-relative) of switch tables MSVC places after the
        function body but inside the PDB's code range: every image-relative
        displacement of a decoded instruction that lands inside the function."""
        out = set()
        end = f["size"]
        for ins in self.disasm(img, f["rva"], f["size"]):
            off = ins.address - f["rva"]
            if out and off >= min(out):
                break  # past the first table: the rest is data, not code
            for _o, _s, k, t in self.fields(img, ins, f["rva"], f["size"]):
                if k == "imgrel" and f["rva"] <= t < f["rva"] + end:
                    out.add(t - f["rva"])
        return sorted(out)

    def _first(self, fa, fb, depth, only=False):
        A, B = self.A, self.B
        tba, tbb = self.tables(A, fa), self.tables(B, fb)
        if tba != tbb:
            return f" switch-table layout differs {[hex(x) for x in tba]} vs {[hex(x) for x in tbb]}"
        code_end = tba[0] if tba else min(fa["size"], fb["size"])
        ia = self.disasm(A, fa["rva"], code_end)
        ib = self.disasm(B, fb["rva"], code_end)
        for x, y in zip(ia, ib):
            off = x.address - fa["rva"]
            if y.address - fb["rva"] != off:
                return f" @+{off:#x}: instruction boundary diverges"
            fla = self.fields(A, x, fa["rva"], fa["size"])
            flb = self.fields(B, y, fb["rva"], fb["size"])
            desc = f" @+{off:#x}: A `{x.mnemonic} {x.op_str}` | B `{y.mnemonic} {y.op_str}`"
            if len(fla) != len(flb) or self.masked(x, fla) != self.masked(y, flb):
                return desc
            for (oa_, sa_, ka_, ta_), (ob_, sb_, kb_, tb_) in zip(fla, flb):
                if (oa_, sa_, ka_) != (ob_, sb_, kb_):
                    return desc
                if ka_ == "imgrel" and ta_ == tb_:
                    # identical raw value: either the same table offset (checked by
                    # tables()) or a plain constant displacement that only looked like an RVA
                    t1 = self.tok(A, fa, ka_, ta_)
                    t2 = self.tok(B, fb, kb_, tb_)
                    if t1 == t2 or not (t1[0] == "local" or t2[0] == "local"):
                        continue
                    return desc + f"  [imgrel A={fmt(t1)} B={fmt(t2)}]"
                t1 = self.tok(A, fa, ka_, ta_)
                t2 = self.tok(B, fb, kb_, tb_)
                if not self.same_target(t1, t2, depth):
                    return desc + f"  [{ka_} target A={fmt(t1)} B={fmt(t2)}]"
        if len(ia) != len(ib):
            return f" instruction count {len(ia)} vs {len(ib)}"
        ea = (ia[-1].address + ia[-1].size - fa["rva"]) if ia else 0
        eb = (ib[-1].address + ib[-1].size - fb["rva"]) if ib else 0
        if ea != eb:
            return f" decoded length {ea:#x} vs {eb:#x}"
        if ea < code_end:
            return f" @+{ea:#x}: undecodable bytes inside the code region"
        if fa["size"] != fb["size"]:
            return f" (code identical up to +{code_end:#x})"
        # data region (switch tables): dwords that point into the function are
        # compared as function-relative offsets, everything else raw
        da = A.read(fa["rva"] + code_end, fa["size"] - code_end)
        db = B.read(fb["rva"] + code_end, fb["size"] - code_end)
        k = 0
        while k < len(da):
            if k + 4 <= len(da):
                va = int.from_bytes(da[k:k + 4], "little")
                vb = int.from_bytes(db[k:k + 4], "little")
                ina = fa["rva"] <= va < fa["rva"] + fa["size"]
                inb = fb["rva"] <= vb < fb["rva"] + fb["size"]
                if ina and inb:
                    if va - fa["rva"] != vb - fb["rva"]:
                        return f" @+{code_end + k:#x}: switch-table entry differs"
                    k += 4
                    continue
            if da[k] != db[k]:
                return f" @+{code_end + k:#x}: switch-table data differs"
            k += 1
        return None if only else ""


def fmt(t):
    if t[0] == "local":
        return f"local+{t[1]:#x}"
    names = sorted(t[1])
    n = names[0] if names else "?"
    more = f" (+{len(names) - 1} aliases)" if len(names) > 1 else ""
    return f"{n}{more}+{t[2]:#x}"


# --------------------------------------------------------------------------
# matching
# --------------------------------------------------------------------------
def compare_data(A, B, cmp, cap=512):
    """Initial contents of every named variable/constant present once in each
    build. Extent = distance to the next symbol in EACH image (min of both,
    capped); DIR64 pointer slots are compared by the symbol they point to."""
    da, db = defaultdict(set), defaultdict(set)
    for n, r in A.datas:
        da[n].add(r)
    for n, r in B.datas:
        db[n].add(r)
    diffs, checked = [], 0
    for n in sorted(set(da) & set(db)):
        # scope: the plugin's own variables/constants/vtables. RTTI records hold
        # 32-bit image-relative offsets and per-TU anonymous-namespace hashes,
        # and CRT/load-config/guard data is link metadata, not program state.
        if not DATA_SCOPE.match(n) or "`RTTI" in n:
            continue
        if len(da[n]) != 1 or len(db[n]) != 1:
            continue
        ra, rb = next(iter(da[n])), next(iter(db[n]))
        sa, sb = A.sec_of(ra), B.sec_of(rb)
        if sa != sb or sa in (None, ".text"):
            diffs.append((n, f"section {sa} vs {sb}"))
            continue

        def extent(img, r):
            j = bisect.bisect_right(img.sym_rvas, r)
            nxt = img.sym_rvas[j] if j < len(img.sym_rvas) else r + cap
            return nxt - r
        ln = min(extent(A, ra), extent(B, rb), cap)
        ba, bb = A.read(ra, ln), B.read(rb, ln)
        checked += 1
        k = 0
        bad = None
        while k < ln:
            pa, pb = (ra + k) in A.relocs, (rb + k) in B.relocs
            if pa or pb:
                if not (pa and pb):
                    bad = f"+{k:#x}: pointer slot in one build only"
                    break
                va = int.from_bytes(ba[k:k + 8], "little") - A.base
                vb = int.from_bytes(bb[k:k + 8], "little") - B.base
                t1 = A.resolve(va) + (va,) if va >= A.secs[0][2] else ("hdr", frozenset(), va, va)
                t2 = B.resolve(vb) + (vb,) if vb >= B.secs[0][2] else ("hdr", frozenset(), vb, vb)
                if not cmp.same_target(t1, t2, 0):
                    bad = f"+{k:#x}: pointer A={fmt(t1)} B={fmt(t2)}"
                    break
                k += 8
                continue
            if ba[k] != bb[k]:
                bad = f"+{k:#x}: byte {ba[k]:#04x} vs {bb[k]:#04x}"
                break
            k += 1
        if bad:
            diffs.append((n, bad))
    return checked, diffs


def match(A, B, cmp):
    ga, gb = defaultdict(list), defaultdict(list)
    for f in A.procs:
        ga[(f["name"], f["sig"])].append(f)
    for f in B.procs:
        gb[(f["name"], f["sig"])].append(f)
    pairs, onlya, onlyb = [], [], []

    def pair_groups(la, lb):
        la, lb = list(la), list(lb)
        got = []
        for fa in list(la):
            for fb in lb:
                if cmp.compare(fa, fb) is None:
                    got.append((fa, fb))
                    la.remove(fa)
                    lb.remove(fb)
                    break
        # leftovers: pair in address order (they will be reported as differing)
        la.sort(key=lambda f: f["rva"])
        lb.sort(key=lambda f: f["rva"])
        for fa, fb in zip(la, lb):
            got.append((fa, fb))
        return got, la[len(lb):], lb[len(la):]

    for k in set(ga) | set(gb):
        la, lb = ga.get(k, []), gb.get(k, [])
        if la and lb:
            if len(la) == 1 and len(lb) == 1:
                pairs.append((la[0], lb[0], False))
            else:
                got, ra, rb = pair_groups(la, lb)
                pairs += [(a, b, False) for a, b in got]
                onlya += ra
                onlyb += rb
        else:
            onlya += la
            onlyb += lb
    # second pass: anonymous-namespace <-> named (a file-local helper that
    # became extern so several split files can share it)
    na, nb = defaultdict(list), defaultdict(list)
    for f in onlya:
        na[(norm(f["name"]), norm(f["sig"]))].append(f)
    for f in onlyb:
        nb[(norm(f["name"]), norm(f["sig"]))].append(f)
    onlya2, onlyb2 = [], []
    for k in set(na) | set(nb):
        la, lb = na.get(k, []), nb.get(k, [])
        if la and lb:
            got, ra, rb = pair_groups(la, lb)
            pairs += [(a, b, True) for a, b in got]
            onlya2 += ra
            onlyb2 += rb
        else:
            onlya2 += la
            onlyb2 += lb
    return pairs, onlya2, onlyb2


def load_allow(path):
    rules = []
    if not path:
        return rules
    for ln in open(path, encoding="utf-8"):
        ln = ln.rstrip("\n")
        if not ln.strip() or ln.lstrip().startswith("#"):
            continue
        kind, rx, why = ln.split("\t", 2)
        rules.append((kind, re.compile(rx), why))
    return rules


def allowed(rules, kind, name):
    for k, rx, why in rules:
        if k in (kind, "any") and rx.search(name):
            return why
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a_dll")
    ap.add_argument("a_pdb")
    ap.add_argument("b_dll")
    ap.add_argument("b_pdb")
    ap.add_argument("--allow", help="allow-list file (TAB separated: kind regex justification)")
    ap.add_argument("--json", help="write the full result as JSON here")
    ap.add_argument("--max", type=int, default=40, help="max entries printed per list")
    ap.add_argument("--filter", help="only report functions whose name matches this regex (triage aid; a filtered run never PASSes)")
    args = ap.parse_args()

    A = Image(args.a_dll, args.a_pdb)
    B = Image(args.b_dll, args.b_pdb)
    cmp = Cmp(A, B)
    rules = load_allow(args.allow)
    pairs, onlya, onlyb = match(A, B, cmp)
    flt = re.compile(args.filter) if args.filter else None

    same, diff, relinked, allowed_hits = 0, [], [], []
    for fa, fb, viaanon in sorted(pairs, key=lambda p: p[0]["name"]):
        if flt and not flt.search(fa["name"]):
            continue
        r = cmp.compare(fa, fb)
        if viaanon:
            relinked.append((fa["name"], fb["name"], r is None))
        if r is None:
            same += 1
            continue
        why = allowed(rules, "differ", fa["name"])
        if why:
            allowed_hits.append(("differ", fa["name"], why))
            continue
        diff.append((fa["name"], fa["sig"], r))
    oa, ob = [], []
    for f in onlya:
        if flt and not flt.search(f["name"]):
            continue
        why = allowed(rules, "only-a", f["name"])
        (allowed_hits.append(("only-a", f["name"], why)) if why else oa.append(f))
    for f in onlyb:
        if flt and not flt.search(f["name"]):
            continue
        why = allowed(rules, "only-b", f["name"])
        (allowed_hits.append(("only-b", f["name"], why)) if why else ob.append(f))

    dchecked, ddiff0 = compare_data(A, B, cmp)
    ddiff = []
    for n, r in ddiff0:
        if flt and not flt.search(n):
            continue
        why = allowed(rules, "data", n)
        (allowed_hits.append(("data", n, why)) if why else ddiff.append((n, r)))
    ok = not diff and not oa and not ob and not ddiff and not flt
    print(f"A: {args.a_dll}  ({len(A.procs)} procs)")
    print(f"B: {args.b_dll}  ({len(B.procs)} procs)")
    print(f"matched pairs: {len(pairs)}   identical: {same}   differing: {len(diff)}   "
          f"only-in-A: {len(oa)}   only-in-B: {len(ob)}   allow-listed: {len(allowed_hits)}")
    print(f"named data compared: {dchecked}   data differing: {len(ddiff)}")
    if relinked:
        print(f"\nanonymous-namespace <-> extern re-links ({len(relinked)}):")
        for a, b, eq in relinked[:args.max * 5]:
            print(f"  {'identical' if eq else 'DIFFERS  '}  {a}  ->  {b}")
    if cmp.notes:
        print("\nnormalisations applied (accepted, counted per reference):")
        for k, v in sorted(cmp.notes.items()):
            print(f"  {v:7d}  {k}")
    if cmp.path_pairs:
        print(f"\nsource-path literals that changed ({len(cmp.path_pairs)} distinct pairs):")
        for a, b in sorted(cmp.path_pairs)[:args.max]:
            print(f"  {a}  ->  {b}")
    for title, lst in (("DIFFERING", [f"{n}  {s}\n      {r}" for n, s, r in diff]),
                       ("DATA DIFFERING", [f"{n}\n      {r}" for n, r in ddiff]),
                       ("ONLY IN A", [f"{f['name']}  ({f['sig']})" for f in oa]),
                       ("ONLY IN B", [f"{f['name']}  ({f['sig']})" for f in ob]),
                       ("ALLOW-LISTED", [f"[{k}] {n}   -- {w}" for k, n, w in allowed_hits])):
        if lst:
            print(f"\n{title} ({len(lst)}):")
            for s in lst[:args.max]:
                print("  " + s)
            if len(lst) > args.max:
                print(f"  ... {len(lst) - args.max} more (see --json)")
    print("\nRESULT:", "PASS" if ok else "FAIL")
    if args.json:
        json.dump({"pass": ok, "identical": same,
                   "differing": [{"name": n, "sig": s, "detail": r} for n, s, r in diff],
                   "only_a": oa, "only_b": ob, "data_differing": ddiff,
                   "relinked": relinked, "allowed": allowed_hits,
                   "notes": cmp.notes, "path_pairs": sorted(cmp.path_pairs)},
                  open(args.json, "w"), indent=1, default=list)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
