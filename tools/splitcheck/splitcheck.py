#!/usr/bin/env python3
"""splitcheck -- prove a source move/split left the compiled code unchanged,
function by function.

Compares two builds (DLL + PDB) of MFO. Every function the PDB knows about is
matched by NAME + SIGNATURE, its bytes are disassembled in lockstep, and every
address-carrying field (RIP-relative displacements, rel8/rel32 branch targets,
image-relative displacements, abs64 relocations) is compared by the SYMBOL it
resolves to rather than by its raw value, because a split legitimately moves
code and data around the image.

A function that is not byte-identical after that normalisation is FAIL unless
it falls in one of the EXPLAINED classes below (each is listed in the output):

  inline-drift   MSVC (no LTCG) decides inlining per translation unit, so the
                 same source compiled in a differently-composed TU can inline a
                 different set of helpers (mostly STL/CommonLib templates). A
                 pair is drift only when (1) its PDB inline-site lists differ
                 and (2) its OWN-CODE fingerprint is equal: every call, data,
                 string-literal and RTTI reference made outside the inlined
                 regions, with "inlined X" == "calls X".
  funclet-drift  an unwind funclet (`...'::`1'::dtor$N / catch$N) of a drift
                 function whose fingerprint is equal (its frame offsets follow
                 the parent's new stack layout).
  outlined       a function present as an out-of-line body in only one build
                 and INLINED (PDB inline site) in the other.
  copies         a header-defined internal-linkage symbol (one copy per TU that
                 includes the header) whose copy count changed; every copy is
                 byte-identical to a copy in the other build.

RESULT: PASS           every function identical, nothing added or missing
        PASS-EXPLAINED no FAIL; the explained classes above are listed
        FAIL           anything else

See README.md. Needs llvm-pdbutil on PATH and python packages capstone + pefile.
"""
import argparse
import bisect
import json
import re
import subprocess
import sys
from collections import Counter, defaultdict

import capstone
from capstone import x86_const as X
import pefile

ANON = "`anonymous namespace'::"
# MSVC spells the SAME anonymous namespace two ways inside one PDB: as
# "`anonymous namespace'" and, inside template arguments, as "A0x<hash>" where
# the hash is per-TU and not even stable across rebuilds of an unchanged TU.
ANON_HASH = re.compile(r"\bA0x[0-9a-f]{8}::")
# the same thing inside a decorated (public / RTTI) name: ...@?A0x1234abcd@...
ANON_HASH_MANGLED = re.compile(r"\?A0x[0-9a-f]{8}@")

# named data compared by compare_data(): the plugin's own variables/constants
DATA_SCOPE = re.compile(r"^(const )?(`)?MFO::")
PATHLIT = re.compile(rb"[A-Za-z]:\\.*\\native\\.*\.(cpp|h)$", re.I)
FUNCLET = re.compile(r"^`(.*)'::`\d+'::(dtor|catch)\$\d+$")
# a function DEFINED by a library header (STL, CommonLib, SKSE, fmt/spdlog...),
# possibly instantiated over an MFO type. Its out-of-line body is a COMDAT that
# the linker takes from whichever TU it sees first, so a split can swap in
# another TU's (equally valid) compilation of the same template.
LIBRARY = re.compile(r"^`?(std|RE|REL|SKSE|SKSE::stl|fmt|spdlog|nlohmann|rapidcsv|ImGui|stl|"
                     r"Concurrency|concurrency|__std|__scrt|_)[:A-Za-z_]")


def canon(name):
    return ANON_HASH_MANGLED.sub("?A0x@", ANON_HASH.sub(ANON, name))


def norm(name):
    """Name with the anonymous namespace removed (a file-local symbol that a
    split made extern keeps its name otherwise)."""
    return name.replace(ANON, "").replace("?A0x@", "")


def short(name):
    """Last component of a demangled name, template args dropped; ctor/dtor
    spelled the way llvm-pdbutil prints an inlinee ({ctor} / {dtor})."""
    s = re.sub(r"<(lambda_\d+)>", r"\1", name)
    prev = None
    while prev != s:                      # drop template argument lists, innermost first
        prev = s
        s = re.sub(r"<[^<>]*>", "", s)
    parts = [p.strip("`'") for p in s.split("::")]
    last = parts[-1]
    if last.startswith("~"):
        return "{dtor}"
    if len(parts) >= 2 and parts[-2] == last:
        return "{ctor}"
    return last


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
        self.procs = []           # dicts: name, sig, rva, size, key
        self.names_at = defaultdict(set)  # rva -> set(names)  (procs, data, publics)
        self.proc_at = {}         # rva -> size (for containment)
        self.datas = []           # (name, rva) of named variables/constants
        self.inl_ranges = {}      # (name, rva) -> [(start, end, inlinee short name)] top-level sites
        self.inl_all = {}         # (name, rva) -> Counter(inlinee short names, all depths)
        self.inlinees = Counter()  # every inlinee short name anywhere in the image
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
        hdr = re.compile(r"^\s*(\d+) \| (S_[A-Z0-9_]+) \[size = \d+\](?: `(.*)`)?\s*$")
        modre = re.compile(r"^\s*Mod \d+ \|")
        addr = re.compile(r"addr = (\d+):(\d+)")
        csize = re.compile(r"code size = (\d+)")
        typ = re.compile(r"type = `?0x[0-9A-F]+ \((.*)\)`?")
        inl = re.compile(r"inlinee = 0x[0-9A-F]+ \((.*)\), parent = (\d+), end = (\d+)")
        codere = re.compile(r"\bcode (?:end )?0x([0-9A-F]+)")
        seen = set()
        cur_proc = None       # (key, record offset)
        sites = {}            # record offset -> (key, top-level?, name)
        cur_site = None       # [key, toplevel, name, ranges, open_start]
        n = len(lines)

        def close_site():
            nonlocal cur_site
            if cur_site is None:
                return
            key, top, nm, ranges, open_start = cur_site
            if open_start is not None:
                ranges.append((open_start, open_start + 1))
            if top and key is not None:
                for a, b in ranges:
                    self.inl_ranges.setdefault(key, []).append((a, b, nm))
            cur_site = None

        for i in range(n):
            line = lines[i]
            if modre.match(line):
                close_site()
                cur_proc = None
                sites = {}
                continue
            m = hdr.match(line)
            if not m:
                if cur_site is not None and "code" in line:
                    # binary annotations: "code 0xA (+..)" opens/extends a range,
                    # "code end 0xB" closes it at B
                    parts = line.split("code end")
                    for tok in codere.finditer(parts[0]):
                        if cur_site[4] is None:
                            cur_site[4] = int(tok.group(1), 16)
                    if len(parts) > 1:
                        e = re.search(r"0x([0-9A-F]+)", parts[1])
                        if e and cur_site[4] is not None:
                            cur_site[3].append((cur_site[4], int(e.group(1), 16)))
                            cur_site[4] = None
                continue
            close_site()
            recoff, kind, name = int(m.group(1)), m.group(2), canon(m.group(3) or "")
            body = " ".join(lines[i + 1:i + 3])
            if kind == "S_INLINESITE":
                im = inl.search(body)
                if im and cur_proc is not None:
                    nm = im.group(1)
                    parent = int(im.group(2))
                    top = parent == cur_proc[1]
                    self.inl_all.setdefault(cur_proc[0], Counter())[nm] += 1
                    self.inlinees[nm] += 1
                    cur_site = [cur_proc[0], top, nm, [], None]
                continue
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
                    cur_proc = None
                    continue
                key = (name, rva)
                cur_proc = (key, recoff)
                if (name, sig, rva) in seen:
                    continue
                seen.add((name, sig, rva))
                self.procs.append({"name": name, "sig": sig, "rva": rva, "size": size, "key": key})
                self.names_at[rva].add(name)
                self.proc_at[rva] = max(self.proc_at.get(rva, 0), size)
            elif kind in ("S_GDATA32", "S_LDATA32", "S_GTHREAD32", "S_LTHREAD32", "S_PUB32"):
                self.names_at[rva].add(name)
                if kind in ("S_GDATA32", "S_LDATA32"):
                    self.datas.append((name, rva))
        close_site()

    def read(self, rva, n):
        return bytes(self.data[rva:rva + n])

    def cstring(self, rva, limit=512):
        b = self.read(rva, limit)
        z = b.find(b"\0")
        return b if z < 0 else b[:z]

    def type_descriptor_name(self, rva):
        """Decorated class name if rva is an RTTI TypeDescriptor
        ({pVFTable, spare, char name[]}), else None."""
        if rva not in self.relocs:
            return None
        s = self.cstring(rva + 16, 1024)
        if not s.startswith(b".?A"):
            return None
        # drop the anonymous-namespace component and the decorated name's
        # back-reference digits (they renumber when that component goes away)
        return re.sub(r"@\d+@", "@#@", norm(canon(s.decode(errors="replace"))))

    def resolve(self, rva):
        """Candidate interpretations of a target address, best first:
        list of (kind, names(frozenset), offset, rva)."""
        if rva < self.secs[0][2]:
            return [("hdr", frozenset(), rva, rva)]
        td = self.type_descriptor_name(rva)
        if td is not None:
            return [("rtti", frozenset([td]), 0, rva)]
        if rva in self.proc_at:
            return [("proc", frozenset(self.names_at[rva]), 0, rva)]
        j = bisect.bisect_right(self.proc_starts, rva) - 1
        if j >= 0:
            st = self.proc_starts[j]
            if st <= rva < st + self.proc_at[st]:
                return [("proc", frozenset(self.names_at[st]), rva - st, rva)]
        out = []
        j = bisect.bisect_right(self.sym_rvas, rva) - 1
        if j >= 0:
            st = self.sym_rvas[j]
            if self.sec_of(st) == self.sec_of(rva) and rva - st < 0x10000:
                out.append(("sym", frozenset(self.names_at[st]), rva - st, rva))
                if st == rva and j > 0:
                    # one-past-the-end of the previous object (a loop bound)
                    pv = self.sym_rvas[j - 1]
                    if self.sec_of(pv) == self.sec_of(rva) and rva - pv < 0x10000:
                        out.append(("sym", frozenset(self.names_at[pv]), rva - pv, rva))
        out.append(("none", frozenset(), 0, rva))
        return out


# --------------------------------------------------------------------------
# comparison
# --------------------------------------------------------------------------
class Cmp:
    def __init__(self, A, B):
        self.A, self.B = A, B
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True
        self.memo = {}
        self.notes = defaultdict(int)   # accepted-by-rule counters
        self.path_pairs = set()
        self._dis = {}

    def disasm(self, img, rva, size):
        k = (id(img), rva, size)
        if k not in self._dis:
            self._dis[k] = list(self.md.disasm(img.read(rva, size), rva))
        return self._dis[k]

    def fields(self, img, ins):
        """Address-carrying fields of one instruction:
        list of (offset_in_insn, size, kind, target_rva)."""
        out = []
        for k in range(ins.size - 7):
            if ins.address + k in img.relocs:
                val = int.from_bytes(ins.bytes[k:k + 8], "little") - img.base
                out.append((k, 8, "abs", val))
        if out:
            return out
        for op in ins.operands:
            if op.type == X.X86_OP_MEM:
                if op.mem.base == X.X86_REG_RIP:
                    # RIP-relative displacements are always 32-bit; capstone
                    # misreports disp_size as 2 under a 0x66 prefix (movdqa)
                    out.append((ins.disp_offset, 4, "rip", ins.address + ins.size + op.mem.disp))
                elif op.mem.disp >= 0x1000 and (op.mem.base != 0 or op.mem.index != 0) \
                        and img.sec_of(op.mem.disp) is not None:
                    # image-relative disp32 (lea base,__ImageBase ; [base+idx*4+TABLE])
                    out.append((ins.disp_offset, 4, "imgrel", op.mem.disp))
            elif op.type == X.X86_OP_IMM and (ins.group(capstone.CS_GRP_JUMP) or
                                               ins.group(capstone.CS_GRP_CALL)):
                out.append((ins.imm_offset, ins.imm_size, "rel", op.imm))
        return out

    @staticmethod
    def masked(ins, flds):
        b = bytearray(ins.bytes)
        for off, sz, _k, _t in flds:
            for q in range(off, off + sz):
                if q < len(b):
                    b[q] = 0
        return bytes(b)

    def toks(self, img, f, kind, tgt):
        if f["rva"] <= tgt < f["rva"] + f["size"] and kind in ("rel", "rip", "imgrel"):
            return [("local", frozenset(), tgt - f["rva"], tgt)]
        return img.resolve(tgt)

    def same_target(self, la, lb, depth):
        for ta in la:
            for tb in lb:
                if self._same(ta, tb, depth):
                    return True
        return False

    def _same(self, ta, tb, depth):
        ka, na, oa, ra = ta
        kb, nb, ob, rb = tb
        if ka in ("local", "hdr", "rtti") or kb in ("local", "hdr", "rtti"):
            return ka == kb and oa == ob and (na == nb) and (ka != "hdr" or ra == rb)
        if ka == "none" or kb == "none":
            if ka == kb == "none":
                sa, sb = self.A.sec_of(ra), self.B.sec_of(rb)
                if sa == sb and sa not in (".text", ".data", ".bss", None) \
                        and self.A.read(ra, 16) == self.B.read(rb, 16):
                    self.notes["unnamed read-only data equal by content"] += 1
                    return True
            return False
        if oa == ob and na and nb and (na == nb or {norm(x) for x in na} & {norm(x) for x in nb}):
            if na != nb:
                self.notes["target name-set differs only by ICF alias / anon namespace"] += 1
            return True
        # string literals: content compare, source-path literals tolerated
        if oa == ob == 0 and na and nb and all(x.startswith("??_C@") for x in na | nb):
            sa, sb = self.A.cstring(ra), self.B.cstring(rb)
            if sa == sb:
                return True
            if PATHLIT.search(sa) and PATHLIT.search(sb):
                self.path_pairs.add((sa.decode(errors="replace"), sb.decode(errors="replace")))
                self.notes["source-path string literal moved file"] += 1
                return True
            return False
        # different named functions: equal if structurally identical (ICF-like)
        if ka == kb == "proc" and oa == ob == 0 and depth < 4:
            fa = {"rva": ra, "size": self.A.proc_at[ra], "name": min(na), "key": None}
            fb = {"rva": rb, "size": self.B.proc_at[rb], "name": min(nb), "key": None}
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

    def tables(self, img, f):
        """Offsets (function-relative) of switch tables MSVC places after the
        function body but inside the PDB's code range: every image-relative
        displacement of a decoded instruction that lands inside the function."""
        out = set()
        for ins in self.disasm(img, f["rva"], f["size"]):
            off = ins.address - f["rva"]
            if out and off >= min(out):
                break  # past the first table: the rest is data, not code
            for _o, _s, k, t in self.fields(img, ins):
                if k == "imgrel" and f["rva"] <= t < f["rva"] + f["size"]:
                    out.add(t - f["rva"])
        return sorted(out)

    def _compare(self, fa, fb, depth):
        A, B = self.A, self.B
        pre = f"size {fa['size']:#x} vs {fb['size']:#x}" if fa["size"] != fb["size"] else ""
        tba, tbb = self.tables(A, fa), self.tables(B, fb)
        if tba != tbb:
            return pre + f" switch-table layout differs {[hex(x) for x in tba]} vs {[hex(x) for x in tbb]}"
        code_end = tba[0] if tba else min(fa["size"], fb["size"])
        ia = self.disasm(A, fa["rva"], code_end)
        ib = self.disasm(B, fb["rva"], code_end)
        for x, y in zip(ia, ib):
            off = x.address - fa["rva"]
            if y.address - fb["rva"] != off:
                return pre + f" @+{off:#x}: instruction boundary diverges"
            fla = self.fields(A, x)
            flb = self.fields(B, y)
            desc = pre + f" @+{off:#x}: A `{x.mnemonic} {x.op_str}` | B `{y.mnemonic} {y.op_str}`"
            if len(fla) != len(flb) or self.masked(x, fla) != self.masked(y, flb):
                return desc
            for (oa_, sa_, ka_, ta_), (ob_, sb_, kb_, tb_) in zip(fla, flb):
                if (oa_, sa_, ka_) != (ob_, sb_, kb_):
                    return desc
                t1 = self.toks(A, fa, ka_, ta_)
                t2 = self.toks(B, fb, kb_, tb_)
                if ka_ == "imgrel" and ta_ == tb_ and not (t1[0][0] == "local" or t2[0][0] == "local"):
                    continue  # identical constant displacement that only looked like an RVA
                if not self.same_target(t1, t2, depth):
                    return desc + f"  [{ka_} target A={fmt(t1[0])} B={fmt(t2[0])}]"
        if len(ia) != len(ib):
            return pre + f" instruction count {len(ia)} vs {len(ib)}"
        ea = (ia[-1].address + ia[-1].size - fa["rva"]) if ia else 0
        eb = (ib[-1].address + ib[-1].size - fb["rva"]) if ib else 0
        if ea != eb:
            return pre + f" decoded length {ea:#x} vs {eb:#x}"
        if ea < code_end:
            return pre + f" @+{ea:#x}: undecodable bytes inside the code region"
        if fa["size"] != fb["size"]:
            return pre + f" (code identical up to +{code_end:#x})"
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
        return None

    # ---- the modulo-inlining fingerprint -------------------------------------
    def fingerprint(self, img, f):
        """(own, every): own = references made OUTSIDE f's top-level inline
        sites plus one ("inl", callee) token per top-level site; every = all
        references anywhere in f plus every inlinee at any depth."""
        ranges = img.inl_ranges.get(f["key"], [])
        own, every = [], []
        tbl = self.tables(img, f)
        end = tbl[0] if tbl else f["size"]
        for ins in self.disasm(img, f["rva"], end):
            off = ins.address - f["rva"]
            inside = any(a <= off < b for a, b, _n in ranges)
            for _o, _s, kind, t in self.fields(img, ins):
                cands = self.toks(img, f, kind, t)
                if cands[0][0] == "local":
                    continue
                tok = ("ref", img, tuple(cands))
                every.append(tok)
                if not inside:
                    own.append(tok)
        for nm in sorted({n for _a, _b, n in ranges}):
            own.append(("inl", None, nm))
        for nm in img.inl_all.get(f["key"], {}):
            every.append(("inl", None, nm))
        return own, every

    def fp_equal(self, fa, fb):
        """Modulo-inlining equivalence (SET semantics): every own-code token of
        either side has an equivalent token ANYWHERE in the other side, where
        "inlined X" is equivalent to "calls X". Returns the unmatched tokens."""
        oa, ea = self.fingerprint(self.A, fa)
        ob, eb = self.fingerprint(self.B, fb)
        un = []
        for side, mine, theirs in (("A", oa, eb), ("B", ob, ea)):
            for x in mine:
                if not any(self._fp_tok_eq(x, y) for y in theirs):
                    un.append((side, x))
        return un

    def _fp_tok_eq(self, x, y):
        if x[0] == "inl" and y[0] == "inl":
            return x[2] == y[2]
        if x[0] == "ref" and y[0] == "ref":
            a, b = (x, y) if x[1] is self.A else (y, x)
            return self.same_target(list(a[2]), list(b[2]), 1)
        # "inlined X" == "calls X"
        inl, ref = (x, y) if x[0] == "inl" else (y, x)
        c = ref[2][0]
        return c[0] == "proc" and c[2] == 0 and inl[2] in {short(n) for n in c[1]}


def fmt(t):
    k, names, off, rva = t
    if k == "local":
        return f"local+{off:#x}"
    if k in ("hdr", "none"):
        return f"{k}@{rva:#x}"
    names = sorted(names)
    n = names[0] if names else "?"
    more = f" (+{len(names) - 1} aliases)" if len(names) > 1 else ""
    return f"{n}{more}+{off:#x}"


def fmt_fp(u):
    side, tok = u
    if tok[0] == "inl":
        return f"{side}: inlined {tok[2]}"
    return f"{side}: ref {fmt(tok[2][0])}"


# --------------------------------------------------------------------------
# named data
# --------------------------------------------------------------------------
def compare_data(A, B, cmp, cap=512):
    """Initial contents of every named MFO variable/constant present once in
    each build. Extent = distance to the next symbol in EACH image (min of
    both, capped); DIR64 pointer slots are compared by the symbol they point to."""
    da, db = defaultdict(set), defaultdict(set)
    for n, r in A.datas:
        da[norm(n)].add(r)
    for n, r in B.datas:
        db[norm(n)].add(r)
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
                if not cmp.same_target(A.resolve(va), B.resolve(vb), 0):
                    bad = f"+{k:#x}: pointer A={fmt(A.resolve(va)[0])} B={fmt(B.resolve(vb)[0])}"
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


# --------------------------------------------------------------------------
# matching
# --------------------------------------------------------------------------
def match(A, B, cmp):
    pairs, onlya, onlyb = [], list(A.procs), list(B.procs)

    def run_pass(keyfn, tag, unique_only=False):
        nonlocal onlya, onlyb
        ga, gb = defaultdict(list), defaultdict(list)
        for f in onlya:
            ga[keyfn(f)].append(f)
        for f in onlyb:
            gb[keyfn(f)].append(f)
        ra, rb = [], []
        for k in set(ga) | set(gb):
            la, lb = ga.get(k, []), gb.get(k, [])
            if not la or not lb or (unique_only and (len(la) > 1 or len(lb) > 1)):
                ra += la
                rb += lb
                continue
            la, lb = list(la), list(lb)
            got = []
            for fa in list(la):          # identical pairs first
                for fb in lb:
                    if cmp.compare(fa, fb) is None:
                        got.append((fa, fb))
                        la.remove(fa)
                        lb.remove(fb)
                        break
            la.sort(key=lambda f: f["rva"])
            lb.sort(key=lambda f: f["rva"])
            if len(la) == len(lb):       # leftovers pair in address order
                got += list(zip(la, lb))
                la, lb = [], []
            elif not la or not lb:
                pass
            else:                        # unequal counts: keep leftovers unpaired
                pass
            pairs.extend((a, b, tag) for a, b in got)
            ra += la
            rb += lb
        onlya, onlyb = ra, rb

    run_pass(lambda f: (f["name"], f["sig"]), "exact")
    # a file-local helper that became extern so several split files can share it
    run_pass(lambda f: (norm(f["name"]), norm(f["sig"])), "relinked")
    # the PDB truncates long signature strings, cutting "`anonymous namespace'"
    # in half ("`anonymous-..."): fall back to the name when it is unique
    run_pass(lambda f: norm(f["name"]), "relinked", unique_only=True)
    return pairs, onlya, onlyb


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
    ap.add_argument("--max", type=int, default=60, help="max entries printed per list")
    ap.add_argument("--strict", action="store_true",
                    help="no explained classes: anything not byte-identical is FAIL")
    args = ap.parse_args()

    A = Image(args.a_dll, args.a_pdb)
    B = Image(args.b_dll, args.b_pdb)
    cmp = Cmp(A, B)
    rules = load_allow(args.allow)
    pairs, onlya, onlyb = match(A, B, cmp)

    same = 0
    relinked, fails, drift, fdrift, allowed_hits = [], [], [], [], []
    diffs = {}
    for fa, fb, tag in pairs:
        r = cmp.compare(fa, fb)
        if tag == "relinked":
            relinked.append((fa["name"], fb["name"], r is None))
        if r is None:
            same += 1
        else:
            diffs[(fa["key"], fb["key"])] = (fa, fb, r)
    drift_names = set()
    # pass 1: whole functions
    for (ka, kb), (fa, fb, r) in sorted(diffs.items(), key=lambda x: x[1][0]["name"]):
        if FUNCLET.match(fa["name"]):
            continue
        ia, ib = A.inl_all.get(ka, Counter()), B.inl_all.get(kb, Counter())
        un = cmp.fp_equal(fa, fb) if not args.strict else ["strict"]
        if not args.strict and not un and (ia != ib or LIBRARY.match(fa["name"])):
            delta = {k: (ia[k], ib[k]) for k in set(ia) | set(ib) if ia[k] != ib[k]}
            drift.append((fa["name"], r, delta))
            drift_names.add(norm(fa["name"]))
            continue
        why = allowed(rules, "differ", fa["name"])
        if why:
            allowed_hits.append(("differ", fa["name"], why))
            continue
        extra = "" if ia != ib else "  (inline sites identical)"
        fails.append((fa["name"], fa["sig"], r + extra, [fmt_fp(u) for u in un[:6]] if un != ["strict"] else []))
    # pass 2: unwind funclets
    for (ka, kb), (fa, fb, r) in sorted(diffs.items(), key=lambda x: x[1][0]["name"]):
        m = FUNCLET.match(fa["name"])
        if not m:
            continue
        parent = norm(m.group(1))
        un = cmp.fp_equal(fa, fb)
        if not args.strict and parent in drift_names and not un:
            fdrift.append((fa["name"], r))
            continue
        why = allowed(rules, "differ", fa["name"])
        if why:
            allowed_hits.append(("differ", fa["name"], why))
            continue
        fails.append((fa["name"], fa["sig"], r, [fmt_fp(u) for u in un[:6]]))

    # only-in: copies, outlined
    copies, outlined, oa, ob = [], [], [], []
    for mine, other, img_other, side, sink in ((onlya, B.procs, B, "A", oa), (onlyb, A.procs, A, "B", ob)):
        by = defaultdict(list)
        for f in other:
            by[(norm(f["name"]), norm(f["sig"]))].append(f)
        for f in mine:
            if not args.strict:
                twins = by.get((norm(f["name"]), norm(f["sig"])), [])
                if twins and any((cmp.compare(f, t) if side == "A" else cmp.compare(t, f)) is None for t in twins):
                    copies.append((side, f["name"]))
                    continue
                sn = short(f["name"])
                if img_other.inlinees.get(sn):
                    outlined.append((side, f["name"], sn))
                    continue
            why = allowed(rules, "only-" + side.lower(), f["name"])
            if why:
                allowed_hits.append(("only-" + side.lower(), f["name"], why))
            else:
                sink.append(f)

    dchecked, ddiff0 = compare_data(A, B, cmp)
    ddiff = []
    for n, r in ddiff0:
        why = allowed(rules, "data", n)
        (allowed_hits.append(("data", n, why)) if why else ddiff.append((n, r)))

    strict_ok = not diffs and not onlya and not onlyb and not ddiff0
    ok = not fails and not oa and not ob and not ddiff
    verdict = "PASS" if strict_ok else ("PASS-EXPLAINED" if ok else "FAIL")

    print(f"A: {args.a_dll}  ({len(A.procs)} procs)")
    print(f"B: {args.b_dll}  ({len(B.procs)} procs)")
    print(f"matched pairs: {len(pairs)}   identical: {same}   FAIL: {len(fails)}   "
          f"inline-drift: {len(drift)}   funclet-drift: {len(fdrift)}   "
          f"only-in-A: {len(oa)}   only-in-B: {len(ob)}   outlined: {len(outlined)}   "
          f"copies: {len(copies)}   allow-listed: {len(allowed_hits)}")
    print(f"named data compared: {dchecked}   data differing: {len(ddiff)}")
    if relinked:
        rl = [x for x in relinked if not x[0].startswith(("std::", "`std::"))]
        print(f"\nanonymous-namespace -> extern re-links ({len(relinked)}; {len(relinked) - len(rl)} "
              f"STL instantiations over a moved type not listed):")
        for a, b, eq in rl[:args.max * 5]:
            print(f"  {'identical' if eq else 'DIFFERS  '}  {a}  ->  {b}")
    if cmp.notes:
        print("\nnormalisations applied (accepted, counted per reference):")
        for k, v in sorted(cmp.notes.items()):
            print(f"  {v:7d}  {k}")
    if cmp.path_pairs:
        print(f"\nsource-path literals that changed ({len(cmp.path_pairs)} distinct pairs):")
        for a, b in sorted(cmp.path_pairs)[:args.max]:
            print(f"  {a}  ->  {b}")
    sections = (
        ("INLINE-DRIFT (explained: inline sites differ -- or a library COMDAT taken from "
         "another TU -- and the own-code fingerprint is equal)",
         [f"{n}\n      {r}\n      inline sites A->B: " +
          ", ".join(f"{k} {a}->{b}" for k, (a, b) in sorted(d.items())) for n, r, d in drift]),
        ("FUNCLET-DRIFT (explained: unwind funclet of a drift function, fingerprint equal)",
         [f"{n}" for n, r in fdrift]),
        ("OUTLINED IN ONE BUILD ONLY (explained: inlined in the other)",
         [f"[only in {s}] {n}   (inlined as `{sn}` in the other)" for s, n, sn in outlined]),
        ("PER-TU COPIES (explained: copy count changed, every copy identical)",
         [f"[extra in {s}] {n}" for s, n in copies]),
        ("FAIL", [f"{n}  {s}\n      {r}" + "".join(f"\n        {u}" for u in us) for n, s, r, us in fails]),
        ("DATA DIFFERING", [f"{n}\n      {r}" for n, r in ddiff]),
        ("ONLY IN A", [f"{f['name']}  ({f['sig']})" for f in oa]),
        ("ONLY IN B", [f"{f['name']}  ({f['sig']})" for f in ob]),
        ("ALLOW-LISTED", [f"[{k}] {n}   -- {w}" for k, n, w in allowed_hits]),
    )
    for title, lst in sections:
        if lst:
            print(f"\n{title} ({len(lst)}):")
            for s in lst[:args.max]:
                print("  " + s)
            if len(lst) > args.max:
                print(f"  ... {len(lst) - args.max} more (see --json)")
    print("\nRESULT:", verdict)
    if args.json:
        json.dump({"result": verdict, "identical": same,
                   "fail": [{"name": n, "sig": s, "detail": r, "fingerprint": u} for n, s, r, u in fails],
                   "inline_drift": [{"name": n, "detail": r, "inline_delta": {k: list(v) for k, v in d.items()}}
                                    for n, r, d in drift],
                   "funclet_drift": [n for n, _ in fdrift],
                   "outlined": outlined, "copies": copies,
                   "only_a": [f["name"] for f in oa], "only_b": [f["name"] for f in ob],
                   "data_differing": ddiff, "relinked": relinked, "allowed": allowed_hits,
                   "notes": cmp.notes, "path_pairs": sorted(cmp.path_pairs)},
                  open(args.json, "w"), indent=1, default=str)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
