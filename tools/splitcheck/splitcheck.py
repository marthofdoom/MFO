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
                 (or it is a pure library COMDAT, no `MFO::` in its name),
                 (2) its OWN-CODE fingerprint is equal: every call, data,
                 string-literal and RTTI reference made outside the inlined
                 regions, with "inlined X" == "calls X" on qualified names, and
                 (3) its constant multiset (own code + inlined MFO code) is
                 equal, or a differing constant comes from a helper that side
                 inlined more.
  funclet-drift  an unwind funclet (`...'::`1'::dtor$N / catch$N) of a drift
                 function whose fingerprint is equal (its frame offsets follow
                 the parent's new stack layout).
  outlined       a function present as an out-of-line body in only one build
                 and INLINED (PDB inline site) in the other.
  copies         a header-defined internal-linkage symbol (one copy per TU that
                 includes the header) whose copy count changed; every copy is
                 byte-identical to a copy in the other build.

RESULT: PASS                     every function identical, nothing added or missing (exit 0)
        PASS-PROVEN              no FAIL, and --proof (the /Od builds of the same two
                                 commits) compares strict PASS (exit 0)
        PASS-EXPLAINED-UNPROVEN  no FAIL, explained classes only, no proof (exit 3;
                                 not enough for a split, CLAUDE.md)
        FAIL                     anything else (exit 1)

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
XTOR = re.compile(r"`dynamic (atexit destructor|initializer) for '")
# a function DEFINED by a library header (STL, CommonLib, SKSE, fmt/spdlog...),
# possibly instantiated over an MFO type. Its out-of-line body is a COMDAT that
# the linker takes from whichever TU it sees first, so a split can swap in
# another TU's (equally valid) compilation of the same template.
LIBRARY = re.compile(r"^`?(std|RE|REL|SKSE|SKSE::stl|fmt|spdlog|nlohmann|rapidcsv|ImGui|stl|"
                     r"Concurrency|concurrency|__std|__scrt|_)[:A-Za-z_]")


def is_text(b, minlen=2):
    """A NUL-free byte string that reads as text: valid UTF-8 (so a log line
    with an em-dash counts), no control characters but tab/newline/CR."""
    if len(b) < minlen:
        return False
    try:
        s = b.decode("utf-8")
    except UnicodeDecodeError:
        return False
    return all(c >= " " or c in "\t\n\r" for c in s)


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


def split_scope(name):
    """Split a demangled name on '::' at template/paren depth 0."""
    parts, cur, depth = [], "", 0
    i = 0
    while i < len(name):
        ch = name[i]
        if ch in "<(":
            depth += 1
        elif ch in ">)":
            depth -= 1
        if depth == 0 and name.startswith("::", i):
            parts.append(cur)
            cur = ""
            i += 2
            continue
        cur += ch
        i += 1
    parts.append(cur)
    return parts


def qual_key(name):
    """The FULLY QUALIFIED spelling "inlined X" and "calls X" are compared on:
    anonymous namespace and whitespace removed, template arguments KEPT. The
    PDB's inline sites carry an id whose qualified name we rebuild from the IPI
    stream (Image.inl_name), so two different functions that merely share a
    last name component (erase, clear, Reset, {ctor}) can no longer stand in
    for each other."""
    n = untag(name).replace("`anonymous-namespace'::", ANON)
    n = re.sub(r"\s+", "", norm(canon(n)))
    # The IPI stream spells a member function WITHOUT its own template argument
    # list (`...>::erase`), the symbol record WITH it (`...>::erase<It,0>`):
    # drop the last component's own argument list; the class's stays.
    parts = split_scope(n)
    last = parts[-1]
    if last.endswith(">") and "<" in last and not last.startswith("<lambda"):
        depth, cut = 0, None
        for i in range(len(last) - 1, -1, -1):
            if last[i] == ">":
                depth += 1
            elif last[i] == "<":
                depth -= 1
                if depth == 0:
                    cut = i
                    break
        if cut:
            parts[-1] = last[:cut]
    return "::".join(parts)


# A symbol whose NAME exists at two or more addresses in one image (two TUs'
# file-local twins: LogApmfRefusal, g_lastApmfRefusal ...) is TAGGED with its
# module, "name\x1fmodule", so a reference that re-binds to the other TU's twin
# is visible. Tags compare through the old->new TU map (--tu-map).
TAG = "\x1f"


def untag(name):
    return name.split(TAG, 1)[0]


def tag_of(name):
    return name.split(TAG, 1)[1] if TAG in name else None


def module_key(path):
    """`...\\CMakeFiles\\MFO.dir\\cast\\Fire.cpp.obj` -> `cast/Fire.cpp`."""
    p = path.replace("\\", "/")
    if "/MFO.dir/" in p:
        p = p.split("/MFO.dir/", 1)[1]
    else:
        p = p.rsplit("/", 1)[-1]
    return p[:-4] if p.endswith(".obj") else p


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
        self.procs = []           # dicts: name, sig, rva, size, key, mod
        self.names_at = defaultdict(set)  # rva -> set(names)  (procs, data, publics)
        self.proc_at = {}         # rva -> size (for containment)
        self.datas = []           # (name, rva, module) of named variables/constants (module streams)
        self.inl_ranges = {}      # (name, rva) -> [(start, end, inlinee qualified name)] top-level sites
        self.inl_all = {}         # (name, rva) -> Counter(inlinee qualified names, all depths)
        self.inl_ranges_all = {}  # (name, rva) -> [(start, end, inlinee)] sites at ALL depths
        self.inlinees = Counter()  # every inlinee qualified key anywhere in the image
        self.inl_name = {}        # IPI id -> qualified function name
        self.tpi = {}             # TPI type index -> (kind, record text) for type_size()
        self.data_size = {}       # rva -> byte size of a named variable (from its PDB type)
        self.data_const = {}      # rva -> its PDB type is const-qualified (arrays: the element)
        self.constants = {}       # S_CONSTANT name -> value (constexprs folded out of storage)
        self._load_ids(pdb)
        self._load_pdb(pdb)
        self.sym_rvas = sorted(self.names_at)
        self.proc_starts = sorted(self.proc_at)

    def _load_ids(self, pdb):
        """Qualified names of every function id an inline site can name
        (LF_FUNC_ID: scope string + name; LF_MFUNC_ID: class name + name)."""
        out = subprocess.run(["llvm-pdbutil", "dump", "-ids", "-types", pdb],
                             check=True, capture_output=True, text=True, errors="replace").stdout
        rec = re.compile(r"^\s*(0x[0-9A-F]+) \| (LF_[A-Z_]+) \[size = \d+\](?: `(.*)`)?(.*)$")
        fn = re.compile(r"^\s*name = (.*), type = (?:0x[0-9A-F]+|<no type>), "
                        r"(parent scope|class type) = (0x[0-9A-F]+|<no type>)\s*$")
        strs, udts, funcs = {}, {}, {}
        lines = out.splitlines()
        in_tpi = True
        for i, ln in enumerate(lines):
            if re.match(r"^\s*Types \(IPI Stream\)", ln):
                in_tpi = False
            m = rec.match(ln)
            if not m:
                continue
            tid, kind = int(m.group(1), 16), m.group(2)
            if in_tpi:
                self.tpi[tid] = (kind, " ".join(lines[i:i + 4]))
            if kind == "LF_STRING_ID":
                s = re.search(r"String: (.*)$", ln)
                if s:
                    strs[tid] = s.group(1)
            elif kind in ("LF_CLASS", "LF_STRUCTURE", "LF_UNION", "LF_ENUM", "LF_INTERFACE") and m.group(3):
                udts.setdefault(tid, m.group(3))
            elif kind in ("LF_FUNC_ID", "LF_MFUNC_ID") and i + 1 < len(lines):
                f = fn.match(lines[i + 1])
                if f:
                    funcs[tid] = (kind, f.group(1), f.group(2),
                                  int(f.group(3), 16) if f.group(3).startswith("0x") else None)
        # ids and types are separate streams whose indices overlap: the dump
        # prints TPI first, then IPI, so a later IPI record of the same index
        # must not be confused with a TPI class -- keep them in separate maps
        # (udts only from LF_CLASS-like kinds, strs/funcs only from ID kinds).
        for tid, (kind, name, rel, scope) in funcs.items():
            if kind == "LF_FUNC_ID":
                q = strs.get(scope, "") if scope is not None else ""
                q = re.sub(r"\?A0x[0-9a-f]{8}", "`anonymous namespace'", q)
                full = (q + "::" + name) if q else name
            else:
                cls = udts.get(scope, "?")
                last = split_scope(cls)[-1]
                if name == "{ctor}":
                    name = last
                elif name == "{dtor}":
                    name = "~" + last
                full = cls + "::" + name
            self.inl_name[tid] = full

    PRIM_SIZE = {0x03: 0, 0x10: 1, 0x20: 1, 0x68: 1, 0x69: 1, 0x70: 1, 0x7C: 1, 0x71: 2, 0x7A: 2, 0x7B: 4,
                 0x30: 1, 0x11: 2, 0x21: 2, 0x72: 2, 0x73: 2, 0x12: 4, 0x22: 4, 0x74: 4, 0x75: 4,
                 0x13: 8, 0x23: 8, 0x76: 8, 0x77: 8, 0x40: 4, 0x41: 8}

    def type_size(self, ti, depth=0):
        """Byte size of TPI type ti, or None when it cannot be told (then the
        named-data compare falls back to "up to the next symbol")."""
        if depth > 8:
            return None
        if ti < 0x1000:
            if (ti >> 8) & 0xF:
                return 8                          # a 64-bit pointer mode
            return self.PRIM_SIZE.get(ti & 0xFF)
        r = self.tpi.get(ti)
        if not r:
            return None
        kind, txt = r
        if kind == "LF_MODIFIER":
            m = re.search(r"referent = (0x[0-9A-F]+)", txt)
            return self.type_size(int(m.group(1), 16), depth + 1) if m else None
        if kind == "LF_POINTER":
            return 8
        if kind == "LF_ARRAY":
            m = re.search(r"size: (\d+)", txt)
            return int(m.group(1)) if m else None
        if kind in ("LF_CLASS", "LF_STRUCTURE", "LF_UNION"):
            m = re.search(r"forward ref \(-> (0x[0-9A-F]+)\)", txt)
            if m:
                return self.type_size(int(m.group(1), 16), depth + 1)
            m = re.search(r"sizeof (\d+)", txt)
            return int(m.group(1)) if m and int(m.group(1)) > 0 else None
        if kind == "LF_ENUM":
            m = re.search(r"underlying type: (0x[0-9A-F]+)", txt)
            return self.type_size(int(m.group(1), 16), depth + 1) if m else None
        return None

    def type_const(self, ti, depth=0):
        """TPI type ti is const-qualified (an array: its element type)."""
        if ti < 0x1000 or depth > 8:
            return False
        kind, txt = self.tpi.get(ti, ("", ""))
        if kind == "LF_MODIFIER":
            return bool(re.search(r"modifiers = [^\n]*\bconst\b", txt))
        if kind == "LF_ARRAY":
            m = re.search(r"element type: (0x[0-9A-F]+)", txt)
            return bool(m) and self.type_const(int(m.group(1), 16), depth + 1)
        return False

    def is_const_data(self, rva):
        """A variable that cannot change after initialisation: const-qualified
        by its PDB type (arrays by element), or placed in a read-only section.
        Both are needed: MSVC records a constexpr CLASS-typed variable
        (std::chrono::seconds kTravelFailCooldown) with the plain class type,
        no const modifier, but places it in .rdata."""
        return bool(self.data_const.get(rva)) or self.readonly_data(rva)

    def xtor_var_const(self, name):
        """For `dynamic initializer/atexit destructor for 'X'`: are ALL of X's
        definitions in this image const? None when name is not such an XTOR or
        X has no data record."""
        m = re.search(r"dynamic (?:initializer|atexit destructor) for '(.+?)''", name)
        if not m:
            return None
        v = norm(m.group(1))
        rv = [r for n, r, _m in self.datas if norm(n) == v or norm(n).endswith("::" + v)]
        if not rv:
            return None
        return all(self.is_const_data(r) for r in rv)

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
        modre = re.compile(r"^\s*Mod \d+ \| `(.*)`")
        secre = re.compile(r"^\s*(Global Symbols|Public Symbols)\s*$")
        addr = re.compile(r"addr = (\d+):(\d+)")
        csize = re.compile(r"code size = (\d+)")
        typ = re.compile(r"type = `?0x[0-9A-F]+ \((.*)\)`?")
        inl = re.compile(r"inlinee = (0x[0-9A-F]+) \((.*)\), parent = (\d+), end = (\d+)")
        cur_mod = None
        raw_names = []        # (rva, name, module or None)
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
            if key is not None:
                for a, b in ranges:
                    if top:
                        self.inl_ranges.setdefault(key, []).append((a, b, nm))
                    self.inl_ranges_all.setdefault(key, []).append((a, b, nm))
            cur_site = None

        for i in range(n):
            line = lines[i]
            mm = modre.match(line)
            if mm:
                close_site()
                cur_proc = None
                sites = {}
                cur_mod = module_key(mm.group(1))
                continue
            if secre.match(line):
                close_site()
                cur_proc = None
                cur_mod = None        # globals / publics: no owning module
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
                    nm = qual_key(self.inl_name.get(int(im.group(1), 16), im.group(2)))
                    parent = int(im.group(3))
                    top = parent == cur_proc[1]
                    self.inl_all.setdefault(cur_proc[0], Counter())[nm] += 1
                    self.inlinees[nm] += 1
                    cur_site = [cur_proc[0], top, nm, [], None]
                continue
            if kind == "S_CONSTANT":
                v = re.search(r"value = (-?\d+)", body)
                if v:
                    self.constants.setdefault(norm(name), int(v.group(1)))
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
                self.procs.append({"name": name, "sig": sig, "rva": rva, "size": size, "key": key,
                                   "mod": cur_mod})
                raw_names.append((rva, name, cur_mod))
                self.proc_at[rva] = max(self.proc_at.get(rva, 0), size)
            elif kind in ("S_GDATA32", "S_LDATA32", "S_GTHREAD32", "S_LTHREAD32", "S_PUB32"):
                raw_names.append((rva, name, cur_mod))
                if kind in ("S_GDATA32", "S_LDATA32"):
                    self.datas.append((name, rva, cur_mod))
                    ti = re.search(r"type = (0x[0-9A-F]+)", body)
                    sz = self.type_size(int(ti.group(1), 16)) if ti else None
                    if sz:
                        self.data_size[rva] = sz
                    if ti:
                        self.data_const[rva] = self.type_const(int(ti.group(1), 16))
        close_site()
        # TWINS: a name at two or more addresses (file-local symbols of the same
        # name in different TUs -- the PDB drops "anonymous namespace" from DATA
        # names entirely, so g_lastApmfRefusal's two twins look identical). Tag
        # every module-owned occurrence of such a name with its module and drop
        # the untagged globals/publics copy, so a reference can only match the
        # twin of the corresponding TU.
        at = defaultdict(set)
        for rva, name, mod in raw_names:
            at[name].add(rva)
        self.twins = {n for n, r in at.items() if len(r) > 1}
        # the owning module of every UNIQUE (non-twin) name, so a twin on one
        # side can still be TU-checked against a unique symbol on the other
        self.mod_of = {}
        for rva, name, mod in raw_names:
            if mod is not None and name not in self.twins:
                self.mod_of.setdefault(name, mod)
        owned = {(rva, name) for rva, name, mod in raw_names if mod is not None and name in self.twins}
        for rva, name, mod in raw_names:
            if name in self.twins:
                if mod is not None:
                    self.names_at[rva].add(name + TAG + mod)
                elif (rva, name) not in owned:
                    self.names_at[rva].add(name)       # a twin with no module record
            else:
                self.names_at[rva].add(name)
        for p in self.procs:
            p["twin"] = p["name"] in self.twins
        # a variable listed by its module AND by the globals stream is one
        # definition: keep the module-owned record
        owned_at = {(n, r) for n, r, m in self.datas if m is not None}
        self.datas = [(n, r, m) for n, r, m in self.datas if m is not None or (n, r) not in owned_at]
        self.datas = list(dict.fromkeys(self.datas))

    def read(self, rva, n):
        return bytes(self.data[rva:rva + n])

    def sec_bounds(self, rva):
        for _i, n, va, sz in self.secs:
            if va <= rva < va + sz:
                return va, va + sz
        return rva, rva

    def cstring(self, rva):
        """The whole NUL-terminated byte string at rva (no length cap: it ends
        at its NUL or at the end of its section)."""
        _lo, hi = self.sec_bounds(rva)
        b = self.read(rva, hi - rva)
        z = b.find(b"\0")
        return b if z < 0 else b[:z]

    def readonly_data(self, rva):
        """rva lies in a section that is neither code nor writable (.rdata...)."""
        for s in self.pe.sections:
            if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
                ch = s.Characteristics
                return not (ch & 0x20000000) and not (ch & 0x80000000) and not (ch & 0x20)
        return False

    _bounds = None
    _sbounds = None
    _lbounds = None

    def boundaries(self):
        """Every address that can START an object: each symbol, each DIR64
        relocation slot and relocation target, each RIP-relative target in the
        code (PDB functions and the gaps between them), each pooled text
        literal. Unnamed read-only data runs from its address to the next one."""
        if self._bounds is None:
            b = set(self.sym_rvas)
            b |= self.relocs           # a relocated pointer slot starts a pointer
            for r in self.relocs:
                v = int.from_bytes(self.data[r:r + 8], "little") - self.base
                if 0 < v < len(self.data):
                    b.add(v)
            md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
            md.detail = True
            md.skipdata = True

            def sweep(lo, hi):
                for ins in md.disasm(self.read(lo, hi - lo), lo):
                    if ins.id == 0:          # skipped data byte
                        continue
                    for op in ins.operands:
                        if op.type == X.X86_OP_MEM and op.mem.base == X.X86_REG_RIP:
                            b.add(ins.address + ins.size + op.mem.disp)
            # every PDB function, then the GAPS between them: code with no PDB
            # record (prebuilt libraries without debug info: fmt's "inf"/"nan"
            # literals are referenced only from there)
            covered = 0
            for s in self.pe.sections:
                if not s.Characteristics & 0x20000000:
                    continue
                lo, hi = s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize
                cur = lo
                for st in self.proc_starts:
                    if st < lo or st >= hi:
                        continue
                    if st > cur:
                        sweep(cur, st)
                    end = st + self.proc_at[st]
                    sweep(st, end)
                    covered += 1
                    cur = max(cur, end)
                if cur < hi:
                    sweep(cur, hi)
            for _i, _n, va, sz in self.secs:
                b.add(va + sz)
            # STRUCTURAL boundaries end here: they come from how the image is
            # linked and referenced, so no data byte can create or remove one
            self._sbounds = sorted(b)
            # the start of every pooled TEXT literal in read-only data: /Od keeps
            # a TU's whole literal pool, referenced or not, so an unreferenced
            # literal (from an inline function this TU never emitted) can sit
            # right after a referenced object; it is an object of its own. These
            # are CONTENT-derived (a changed byte can make or break one), so the
            # compare only ever cuts at one found at the same offset in BOTH builds
            lit = set()
            for s in self.pe.sections:
                ch = s.Characteristics
                if ch & 0x20000000 or ch & 0x80000000 or ch & 0x20:
                    continue
                lo = s.VirtualAddress
                raw = self.read(lo, s.Misc_VirtualSize)
                for m in re.finditer(rb"(?<=\x00)[^\x00]{2,}(?=\x00)", raw):
                    if is_text(m.group(0)):
                        lit.add(lo + m.start())
            self._lbounds = lit
            self._bounds = sorted(b | lit)
        return self._bounds

    _bset = None

    def boundary_set(self):
        if self._bset is None:
            self._bset = set(self.boundaries())
        return self._bset

    def struct_extent(self, rva):
        """Bytes from rva to the next STRUCTURAL boundary (symbol, relocation
        slot or target, code reference, section end) -- never content-derived."""
        self.boundaries()
        bs = self._sbounds
        j = bisect.bisect_right(bs, rva)
        _lo, hi = self.sec_bounds(rva)
        nxt = bs[j] if j < len(bs) else hi
        return max(0, min(nxt, hi) - rva)

    def extent(self, rva):
        """Bytes from rva to the next object boundary (see boundaries())."""
        bs = self.boundaries()
        j = bisect.bisect_right(bs, rva)
        _lo, hi = self.sec_bounds(rva)
        nxt = bs[j] if j < len(bs) else hi
        return max(0, min(nxt, hi) - rva)

    def type_descriptor_name(self, rva):
        """Decorated class name if rva is an RTTI TypeDescriptor
        ({pVFTable, spare, char name[]}), else None."""
        if rva not in self.relocs:
            return None
        s = self.cstring(rva + 16)
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
        self.tu_map = {}        # old module -> set(new modules); filled by main (--tu-map)
        # set ONLY while comparing per-TU COPIES of a header object's initializer:
        # each copy refers to its OWN TU's twin, so any TU's twin is the same shape
        self.own_tu_twins = False
        self._hcache = {}
        self._ue_active = set()
        self._live = {}

    def copy_readers(self, img, var):
        """Who touches each definition (copy) of variable `var` (norm'd name) in
        img, other than its own initializer / atexit destructor. A reference
        ANYWHERE inside a copy counts ([rip+g+4], an element, a member), the
        copy's extent being its PDB type size (else up to the next structural
        boundary). Readers are keyed by (function name, owning MODULE), so
        per-TU copies of a same-named file-local accessor stay distinct; a
        relocated pointer to a copy is the reader ("data:<symbol>", module).
        Returns {(reader, module): set(copy rvas)}."""
        key = (id(img), var)
        if key in self._live:
            return self._live[key]
        spans = sorted((r, r + (img.data_size.get(r) or max(1, img.struct_extent(r))))
                       for n, r, _m in img.datas if norm(n) == var)
        starts = [a for a, _b in spans]

        def copy_of(t):
            j = bisect.bisect_right(starts, t) - 1
            return spans[j][0] if j >= 0 and spans[j][0] <= t < spans[j][1] else None
        readers = defaultdict(set)
        own = re.compile(r"dynamic (?:initializer|atexit destructor) for '" + re.escape(var.rsplit("::", 1)[-1]) + "''")
        for p in img.procs:
            if own.search(p["name"]):
                continue
            for ins in self.disasm(img, p["rva"], p["size"]):
                for _o, _s, _k, t in self.fields(img, ins):
                    c = copy_of(t)
                    if c is not None:
                        readers[(norm(untag(p["name"])), p.get("mod"))].add(c)
        for r in img.relocs:
            v = int.from_bytes(img.data[r:r + 8], "little") - img.base
            c = copy_of(v)
            if c is not None:
                sym = img.resolve(r)[0]
                nm = untag(min(sym[1])) if sym[1] else hex(r)
                readers[("data:" + norm(nm), img.mod_of.get(nm))].add(c)
        self._live[key] = dict(readers)
        return self._live[key]

    def state_split_ok(self, var, layout_changed=True):
        """A MUTABLE variable with per-TU copies is still the same state only if
        the split kept who shares a copy with whom. Every reader in B (keyed by
        name + module) must map to exactly one reader in A of the same name in
        a TU its TU came from (--tu-map), every A reader must be mapped, and for
        every two B readers "they touch a common copy" must equal the same fact
        for their A readers -- two B readers that map to ONE A reader (per-TU
        copies of one accessor) must therefore share a copy. When the copy
        layout changed, finding NO reader at all is a FAIL (nothing proves the
        state is unsplit). Returns (ok, detail)."""
        ra, rb = self.copy_readers(self.A, var), self.copy_readers(self.B, var)
        if not ra and not rb:
            if layout_changed:
                return False, "no reader of any copy was found, so no sharing can be proven"
            return True, "no readers"
        # The SPLIT's TUs: the old ones (--tu-map keys) and what they became.
        # A reader in any other TU is outside the split; when it is an inline /
        # template COMDAT (RE::TESForm::As<...>) the linker keeps ONE TU's
        # instance, and which TU's -- hence which per-TU copy it reads -- can
        # change between two links of unchanged source. Such readers match by
        # name alone, and a pair of two of them is not judged: only a pair with
        # at least one reader inside the split can show a split the split made.
        split_a = set(self.tu_map)
        split_b = {m for v in self.tu_map.values() for m in v}
        mapping, why = {}, []
        for kb in rb:
            if kb[1] in split_b:
                c = [ka for ka in ra if ka[0] == kb[0] and self.tu_ok(ka[1], kb[1])]
            else:
                c = [ka for ka in ra if ka[0] == kb[0] and ka[1] not in split_a]
                c = c[:1]
            if len(c) != 1:
                why.append(f"B reader {kb[0][:60]} ({kb[1]}) maps to {len(c)} A reader(s)")
            else:
                mapping[kb] = c[0]
        unmapped = [ka for ka in ra if ka not in mapping.values()
                    and (ka[1] in split_a or not any(kb[0] == ka[0] for kb in rb))]
        if unmapped:
            why.append(f"A reader(s) with no B counterpart: {[k[0][:50] for k in unmapped[:3]]}")
        if why:
            return False, "; ".join(why[:3])
        kbs = sorted(rb, key=str)
        for x in range(len(kbs)):
            for y in range(x + 1, len(kbs)):
                p, q = kbs[x], kbs[y]
                if p[1] not in split_b and q[1] not in split_b:
                    continue
                sa = mapping[p] == mapping[q] or bool(ra[mapping[p]] & ra[mapping[q]])
                sb = bool(rb[p] & rb[q])
                if sa != sb:
                    return False, (f"{p[0][:50]} ({p[1]}) and {q[0][:50]} ({q[1]}) "
                                   f"{'share' if sa else 'do not share'} a copy in A but "
                                   f"{'share' if sb else 'do not share'} one in B")
        return True, f"{len(rb)} reader(s), sharing unchanged"

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

    def tu_ok(self, mod_a, mod_b):
        """May a symbol owned by TU mod_a in build A be the same one owned by
        TU mod_b in build B? Identity, or listed in the old->new TU map."""
        if mod_a is None or mod_b is None:
            return True
        return mod_b == mod_a or mod_b in self.tu_map.get(mod_a, ())

    def names_match(self, na, nb):
        """Two target name sets denote the same entity: some name in each is the
        same after dropping the anonymous namespace, and when BOTH are module-
        tagged twins, B's module is one A's module became (--tu-map). A twin on
        one side against a now-unique symbol on the other (the split made it
        extern / removed the other twin) matches by name and is counted."""
        for x in na:
            bx, mx = untag(x), tag_of(x)
            nx = norm(bx)
            for y in nb:
                by, my = untag(y), tag_of(y)
                if norm(by) != nx:
                    continue
                if mx is not None and my is not None:
                    if self.tu_ok(mx, my) or self.own_tu_twins:
                        return True
                    continue
                if (mx is None) != (my is None):
                    # a twin on one side, a now-unique symbol on the other (the
                    # split made it extern / left one copy): the unique one's
                    # owning module must still be one the twin's TU became
                    ma = mx if mx is not None else self.A.mod_of.get(bx)
                    mb = my if my is not None else self.B.mod_of.get(by)
                    if ma is not None and mb is not None:
                        if not (self.tu_ok(ma, mb) or self.own_tu_twins):
                            continue
                        self.notes["twin on one side, unique on the other: same TU lineage"] += 1
                    else:
                        self.notes["twin on one side, unique on the other: unique one has no module record"] += 1
                return True
        return False

    def _unnamed_equal(self, ra, rb):
        """Unnamed read-only data. A string literal (UTF-8 accepted) is compared
        as its whole NUL-terminated byte string; a source-path literal may
        change its file name. Anything else is compared over min(extent in A,
        extent in B), the extent running to the next object boundary: the
        object ends before the next object starts in EACH build, so the shorter
        extent still covers all of it (the longer one's excess is a different,
        e.g. unreferenced, object). Relocated pointer slots compare by target."""
        sa, sb = self.A.cstring(ra), self.B.cstring(rb)
        if sa != sb and PATHLIT.search(sa) and PATHLIT.search(sb):
            self.path_pairs.add((sa.decode(errors="replace"), sb.decode(errors="replace")))
            self.notes["source-path string literal moved file"] += 1
            return True
        if is_text(sa, 1) or is_text(sb, 1):
            # a STRING LITERAL (UTF-8 accepted): the object is the whole
            # NUL-terminated byte string, compared in full
            if sa == sb:
                self.notes["unnamed string literal equal in full"] += 1
                return True
            return False
        if (ra, rb) in self._ue_active:        # a pointer cycle: assume, then verify the rest
            return True
        self._ue_active.add((ra, rb))
        try:
            return self._unnamed_equal_body(ra, rb, empty=(sa == sb == b""))
        finally:
            self._ue_active.discard((ra, rb))

    def _unnamed_equal_body(self, ra, rb, empty=False):
        # The object ends at the first boundary present at the SAME relative
        # offset in BOTH builds. A boundary only one build has (a content-derived
        # pooled-literal start, a reference only one side makes) is compared
        # THROUGH: a changed byte must never be able to end its own object early
        # by creating or removing a boundary in one build.
        # ...and never past the first STRUCTURAL boundary of either build (those
        # no data byte can move): the object ends before the next object starts
        # in each build, so the shorter structural extent still covers it.
        ba_set, bb_set = self.A.boundary_set(), self.B.boundary_set()
        lim = min(self.A.struct_extent(ra), self.B.struct_extent(rb))
        if lim <= 0:
            return False
        k = 0
        while k < lim:
            if k and (ra + k) in ba_set and (rb + k) in bb_set:
                break
            if empty and k and ((ra + k) in self.A._lbounds or (rb + k) in self.B._lbounds):
                # "" (the object starts with its NUL in BOTH builds): the text
                # literal that follows in one build only is another, unreferenced
                # literal. RESIDUAL (backlog): a non-string object whose first
                # byte is 0 in both builds is read as "" here.
                break
            pa, pb = (ra + k) in self.A.relocs, (rb + k) in self.B.relocs
            if pa or pb:
                if not (pa and pb) or k + 8 > lim:
                    return False
                va = int.from_bytes(self.A.read(ra + k, 8), "little") - self.A.base
                vb = int.from_bytes(self.B.read(rb + k, 8), "little") - self.B.base
                if not self.same_target(self.A.resolve(va), self.B.resolve(vb), 3):
                    return False
                k += 8
                continue
            if self.A.data[ra + k] != self.B.data[rb + k]:
                return False
            k += 1
        self.notes["unnamed read-only data equal up to the first common boundary"] += 1
        return True

    def _same(self, ta, tb, depth):
        ka, na, oa, ra = ta
        kb, nb, ob, rb = tb
        if ka in ("local", "hdr", "rtti") or kb in ("local", "hdr", "rtti"):
            return ka == kb and oa == ob and (na == nb) and (ka != "hdr" or ra == rb)
        # READ-ONLY DATA (a section neither code nor writable). A target that is
        # a named object's start, or lies inside a named object of known size,
        # is compared by name (named data is byte-compared by compare_data).
        # Anything else is UNNAMED read-only data -- /Od string literals (no
        # ??_C symbols), template statics such as _Hash::_Min_buckets, a constant
        # named in one build only -- and is compared by CONTENT over its whole
        # extent: from the target to the next object boundary (a symbol, a
        # relocation target, a code reference), never a fixed length.
        if ka in ("sym", "none") and kb in ("sym", "none") \
                and self.A.readonly_data(ra) and self.B.readonly_data(rb):
            if ka == kb == "sym" and oa == ob and na and nb and self.names_match(na, nb):
                za = self.A.data_size.get(ra - oa)
                is_lit = any(x.startswith("??_C@") for x in na | nb)
                if oa == 0 and not is_lit:
                    if na != nb:
                        self.notes["target name-set differs only by ICF alias / anon namespace / TU of a twin"] += 1
                    return True
                if za and oa <= za:          # inside it, or one past its end (a loop bound)
                    return True
            return self._unnamed_equal(ra, rb)
        if ka == "none" or kb == "none":
            return False
        if oa == ob and na and nb and self.names_match(na, nb):
            if na != nb:
                self.notes["target name-set differs only by ICF alias / anon namespace / TU of a twin"] += 1
            return True
        # (string literals, named ??_C or unnamed, are read-only data: handled
        # above by _unnamed_equal over their whole extent)
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
        spans = self.own_spans(img, f)
        tbl = self.tables(img, f)
        end = tbl[0] if tbl else f["size"]
        imms = Counter()
        seq = self.disasm(img, f["rva"], end)
        for n_ins, ins in enumerate(seq):
            off = ins.address - f["rva"]
            inside = any(a <= off < b for a, b, _n in spans)
            if not inside or self.in_mfo_inlinee(img, f, off):
                for v in self.own_imm(img, ins, seq[n_ins + 1] if n_ins + 1 < len(seq) else None):
                    imms[v] += 1
            for _o, _s, kind, t in self.fields(img, ins):
                cands = self.toks(img, f, kind, t)
                if cands[0][0] == "local":
                    continue
                tok = ("ref", img, tuple(cands))
                every.append(("iref", img, tuple(cands)) if inside else tok)
                if not inside:
                    own.append(tok)
        for nm in sorted({n for _a, _b, n in ranges}):
            own.append(("inl", None, nm))
        for nm in img.inl_all.get(f["key"], {}):
            every.append(("inl", None, nm))
        return own, every, imms

    def in_mfo_inlinee(self, img, f, off):
        """True when the innermost inline site covering f+off is MFO code (a
        lambda body inlined through std::invoke into its std::function wrapper,
        an inlined MFO helper). Constants there are counted like own code; only
        LIBRARY inlinees are left to the inline-site comparison."""
        cover = [(b - a, n) for a, b, n in img.inl_ranges_all.get(f["key"], []) if a <= off < b]
        return bool(cover) and not library_only(min(cover)[1])

    def own_spans(self, img, f):
        """f's top-level inline-site byte spans, function-relative (a, b, inlinee)."""
        ranges = img.inl_ranges.get(f["key"], [])
        # A site's sub-ranges can leave a one-instruction gap (MSVC attributes the
        # call instruction an inlined body makes to the caller's line table): fill
        # gaps of <= 16 bytes between sub-ranges of the SAME site.
        spans = []
        by_site = defaultdict(list)
        for a, b, n in ranges:
            by_site[n].append((a, b))
        for n, rs in by_site.items():
            rs.sort()
            cur_a, cur_b = rs[0]
            for a, b in rs[1:]:
                if a - cur_b <= 16:
                    cur_b = max(cur_b, b)
                else:
                    spans.append((cur_a, cur_b, n)); cur_a, cur_b = a, b
            spans.append((cur_a, cur_b, n))
        return spans

    FRAME_REGS = {X.X86_REG_RSP, X.X86_REG_RBP, X.X86_REG_ESP, X.X86_REG_EBP}

    def own_imm(self, img, ins, nxt):
        """The CONSTANTS an instruction carries as immediate operands, excluding
        what legitimately moves when inlining changes: frame-size arithmetic on
        rsp/rbp, the __chkstk size, branch/call targets and relocated addresses
        (those are compared by symbol already)."""
        if ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL) or \
                ins.group(capstone.CS_GRP_RET):
            return []
        if any(f[2] == "abs" for f in self.fields(img, ins)):
            return []
        ops = ins.operands
        if ops and ops[0].type == X.X86_OP_REG and ops[0].reg in self.FRAME_REGS:
            return []
        if ins.mnemonic == "mov" and nxt is not None and nxt.group(capstone.CS_GRP_CALL):
            tgt = [t for _o, _s, k, t in self.fields(img, nxt) if k == "rel"]
            if tgt and any("chkstk" in n for c in img.resolve(tgt[0]) for n in c[1]):
                return []
        return [(ins.mnemonic, op.imm) for op in ops if op.type == X.X86_OP_IMM]

    def helper_imms(self, img, helpers, depth=3):
        """Every (mnemonic, constant) in the out-of-line bodies (in img) of the
        given helpers, following their direct calls `depth` levels."""
        key = (id(img), tuple(sorted(helpers)), depth)
        if key in self._hcache:
            return self._hcache[key]
        pool = Counter()
        todo = [p for p in img.procs if qual_key(p["name"]) in set(helpers)]
        seen = set()
        for _lvl in range(depth):
            nxt = []
            for p in todo:
                if p["rva"] in seen:
                    continue
                seen.add(p["rva"])
                tbl = self.tables(img, p)
                seq = self.disasm(img, p["rva"], tbl[0] if tbl else p["size"])
                for n_ins, ins in enumerate(seq):
                    for v in self.own_imm(img, ins, seq[n_ins + 1] if n_ins + 1 < len(seq) else None):
                        pool[v] += 1
                    if ins.group(capstone.CS_GRP_CALL):
                        for _o, _s, k, t in self.fields(img, ins):
                            if k == "rel" and t in img.proc_at:
                                nxt.append({"rva": t, "size": img.proc_at[t], "name": "", "key": None})
            todo = nxt
        self._hcache[key] = pool
        return pool

    def fp_equal(self, fa, fb):
        """Modulo-inlining equivalence (SET semantics): every own-code token of
        either side has an equivalent token ANYWHERE in the other side, where
        "inlined X" is equivalent to "calls X". Returns the unmatched tokens."""
        oa, ea, ima = self.fingerprint(self.A, fa)
        ob, eb, imb = self.fingerprint(self.B, fb)
        imm_un = []
        if ima != imb:
            # MULTISET of own-code constants. A constant only one side carries is
            # accepted ONLY when it can be attributed to the inlining difference:
            # this side inlined helper H more often than the other side, and the
            # other side's out-of-line body of H (or of a function H's body calls,
            # 3 levels) contains that very constant. Anything else -- a changed
            # constant in a drift function -- is unmatched and FAILs the pair.
            ia = self.A.inl_all.get(fa["key"], Counter()) if fa.get("key") else Counter()
            ib = self.B.inl_all.get(fb["key"], Counter()) if fb.get("key") else Counter()
            # helpers whose inline count differs between the builds; their
            # out-of-line bodies are looked up in BOTH images (a helper inlined
            # everywhere in one build still has its out-of-line COMDAT in the other)
            for side, extra, mine_inl, other_inl in (("A", ima - imb, ia, ib), ("B", imb - ima, ib, ia)):
                # a constant only THIS side carries may come from a helper THIS side
                # inlined more often; its out-of-line body is looked up in both
                # images (a helper inlined everywhere in one build still has its
                # COMDAT body in the other)
                helpers = sorted(h for h in mine_inl if mine_inl[h] > other_inl.get(h, 0))
                pool = self.helper_imms(self.A, helpers) + self.helper_imms(self.B, helpers)
                for (mn, v), c in sorted(extra.items(), key=str):
                    if pool[(mn, v)] >= 1:
                        self.notes["own-code constant attributed to a helper inlined on one side only"] += c
                        continue
                    imm_un.append((side, ("imm", None, f"{mn} {v:#x} x{c}")))
        if XTOR.search(fa["name"]):
            # a compiler-generated initializer / atexit destructor only builds or
            # destroys its ONE named variable; whether the member destructor is
            # inlined decides which member OFFSET the code touches, so compare the
            # variable referenced, not the offset into it
            def loose(toks):
                return [(t[0], t[1], tuple((c[0], c[1], 0, c[3]) if c[0] == "sym" else c for c in t[2]))
                        if t[0] in ("ref", "iref") else t for t in toks]
            oa, ea, ob, eb = loose(oa), loose(ea), loose(ob), loose(eb)
        un = list(imm_un)
        for side, mine, theirs in (("A", oa, eb), ("B", ob, ea)):
            for x in mine:
                if not any(self._fp_tok_eq(x, y) for y in theirs):
                    un.append((side, x))
        return un

    def _fp_tok_eq(self, x, y):
        if x[0] == "inl" and y[0] == "inl":
            return x[2] == y[2]
        if x[0] in ("ref", "iref") and y[0] in ("ref", "iref"):
            a, b = (x, y) if x[1] is self.A else (y, x)
            if self.same_target(list(a[2]), list(b[2]), 1):
                return True
            if "iref" in (x[0], y[0]):
                # the other side reaches this object from INSIDE an inlined body
                # (e.g. an inlined unordered_map::erase touching g_owned's members
                # where this side passes &g_owned to the out-of-line call): same
                # object, member offset free
                la = [(c[0], c[1], 0, c[3]) if c[0] == "sym" else c for c in a[2]]
                lb = [(c[0], c[1], 0, c[3]) if c[0] == "sym" else c for c in b[2]]
                return self.same_target(la, lb, 1)
            return False
        # "inlined X" == "calls X"
        inl, ref = (x, y) if x[0] == "inl" else (y, x)
        if ref[0] == "inl":
            return False
        c = ref[2][0]
        return c[0] == "proc" and c[2] == 0 and inl[2] in {qual_key(n) for n in c[1]}


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
        return f"{side}: inlined {tok[2][:160]}"
    if tok[0] == "imm":
        return f"{side}: own-code constant {tok[2]}"
    return f"{side}: ref {fmt(tok[2][0])}"


# --------------------------------------------------------------------------
# named data
# --------------------------------------------------------------------------
def compare_data(A, B, cmp, cap=512):
    """Initial contents of every named MFO variable/constant (module streams).
    Extent = the PDB type size, else the distance to the next symbol (no cap);
    DIR64 pointer slots are compared by the symbol they point to. Twins (one name
    defined in several TUs) are paired through the TU map. Returns
    (checked, diffs, only_a, only_b, folded): a name present in only one build is
    reported -- it is FOLDED (explained) only when the other build carries it as
    an S_CONSTANT whose value equals the first build's stored bytes."""
    da, db = defaultdict(list), defaultdict(list)
    for n, r, m in A.datas:
        if (r, m) not in da[norm(n)]:
            da[norm(n)].append((r, m))
    for n, r, m in B.datas:
        if (r, m) not in db[norm(n)]:
            db[norm(n)].append((r, m))
    in_scope = lambda n: DATA_SCOPE.match(n) and "`RTTI" not in n
    diffs, checked, only_a, only_b, folded, copies = [], 0, [], [], [], []

    def const_equal(img_with, rva, img_const, n):
        v = img_const.constants.get(n)
        if v is None:
            return False
        raw = img_with.read(rva, 8)
        return any(int.from_bytes(raw[:k], "little", signed=True) == v or
                   int.from_bytes(raw[:k], "little") == v for k in (1, 2, 4, 8))

    for n in sorted(set(da) | set(db)):
        la, lb = da.get(n, []), db.get(n, [])
        # MUTABLE per-TU copies (a twin in either build): the state-split check
        # runs whenever they exist, whether or not the copy layout changed, and
        # for EVERY variable -- a header static of a library (rapidcsv) is split
        # by a TU split exactly like one of MFO's
        if (len(la) > 1 or len(lb) > 1) and la and lb and n and "`" not in n and \
                not any(A.sec_of(r) == ".text" for r, _m in la) and \
                (any(not A.is_const_data(r) for r, _m in la) or any(not B.is_const_data(r) for r, _m in lb)):
            changed = sorted(str(m) for _r, m in la) != sorted(str(m) for _r, m in lb)
            ok_, why_ = cmp.state_split_ok(n, layout_changed=changed)
            if not ok_:
                diffs.append((n, f"STATE SPLIT on a MUTABLE per-TU variable ({len(la)} copies in A, "
                                 f"{len(lb)} in B): {why_}"))
                continue
        if not in_scope(n):
            continue
        if not lb:
            if len(la) == 1 and const_equal(A, la[0][0], B, n):
                folded.append((n, "A stores it, B folded it to an S_CONSTANT of the same value"))
            else:
                only_a.append((n, f"{len(la)} definition(s) in A, none in B"))
            continue
        if not la:
            if len(lb) == 1 and const_equal(B, lb[0][0], A, n):
                folded.append((n, "B stores it, A folded it to an S_CONSTANT of the same value"))
            else:
                only_b.append((n, f"{len(lb)} definition(s) in B, none in A"))
            continue
        # pair definitions: unique on both sides pair directly; twins pair only
        # with a definition in a TU their TU became
        pairs = []
        if len(la) == 1 and len(lb) == 1:
            pairs = [(la[0], lb[0])]
        elif any(sum(1 for x in lb if cmp.tu_ok(ma, x[1])) > 1 for _r, ma in la):
            # PER-TU COPIES of a header constant (one per including TU): a TU that
            # became several TUs has several copies now. Every copy on each side
            # must be byte-identical to a copy on the other side in a mapped TU.
            bad = []
            for rb_, mb in lb:
                if not any(cmp.tu_ok(ma, mb) and _data_diff(A, B, cmp, ra_, rb_, cap) is None for ra_, ma in la):
                    bad.append(f"B copy in {mb} matches no A copy")
            for ra_, ma in la:
                if not any(cmp.tu_ok(ma, mb) and _data_diff(A, B, cmp, ra_, rb_, cap) is None for rb_, mb in lb):
                    bad.append(f"A copy in {ma} matches no B copy")
            checked += 1
            if bad:
                diffs.append((n, "; ".join(bad)))
            elif sorted(str(m) for _r, m in la) != sorted(str(m) for _r, m in lb):
                # the copy layout changed (the split's doing); an unchanged layout
                # with every copy identical is simply identical. Only a CONST
                # object may change its copy count: a mutable one would be STATE
                # the old TU's functions shared and the new TUs now each keep
                # their own of -- a behaviour change no byte compare can see
                mut = [f"A {m}" for r, m in la if not A.is_const_data(r)] + \
                      [f"B {m}" for r, m in lb if not B.is_const_data(r)]
                ok, why_ = cmp.state_split_ok(n) if mut else (True, "")
                if mut and ok:
                    copies.append((n, f"{len(la)} copies in A, {len(lb)} in B, all identical, MUTABLE but "
                                      f"no state split ({why_})"))
                elif mut:
                    diffs.append((n, f"PER-TU COPY COUNT CHANGED ON MUTABLE STATE ({len(la)} copies in A, "
                                     f"{len(lb)} in B; mutable: {', '.join(mut[:4])}): the split gives each new "
                                     f"TU its own copy of state the old TU shared -- {why_}"))
                else:
                    copies.append((n, f"{len(la)} copies in A, {len(lb)} in B, all identical, const"))
            continue
        else:
            free = list(lb)
            for ra_, ma in la:
                cand = [x for x in free if cmp.tu_ok(ma, x[1])]
                if len(cand) != 1:
                    diffs.append((n, f"twin in {ma}: {len(cand)} candidate(s) in B "
                                     f"({', '.join(str(x[1]) for x in cand) or 'none'})"))
                    continue
                pairs.append(((ra_, ma), cand[0]))
                free.remove(cand[0])
            for rb_, mb in free:
                diffs.append((n, f"twin in B's {mb} has no counterpart in A"))
        for (ra, _ma), (rb, _mb) in pairs:
            bad = _data_diff(A, B, cmp, ra, rb, cap)
            checked += 1
            if bad:
                diffs.append((n, bad))
    return checked, diffs, only_a, only_b, folded, copies


def _data_diff(A, B, cmp, ra, rb, cap):
    sa, sb = A.sec_of(ra), B.sec_of(rb)
    if sa != sb or sa in (None, ".text"):
        return f"section {sa} vs {sb}"

    def extent(img, r):
        # the variable's own PDB type size when known (bytes past the object --
        # an unnamed literal the linker placed next to it -- are not it); else
        # up to the next symbol (or its section's end). Never a fixed cap.
        if img.data_size.get(r):
            return img.data_size[r]
        j = bisect.bisect_right(img.sym_rvas, r)
        _lo, hi = img.sec_bounds(r)
        nxt = img.sym_rvas[j] if j < len(img.sym_rvas) else hi
        return min(nxt, hi) - r
    xa, xb = extent(A, ra), extent(B, rb)
    if A.data_size.get(ra) and B.data_size.get(rb) and xa != xb:
        return f"type size {xa} vs {xb}"
    ln = min(xa, xb)
    ba, bb = A.read(ra, ln), B.read(rb, ln)
    k = 0
    while k < ln:
        pa, pb = (ra + k) in A.relocs, (rb + k) in B.relocs
        if pa or pb:
            if not (pa and pb):
                return f"+{k:#x}: pointer slot in one build only"
            va = int.from_bytes(ba[k:k + 8], "little") - A.base
            vb = int.from_bytes(bb[k:k + 8], "little") - B.base
            if not cmp.same_target(A.resolve(va), B.resolve(vb), 0):
                return f"+{k:#x}: pointer A={fmt(A.resolve(va)[0])} B={fmt(B.resolve(vb)[0])}"
            k += 8
            continue
        if ba[k] != bb[k]:
            return f"+{k:#x}: byte {ba[k]:#04x} vs {bb[k]:#04x}"
        k += 1
    return None


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
            compat = lambda fa, fb: not (fa.get("twin") and fb.get("twin")) or cmp.tu_ok(fa["mod"], fb["mod"])
            for fa in list(la):          # identical pairs first
                for fb in lb:
                    if compat(fa, fb) and cmp.compare(fa, fb) is None:
                        got.append((fa, fb))
                        la.remove(fa)
                        lb.remove(fb)
                        break
            la.sort(key=lambda f: f["rva"])
            lb.sort(key=lambda f: f["rva"])
            if len(la) == len(lb) and all(compat(x, y) for x, y in zip(la, lb)):
                got += list(zip(la, lb))   # leftovers pair in address order
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


def read_build_info(dll, explicit=None):
    """The CI build record of a DLL (native.yml "Record the built commit"):
    `built=<sha> noinline=<b> noopt=<b> dll_sha256=<hex>`, from the explicit
    path, else the MFO-build-info artifact next to the DLL's MFO-dll folder
    (`gh run download` layout), else a build-info.txt beside the DLL (older
    proof artifacts). Returns (dict or None, path tried)."""
    import os
    here = os.path.dirname(os.path.abspath(dll))
    cands = [explicit] if explicit else [os.path.join(os.path.dirname(here), "MFO-build-info", "build-info.txt"),
                                         os.path.join(here, "build-info.txt")]
    for c in cands:
        if c and os.path.isfile(c):
            raw = open(c, "rb").read()
            txt = raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else raw.decode("utf-8-sig", "replace")
            return dict(kv.split("=", 1) for kv in txt.split() if "=" in kv), c
    return None, cands[0]


def check_provenance(a_dll, b_dll, a0_dll, b0_dll, explicit=None):
    """--proof is sound only when the proof pair is the SAME two commits as the
    shipped pair: A and A0 built from one SHA, B and B0 from another, A0/B0
    with the optimizer off, A/B the normal build, and each record bound to its
    DLL by SHA-256. Returns the list of violations (empty = paired)."""
    import hashlib
    ex = explicit or [None] * 4
    why, info = [], {}
    for tag, dll, e in (("A", a_dll, ex[0]), ("B", b_dll, ex[1]), ("A0", a0_dll, ex[2]), ("B0", b0_dll, ex[3])):
        d, path = read_build_info(dll, e)
        if d is None:
            why.append(f"{tag}: no build record ({path}); every native run uploads MFO-build-info")
            continue
        info[tag] = d
        if "built" not in d:
            why.append(f"{tag}: build record has no built= SHA ({path})")
        h = d.get("dll_sha256")
        if not h:
            why.append(f"{tag}: build record carries no dll_sha256, so it cannot be bound to {dll}")
        elif hashlib.sha256(open(dll, "rb").read()).hexdigest() != h.lower():
            why.append(f"{tag}: {dll} is not the DLL its build record describes (SHA-256 mismatch)")
    if len(info) == 4:
        for o2, od in (("A", "A0"), ("B", "B0")):
            if info[o2].get("built") != info[od].get("built"):
                why.append(f"{o2} built {info[o2].get('built')} but {od} built {info[od].get('built')}: "
                           f"the proof build is not the same commit")
        for t in ("A0", "B0"):
            if str(info[t].get("noopt")).lower() != "true":
                why.append(f"{t} is not a noopt (/Od) build: noopt={info[t].get('noopt')}")
        for t in ("A", "B"):
            if str(info[t].get("noopt")).lower() != "false" or str(info[t].get("noinline")).lower() != "false":
                why.append(f"{t} is not the normal /O2 build: noopt={info[t].get('noopt')} "
                           f"noinline={info[t].get('noinline')}")
        if info["A"].get("built") == info["B"].get("built"):
            why.append("A and B are the same commit: there is no split to prove")
    return why


def load_tu_map(path):
    """`old/Module.cpp: new/A.cpp new/B.cpp` per line; # comments."""
    tm = {}
    if not path:
        return tm
    for ln in open(path, encoding="utf-8"):
        ln = ln.split("#", 1)[0].strip()
        if not ln:
            continue
        old, new = ln.split(":", 1)
        tm.setdefault(old.strip(), set()).update(new.split())
    return tm


def library_only(name):
    """A library template instance with NO own type in it. Only those may be
    explained by "the linker took another TU's COMDAT" without an inline-site
    difference; anything naming an MFO:: entity (a std::function wrapping an MFO
    lambda, a container of an MFO struct) must show a real inline-site delta."""
    return bool(LIBRARY.match(name)) and "MFO::" not in name


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
    ap.add_argument("--copies-ok", action="store_true",
                    help="with --strict: still accept identical per-TU copies of a header-defined "
                         "internal-linkage object whose copy COUNT changed (what --proof runs)")
    ap.add_argument("--proof", nargs=4, metavar=("A0_DLL", "A0_PDB", "B0_DLL", "B0_PDB"),
                    help="the same two commits built with the optimizer OFF (native.yml dispatch noopt=true, "
                         "/Od; /Ob0 is not enough, see README). They must compare STRICT PASS (--copies-ok); "
                         "then an /O2 pair whose inline sites differ is PROVEN, and the verdict is PASS-PROVEN")
    ap.add_argument("--build-info", nargs=4, metavar=("A", "B", "A0", "B0"),
                    help="the four CI build records (build-info.txt) when they are not in the "
                         "`gh run download` layout next to each DLL; --proof refuses without them")
    ap.add_argument("--tu-map", help="old->new translation-unit map (see tumaps/); a module-tagged "
                                     "twin in A may only match a twin in a TU its TU became")
    args = ap.parse_args()

    if args.proof:
        # PROVENANCE first (cheap, and nothing else is worth running without it)
        why = check_provenance(args.a_dll, args.b_dll, args.proof[0], args.proof[2], args.build_info)
        if why:
            print("PROVENANCE: the four builds do not pair up -- refusing to run --proof:\n  " + "\n  ".join(why))
            print("\nRESULT: FAIL")
            if args.json:
                json.dump({"result": "FAIL", "provenance": why, "identical": 0, "proven": [], "fail": [],
                           "inline_drift": [], "data_differing": []}, open(args.json, "w"), indent=1)
            sys.exit(1)

    A = Image(args.a_dll, args.a_pdb)
    B = Image(args.b_dll, args.b_pdb)
    cmp = Cmp(A, B)
    cmp.tu_map = load_tu_map(args.tu_map)
    rules = load_allow(args.allow)
    pairs, onlya, onlyb = match(A, B, cmp)

    proof = None
    if args.proof:
        import tempfile
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tf:
            jpath = tf.name
        cmd = [sys.executable, __file__, *args.proof, "--strict", "--copies-ok", "--json", jpath, "--max", "0"]
        if args.tu_map:
            cmd += ["--tu-map", args.tu_map]
        subprocess.run(cmd, capture_output=True, text=True)
        try:
            proof = json.load(open(jpath))
        except (OSError, ValueError):
            proof = None
    proof_ok = bool(proof) and proof.get("result") == "PASS"
    proven = []

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
        if not args.strict and not un and (ia != ib or library_only(fa["name"])):
            delta = {k: (ia[k], ib[k]) for k in set(ia) | set(ib) if ia[k] != ib[k]}
            drift.append((fa["name"], r, delta))
            drift_names.add(norm(fa["name"]))
            continue
        if not args.strict and proof_ok and ia != ib:
            # the proof build (/Od, no optimizer) of the SAME two commits is byte-identical, so
            # this function's source compiles identically; with the inline sites
            # differing here, the /O2 difference is the inlining decision alone
            proven.append((fa["name"], r, [fmt_fp(u) for u in un[:6]]))
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
        if not args.strict and parent in drift_names and (not un or proof_ok):
            fdrift.append((fa["name"], r))
            continue
        why = allowed(rules, "differ", fa["name"])
        if why:
            allowed_hits.append(("differ", fa["name"], why))
            continue
        fails.append((fa["name"], fa["sig"], r, [fmt_fp(u) for u in un[:6]]))

    # only-in: renumbered funclets, copies, outlined
    copies, outlined, oa, ob = [], [], [], []
    renumbered = []
    if not args.strict:
        # MSVC numbers a function's catch$N/dtor$N funclets per compile; the same
        # funclet can come back under another number. Pair unmatched funclets of the
        # same parent whose bodies are identical.
        ga, gb = defaultdict(list), defaultdict(list)
        for f in onlya:
            m = FUNCLET.match(f["name"])
            if m:
                ga[norm(m.group(1))].append(f)
        for f in onlyb:
            m = FUNCLET.match(f["name"])
            if m:
                gb[norm(m.group(1))].append(f)
        gone_a, gone_b = set(), set()
        for parent in set(ga) & set(gb):
            lb = list(gb[parent])
            la = list(ga[parent])
            for fa in list(la):
                for fb in lb:
                    if cmp.compare(fa, fb) is None:
                        renumbered.append((fa["name"], fb["name"]))
                        gone_a.add(id(fa)); gone_b.add(id(fb))
                        lb.remove(fb); la.remove(fa)
                        break
            # a library parent's EH funclets come from whichever TU's COMDAT the
            # linker kept; its catch handler may inline differently (drift)
            if library_only(parent) or parent in drift_names:
                for fa in la:
                    for fb in lb:
                        if not cmp.fp_equal(fa, fb):
                            renumbered.append((fa["name"], fb["name"] + "  [drift]"))
                            gone_a.add(id(fa)); gone_b.add(id(fb))
                            lb.remove(fb)
                            break
        onlya = [f for f in onlya if id(f) not in gone_a]
        onlyb = [f for f in onlyb if id(f) not in gone_b]
    for mine, other, img_other, side, sink in ((onlya, B.procs, B, "A", oa), (onlyb, A.procs, A, "B", ob)):
        by = defaultdict(list)
        for f in other:
            by[(norm(f["name"]), norm(f["sig"]))].append(f)
        for f in mine:
            if not args.strict:
                fm = FUNCLET.match(f["name"])
                if fm and norm(fm.group(1)) in drift_names:
                    # MSVC numbers a function's unwind funclets per compile; when the
                    # parent's inlining drifted, its funclet set/numbering drifts too
                    outlined.append((side, f["name"], "(funclet of a drift function)"))
                    continue
            if not args.strict or args.copies_ok:
                twins = by.get((norm(f["name"]), norm(f["sig"])), [])
                vc = (A if side == "A" else B).xtor_var_const(f["name"])
                if vc is False:
                    mv = re.search(r"dynamic (?:initializer|atexit destructor) for '(.+?)''", f["name"])
                    var = norm(f["name"].split("`dynamic")[0] + mv.group(1)) if mv else None
                    vc = None if var and cmp.state_split_ok(var)[0] else False
                if twins and f.get("twin") and vc is not False:
                    # (an initializer/destructor copy of a MUTABLE variable is not
                    # accepted: see compare_data's per-TU copies)
                    # a copy refers to its own TU's twin data; compare with the TU of
                    # a twin reference free (fresh memo: these pairs are not reused)
                    saved, cmp.memo, cmp.own_tu_twins = cmp.memo, {}, True
                    hit = any((cmp.compare(f, t) if side == "A" else cmp.compare(t, f)) is None for t in twins)
                    cmp.memo, cmp.own_tu_twins = saved, False
                    if hit:
                        copies.append((side, f["name"]))
                        continue
            if not args.strict:
                qn = qual_key(f["name"])
                if img_other.inlinees.get(qn):
                    outlined.append((side, f["name"], qn))
                    continue
            why = allowed(rules, "only-" + side.lower(), f["name"])
            if why:
                allowed_hits.append(("only-" + side.lower(), f["name"], why))
            else:
                sink.append(f)

    dchecked, ddiff0, donly_a, donly_b, dfolded, dcopies = compare_data(A, B, cmp)
    ddiff = []
    for n, r in ddiff0 + [(n, "ONLY IN A: " + r) for n, r in donly_a] + [(n, "ONLY IN B: " + r) for n, r in donly_b]:
        why = allowed(rules, "data", n)
        (allowed_hits.append(("data", n, why)) if why else ddiff.append((n, r)))
    if args.strict:
        ddiff += [(n, "FOLDED (not allowed under --strict): " + r) for n, r in dfolded]
        if not args.copies_ok:
            ddiff += [(n, "COPY COUNT CHANGED (not allowed under --strict): " + r) for n, r in dcopies]

    if args.copies_ok:
        # --strict --copies-ok (the no-optimizer proof): every function and every
        # datum byte-identical; the ONLY tolerance is the number of identical
        # per-TU copies of a header-defined internal-linkage object
        strict_ok = not diffs and not oa and not ob and not ddiff0 and not renumbered \
            and not donly_a and not donly_b and not dfolded
    else:
        strict_ok = not diffs and not onlya and not onlyb and not ddiff0 and not renumbered \
            and not donly_a and not donly_b and not dfolded and not dcopies
    ok = not fails and not oa and not ob and not ddiff
    if args.proof and not proof_ok:
        ok = False
    verdict = "PASS" if strict_ok else ("FAIL" if not ok else
                                        "PASS-PROVEN" if proof_ok else "PASS-EXPLAINED-UNPROVEN")

    print(f"A: {args.a_dll}  ({len(A.procs)} procs)")
    print(f"B: {args.b_dll}  ({len(B.procs)} procs)")
    print(f"matched pairs: {len(pairs)}   identical: {same}   FAIL: {len(fails)}   "
          f"inline-drift: {len(drift)}   funclet-drift: {len(fdrift)}   "
          f"only-in-A: {len(oa)}   only-in-B: {len(ob)}   outlined: {len(outlined)}   "
          f"copies: {len(copies)}   renumbered: {len(renumbered)}   allow-listed: {len(allowed_hits)}")
    print(f"named data compared: {dchecked}   data differing / only-in-one-build: {len(ddiff)}   "
          f"folded constants: {len(dfolded)}   per-TU data copies: {len(dcopies)}")
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
        ("OUTLINED IN ONE BUILD ONLY (explained: inlined in the other, or an unwind funclet of a drift function)",
         [f"[only in {s}] {n}   " + (sn if sn.startswith("(") else f"(inlined as `{sn}` in the other)")
          for s, n, sn in outlined]),
        ("RENUMBERED FUNCLETS (explained: identical body, per-compile catch$/dtor$ number changed)",
         [f"{a}  ->  ...{b[-12:]}" for a, b in renumbered]),
        ("PER-TU COPIES (explained: copy count changed, every copy identical)",
         [f"[extra in {s}] {n}" for s, n in copies]),
        ("FAIL", [f"{n}  {s}\n      {r}" + "".join(f"\n        {u}" for u in us) for n, s, r, us in fails]),
        ("FOLDED CONSTANTS (explained: stored in one build, an S_CONSTANT of the same value in the other)",
         [f"{n}\n      {r}" for n, r in dfolded]),
        ("PER-TU DATA COPIES (explained: a header constant, one copy per including TU; every copy identical)",
         [f"{n}\n      {r}" for n, r in dcopies]),
        ("DATA DIFFERING / ONLY IN ONE BUILD", [f"{n}\n      {r}" for n, r in ddiff]),
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
    if args.proof:
        pr = proof or {}
        print(f"\nPROOF (no-optimizer builds, --strict --copies-ok): {pr.get('result', 'did not run')}"
              f"   identical: {pr.get('identical', '-')}   fail: {len(pr.get('fail', []))}   "
              f"only-in: {len(pr.get('only_a', [])) + len(pr.get('only_b', []))}   "
              f"data: {len(pr.get('data_differing', []))}")
        if proven:
            print(f"\nPROVEN BY THE NO-OPTIMIZER BUILD (inline sites differ; the own-code fingerprint "
                  f"differs too) ({len(proven)}):")
            for n, r, us in proven[:args.max]:
                print(f"  {n}\n      {r}" + "".join(f"\n        {u}" for u in us))
    print("\nRESULT:", verdict)
    if args.json:
        json.dump({"result": verdict, "identical": same, "proven": [n for n, _r, _u in proven],
                   "fail": [{"name": n, "sig": s, "detail": r, "fingerprint": u} for n, s, r, u in fails],
                   "inline_drift": [{"name": n, "detail": r, "inline_delta": {k: list(v) for k, v in d.items()}}
                                    for n, r, d in drift],
                   "funclet_drift": [n for n, _ in fdrift],
                   "outlined": outlined, "copies": copies, "renumbered": renumbered,
                   "only_a": [f["name"] for f in oa], "only_b": [f["name"] for f in ob],
                   "data_differing": ddiff, "data_folded": dfolded, "data_copies": dcopies,
                   "relinked": relinked, "allowed": allowed_hits,
                   "notes": cmp.notes, "path_pairs": sorted(cmp.path_pairs)},
                  open(args.json, "w"), indent=1, default=str)
    sys.exit(0 if verdict in ("PASS", "PASS-PROVEN") else 3 if verdict == "PASS-EXPLAINED-UNPROVEN" else 1)


if __name__ == "__main__":
    main()
