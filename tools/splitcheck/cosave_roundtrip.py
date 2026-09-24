#!/usr/bin/env python3
"""cosave_roundtrip -- emulated MFO co-save round-trip: run the REAL
CoSaveLoad/CoSaveSave machine code of two MFO.dll builds on the records of real
SKSE co-saves (.skse), and compare what each build writes back. The behavioural
check CLAUDE.md requires for a co-save function a split only EXPLAINS.

  cosave_roundtrip.py --a DIR_A --b DIR_B [--json out] [--selftest] SAVE.skse ...

Needs python packages unicorn, capstone, pefile (a venv) and llvm-pdbutil.
Copy the saves somewhere first; this only reads them. Exit 0 = every save
IDENTICAL in both builds; 1 = a difference or an emulation fault.
--selftest additionally patches B's CoSaveSave record version (a copy in a temp
dir) and requires EVERY save to come out DIFFERENT (the harness can see a change).

DIR_* holds MFO.dll + MFO.pdb. Per save and per DLL, on a FRESH emulator:
  1. MFO's own C++ dynamic initializers run (maps/vectors/strings are constructed);
  2. ProgAllocator::CoSaveLoad(intfc, version) reads the save's PRGN bytes;
     then ProgAllocator::CoSaveSave(intfc) writes PRGN back out;
  3. Actuation::CoLoadForcedWeapons / CoSaveForcedWeapons do the same for FWPN.
The SKSE SerializationInterface, the heap, the engine singletons and logging are
stubs (see STUBS); every stub call is RECORDED (name + key arguments) and the
two builds' recorded sequences, written bytes and emulated instruction counts
are compared.
"""
import argparse
import bisect
import json
import os
import struct
import sys
from collections import Counter, defaultdict

import pefile
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
from unicorn.x86_const import (UC_X86_REG_RAX, UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9,
                               UC_X86_REG_RSP, UC_X86_REG_RIP, UC_X86_REG_GS_BASE)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import splitcheck as SC  # noqa: E402  (PDB symbol loader)

# ---- SKSE co-save parser ----------------------------------------------------------
# Parse an SKSE64 co-save (.skse) and extract one plugin's records.
# 
# Format (skse64 Serialization.cpp):
#   Header       { u32 signature 'SKSE'; u32 formatVersion; u32 skseVersion; u32 runtimeVersion; u32 numPlugins }
#   per plugin:  { u32 uid; u32 numChunks; u32 length }  then its chunks:
#   per chunk:   { u32 type; u32 version; u32 length }  then `length` bytes
# Four-character codes are C multichar constants ('MFO0' = 'M'<<24|...), stored little-endian.
def fourcc(s):
    v = 0
    for ch in s:
        v = (v << 8) | ord(ch)
    return v


def cc_str(v):
    return bytes([(v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255]).decode("latin-1")


def parse(path):
    b = open(path, "rb").read()
    sig, fmt, skse, rt, nplug = struct.unpack_from("<5I", b, 0)
    if sig not in (fourcc("SKSE"), fourcc("ESKS")):
        raise ValueError(f"not an SKSE co-save: signature {sig:#x}")
    off = 20
    plugins = {}
    for _ in range(nplug):
        uid, nchunks, length = struct.unpack_from("<3I", b, off)
        off += 12
        end = off + length
        chunks = []
        p = off
        for _c in range(nchunks):
            typ, ver, ln = struct.unpack_from("<3I", b, p)
            p += 12
            chunks.append((cc_str(typ), ver, b[p:p + ln]))
            p += ln
        plugins[cc_str(uid)] = chunks
        off = end
    return {"format": fmt, "skse": skse, "runtime": rt, "plugins": plugins}


STACK, STACK_SZ = 0x10000000, 0x200000
HEAP, HEAP_SZ = 0x20000000, 0x10000000
FAKE, FAKE_SZ = 0x40000000, 0x100000          # fake SKSE interface / fake logger / TEB / TLS
STUBS_BASE, STUBS_SZ = 0x50000000, 0x100000    # one 16-byte slot per import + MAGIC return
MAGIC_RET = STUBS_BASE + STUBS_SZ - 0x10
INTFC = FAKE + 0x1000
LOGGER = FAKE + 0x2000
TEB = FAKE + 0x10000
TLS_ARR = FAKE + 0x11000
TLS_BLOCKS = FAKE + 0x20000


class Stop(Exception):
    pass


class Emu:
    def __init__(self, ddir, symbols):
        self.dll = os.path.join(ddir, "MFO.dll")
        self.sym = symbols
        pe = pefile.PE(self.dll)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        img = pe.get_memory_mapped_image()
        size = (len(img) + 0xFFFF) & ~0xFFFF
        self.uc = uc = Uc(UC_ARCH_X86, UC_MODE_64)
        uc.mem_map(self.base, size)
        uc.mem_write(self.base, bytes(img))
        for a, s in ((STACK, STACK_SZ), (HEAP, HEAP_SZ), (FAKE, FAKE_SZ), (STUBS_BASE, STUBS_SZ)):
            uc.mem_map(a, s)
        uc.mem_write(STUBS_BASE, b"\xc3" * STUBS_SZ)
        # TEB: ThreadLocalStoragePointer at gs:[0x58] -> array of zeroed TLS blocks
        uc.reg_write(UC_X86_REG_GS_BASE, TEB)
        uc.mem_write(TEB + 0x58, struct.pack("<Q", TLS_ARR))
        for i in range(64):
            uc.mem_write(TLS_ARR + 8 * i, struct.pack("<Q", TLS_BLOCKS + 0x1000 * i))
        self.heap_top = HEAP
        self.alloc_sizes = {}
        # imports -> stub slots
        self.imports = {}
        slot = STUBS_BASE
        for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
            for imp in entry.imports:
                name = (imp.name or b"#%d" % imp.ordinal).decode()
                uc.mem_write(imp.address, struct.pack("<Q", slot))
                self.imports[slot] = (entry.dll.decode(), name)
                slot += 16
        self.calls = []            # recorded stub calls
        self.written = []          # (type, version, bytes)
        self.cur_write = None
        self.read_buf, self.read_pos = b"", 0
        self.faults = []
        self.icount = Counter()    # per-function instruction counts (by containing proc)
        self.total = 0
        self.stub_at = {}          # address -> handler
        self._install_stubs()
        uc.hook_add(UC_HOOK_CODE, self._on_code, begin=self.base, end=self.base + size)
        uc.hook_add(UC_HOOK_CODE, self._on_stub, begin=STUBS_BASE, end=STUBS_BASE + STUBS_SZ)
        uc.hook_add(UC_HOOK_MEM_INVALID, self._on_bad)

    # --- plumbing ---------------------------------------------------------------
    def reg(self, r):
        return self.uc.reg_read(r)

    def ret(self, value=0):
        uc = self.uc
        rsp = uc.reg_read(UC_X86_REG_RSP)
        ra = struct.unpack("<Q", uc.mem_read(rsp, 8))[0]
        uc.reg_write(UC_X86_REG_RSP, rsp + 8)
        uc.reg_write(UC_X86_REG_RAX, value & 0xFFFFFFFFFFFFFFFF)
        uc.reg_write(UC_X86_REG_RIP, ra)

    def rd(self, a, n):
        return bytes(self.uc.mem_read(a, n))

    def cstr(self, a, n=256):
        b = self.rd(a, n)
        return b.split(b"\0", 1)[0].decode("latin-1")

    def malloc(self, n):
        n = max(int(n), 1)
        a = (self.heap_top + 15) & ~15
        self.heap_top = a + n
        if self.heap_top >= HEAP + HEAP_SZ:
            raise Stop("emulated heap exhausted")
        self.uc.mem_write(a, b"\0" * n)
        self.alloc_sizes[a] = n
        return a

    def call(self, rva_or_addr, *args, limit=50_000_000):
        uc = self.uc
        addr = rva_or_addr if rva_or_addr >= self.base else self.base + rva_or_addr
        rsp = STACK + STACK_SZ - 0x1000
        rsp -= 0x40                                   # shadow space + alignment
        uc.mem_write(rsp, struct.pack("<Q", MAGIC_RET))
        uc.reg_write(UC_X86_REG_RSP, rsp)
        for r, v in zip((UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9), args):
            uc.reg_write(r, v)
        try:
            uc.emu_start(addr, MAGIC_RET, count=limit)
        except UcError as e:
            rip = uc.reg_read(UC_X86_REG_RIP)
            self.faults.append(f"UcError {e} at {self.where(rip)}")
            raise Stop(f"fault at {self.where(rip)}: {e}")
        return uc.reg_read(UC_X86_REG_RAX)

    def where(self, addr):
        if STUBS_BASE <= addr < STUBS_BASE + STUBS_SZ:
            return f"import {self.imports.get(addr & ~0xF, ('?', '?'))[1]}"
        rva = addr - self.base
        n = self.sym.proc_name_at(rva)
        return f"{n}+{rva - self.sym.proc_start(rva):#x}" if n else hex(addr)

    def _on_bad(self, uc, access, addr, size, value, _ud):
        rip = uc.reg_read(UC_X86_REG_RIP)
        self.faults.append(f"bad memory access {addr:#x} (size {size}) at {self.where(rip)}")
        return False

    def _on_code(self, uc, addr, size, _ud):
        self.total += 1
        self.icount[self.sym.proc_start(addr - self.base)] += 1
        h = self.stub_at.get(addr)
        if h:
            h()

    def _on_stub(self, uc, addr, size, _ud):
        if addr == MAGIC_RET:
            return
        dll, name = self.imports.get(addr & ~0xF, ("?", "?"))
        self._import(name)

    # --- imports ----------------------------------------------------------------
    def _import(self, name):
        rcx, rdx, r8 = self.reg(UC_X86_REG_RCX), self.reg(UC_X86_REG_RDX), self.reg(UC_X86_REG_R8)
        if name in ("malloc", "_malloc_base"):
            return self.ret(self.malloc(rcx))
        if name in ("calloc", "_calloc_base"):
            return self.ret(self.malloc(rcx * rdx))
        if name in ("free", "_free_base", "_aligned_free"):
            return self.ret(0)
        if name == "_aligned_malloc":
            return self.ret(self.malloc(rcx + rdx) + 0)
        if name in ("realloc", "_realloc_base"):
            new = self.malloc(rdx)
            if rcx:
                self.uc.mem_write(new, self.rd(rcx, min(self.alloc_sizes.get(rcx, rdx), rdx)))
            return self.ret(new)
        if name == "memcpy" or name == "memmove":
            if r8:
                self.uc.mem_write(rcx, self.rd(rdx, r8))
            return self.ret(rcx)
        if name == "memset":
            if r8:
                self.uc.mem_write(rcx, bytes([rdx & 0xFF]) * r8)
            return self.ret(rcx)
        if name == "memcmp":
            a, b = self.rd(rcx, r8), self.rd(rdx, r8)
            return self.ret(0 if a == b else (-1 if a < b else 1))
        if name == "strlen":
            return self.ret(len(self.cstr(rcx, 4096)))
        if name == "_Query_perf_frequency":
            return self.ret(10_000_000)
        if name == "_Query_perf_counter":
            return self.ret(0)
        if name in ("atexit", "_register_onexit_function", "_crt_atexit", "_initialize_onexit_table"):
            return self.ret(0)
        if name in ("AcquireSRWLockExclusive", "ReleaseSRWLockExclusive", "AcquireSRWLockShared",
                    "ReleaseSRWLockShared", "InitializeSRWLock", "_Mtx_lock", "_Mtx_unlock"):
            return self.ret(0)
        if name in ("_Mtx_init_in_situ", "_Mtx_destroy_in_situ", "_Cnd_init_in_situ", "_Cnd_destroy_in_situ"):
            return self.ret(0)
        if name in ("_Init_thread_header", "_Init_thread_footer", "_Init_thread_abort"):
            return self.ret(0)
        self.calls.append(("import", name))
        return self.ret(0)

    # --- in-DLL function stubs ----------------------------------------------------
    def _install_stubs(self):
        S = self.sym

        def at(names, h):
            for n in names:
                for rva in S.rvas_named(n):
                    self.stub_at[self.base + rva] = h

        def ser_read():
            buf, ln = self.reg(UC_X86_REG_RDX), self.reg(UC_X86_REG_R8) & 0xFFFFFFFF
            chunk = self.read_buf[self.read_pos:self.read_pos + ln]
            self.read_pos += len(chunk)
            if chunk:
                self.uc.mem_write(buf, chunk)
            self.calls.append(("ReadRecordData", ln, len(chunk)))
            self.ret(len(chunk))

        def ser_resolve():
            old, outp = self.reg(UC_X86_REG_RDX) & 0xFFFFFFFF, self.reg(UC_X86_REG_R8)
            self.uc.mem_write(outp, struct.pack("<I", old))
            self.calls.append(("ResolveFormID", old))
            self.ret(1)

        def ser_open():
            typ, ver = self.reg(UC_X86_REG_RDX) & 0xFFFFFFFF, self.reg(UC_X86_REG_R8) & 0xFFFFFFFF
            self.cur_write = [typ, ver, bytearray()]
            self.written.append(self.cur_write)
            self.calls.append(("OpenRecord", typ, ver))
            self.ret(1)

        def ser_write():
            buf, ln = self.reg(UC_X86_REG_RDX), self.reg(UC_X86_REG_R8) & 0xFFFFFFFF
            data = self.rd(buf, ln) if ln else b""
            if self.cur_write is None:
                raise Stop("WriteRecordData before OpenRecord")
            self.cur_write[2] += data
            self.ret(1)

        def null_singleton(label):
            def h():
                self.calls.append(("engine", label))
                self.ret(0)
            return h

        def lookup_addon():
            local = self.reg(UC_X86_REG_RCX) & 0xFFFFFFFF
            sv = self.reg(UC_X86_REG_RDX)
            p, n = struct.unpack("<QQ", self.rd(sv, 16))
            plugin = self.rd(p, min(n, 260)).decode("latin-1") if p else ""
            self.calls.append(("LookupAddonForm", local, plugin))
            self.ret(0)

        def logger_raw():
            self.ret(LOGGER)

        def log_call():
            lvl = self.reg(UC_X86_REG_R8) & 0xFF
            fp = self.reg(UC_X86_REG_R9)
            try:
                p, n = struct.unpack("<QQ", self.rd(fp, 16))
                fmt = self.rd(p, min(n, 400)).decode("latin-1")
            except Exception:  # noqa
                fmt = "?"
            self.calls.append(("log", lvl, fmt))
            self.ret(0)

        def noop(label):
            def h():
                self.calls.append(("noop", label))
                self.ret(0)
            return h

        at(["SKSE::SerializationInterface::ReadRecordData"], ser_read)
        at(["SKSE::SerializationInterface::ResolveFormID"], ser_resolve)
        at(["SKSE::SerializationInterface::OpenRecord"], ser_open)
        at(["SKSE::SerializationInterface::WriteRecordData"], ser_write)
        at(["RE::PlayerCharacter::GetSingleton"], null_singleton("PlayerCharacter::GetSingleton -> null"))
        at(["RE::TESDataHandler::GetSingleton"], null_singleton("TESDataHandler::GetSingleton -> null"))
        at(["MFO::ProgAllocator::LookupAddonForm"], lookup_addon)
        at(["spdlog::default_logger_raw"], logger_raw)
        for n in S.names_matching(r"^spdlog::logger::log_"):
            at([n], log_call)
        at(["MFO::MainThread::Post"], noop("MainThread::Post"))


class Symbols:
    """Name <-> address lookups for one build, from its PDB."""

    def __init__(self, ddir):
        self.img = SC.Image(os.path.join(ddir, "MFO.dll"), os.path.join(ddir, "MFO.pdb"))
        self.by_name = defaultdict(set)
        for p in self.img.procs:
            self.by_name[p["name"]].add(p["rva"])
        self.starts = sorted(self.img.proc_at)
        self.first_name = {}
        for p in self.img.procs:
            self.first_name.setdefault(p["rva"], p["name"])

    def rvas_named(self, name):
        return sorted(self.by_name.get(name, ()))

    def names_matching(self, rx):
        import re
        r = re.compile(rx)
        return [n for n in self.by_name if r.search(n)]

    def proc_start(self, rva):
        j = bisect.bisect_right(self.starts, rva) - 1
        if j >= 0 and rva < self.starts[j] + self.img.proc_at[self.starts[j]]:
            return self.starts[j]
        return -1

    def proc_name_at(self, rva):
        s = self.proc_start(rva)
        return self.first_name.get(s)

    def one(self, name):
        r = self.rvas_named(name)
        if len(r) != 1:
            raise KeyError(f"{name}: {len(r)} definitions")
        return r[0]

    def initializers(self):
        """MFO's own C++ dynamic initializers, in address order."""
        out = []
        for n, rvas in self.by_name.items():
            if "dynamic initializer for '" in n and n.startswith("MFO::") and "dtor$" not in n:
                out += list(rvas)
        return sorted(set(out))


def run_one(sym, ddir, recs):
    """recs: {'PRGN': (ver, bytes) | None, 'FWPN': ...}. Returns a result dict."""
    e = Emu(ddir, sym)
    res = {"init": 0, "steps": [], "faults": [], "written": [], "calls": [], "icount": {}}
    try:
        for rva in sym.initializers():
            try:
                e.call(rva, limit=2_000_000)
            except Stop as s:
                res["init_faults"] = res.get("init_faults", []) + [f"{sym.first_name.get(rva)}: {s}"]
        res["init"] = e.total
        res["init_faults"] = res.get("init_faults", []) + e.faults
        e.faults = []
        e.calls = []
        steps = [("PRGN", "MFO::ProgAllocator::CoSaveLoad", "MFO::ProgAllocator::CoSaveSave"),
                 ("FWPN", "MFO::Actuation::CoLoadForcedWeapons", "MFO::Actuation::CoSaveForcedWeapons")]
        for typ, load_fn, save_fn in steps:
            ver, data = recs.get(typ, (None, None))
            if data is not None:
                e.read_buf, e.read_pos = data, 0
                t0, c0 = e.total, len(e.calls)
                e.call(sym.one(load_fn), INTFC, ver)
                res["steps"].append((load_fn, e.total - t0, e.read_pos, len(data)))
            t0 = e.total
            e.call(sym.one(save_fn), INTFC)
            res["steps"].append((save_fn, e.total - t0, None, None))
    except Stop as s:
        res["faults"].append(str(s))
    res["faults"] += e.faults
    res["written_raw"] = [(SC_cc(t), v, bytes(b)) for t, v, b in e.written]
    res["written"] = [(SC_cc(t), v, bytes(b).hex()) for t, v, b in e.written]
    res["calls"] = e.calls
    top = e.icount.most_common(12)
    res["icount"] = {sym.first_name.get(r, hex(r)): c for r, c in top}
    return res


def SC_cc(v):
    return bytes([(v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255]).decode("latin-1")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--json")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("saves", nargs="+")
    args = ap.parse_args()
    sa, sb = Symbols(args.a), Symbols(args.b)
    summary = Counter()
    out = []
    for path in args.saves:
        try:
            cs = parse(path)
        except Exception as ex:  # noqa
            summary["unreadable"] += 1
            continue
        mfo = cs["plugins"].get("MFO0")
        if not mfo:
            summary["no MFO record"] += 1
            continue
        recs = {t: (v, d) for t, v, d in mfo if t in ("PRGN", "FWPN")}
        ra = run_one(sa, args.a, recs)
        rb = run_one(sb, args.b, recs)
        same_w = ra["written"] == rb["written"]
        same_c = ra["calls"] == rb["calls"]
        ok = same_w and same_c and not ra["faults"] and not rb["faults"]
        summary["IDENTICAL" if ok else "DIFFERENT" if not (ra["faults"] or rb["faults"]) else "FAULT"] += 1
        if ra.get("init_faults") or rb.get("init_faults"):
            print("   init faults A:", ra.get("init_faults"), " B:", rb.get("init_faults"))
        prgn_in = recs.get("PRGN")
        prgn_out = [w for w in ra["written"] if w[0] == "PRGN"]
        rt_equal = bool(prgn_in and prgn_out and prgn_in[0] == prgn_out[0][1] and prgn_in[1].hex() == prgn_out[0][2])
        row = {"save": os.path.basename(path), "prgn_version_in": prgn_in[0] if prgn_in else None,
               "prgn_bytes_in": len(prgn_in[1]) if prgn_in else 0, "identical": ok,
               "written_identical": same_w, "calls_identical": same_c,
               "a_faults": ra["faults"], "b_faults": rb["faults"],
               "a_steps": ra["steps"], "b_steps": rb["steps"],
               "save_reproduced_input_A": rt_equal,
               "calls_n": len(ra["calls"]),
               "engine_calls": sorted({str(c) for c in ra["calls"] if c[0] in ("engine", "LookupAddonForm", "import", "noop")})}
        out.append(row)
        print(f"{'OK  ' if ok else 'DIFF'} {row['save'][:70]:<70} PRGN v{row['prgn_version_in']} {row['prgn_bytes_in']}B "
              f"load A/B {ra['steps'][0][1] if ra['steps'] else '-'}/{rb['steps'][0][1] if rb['steps'] else '-'} insns"
              f"  faults {len(ra['faults'])}/{len(rb['faults'])}")
        if not ok:
            for f in (ra["faults"] + rb["faults"])[:3]:
                print("      ", f[:200])
            if not same_w:
                print("       written A:", str(ra["written"])[:200])
                print("       written B:", str(rb["written"])[:200])
            if not same_c:
                for i, (x, y) in enumerate(zip(ra["calls"], rb["calls"])):
                    if x != y:
                        print(f"       first call diff #{i}: A {x} | B {y}")
                        break
                else:
                    print(f"       call count A {len(ra['calls'])} B {len(rb['calls'])}")
    print("\nSUMMARY:", dict(summary))
    if args.json:
        json.dump({"summary": summary, "rows": out}, open(args.json, "w"), indent=1, default=str)
    ok = set(summary) <= {"IDENTICAL", "no MFO record"}
    if args.selftest:
        ok = selftest(args, sa) and ok
    sys.exit(0 if ok else 1)


def selftest(args, sa):
    """Patch B's CoSaveSave `mov r8d, <PRGN version>` (the OpenRecord version)
    in a temp copy; every save must then come out DIFFERENT."""
    import shutil
    import tempfile
    import capstone
    sb = Symbols(args.b)
    rva = sb.one("MFO::ProgAllocator::CoSaveSave")
    pe = pefile.PE(os.path.join(args.b, "MFO.dll"))
    img = pe.get_memory_mapped_image()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    seen_prgn = False
    target = None
    for ins in md.disasm(img[rva:rva + sb.img.proc_at[rva]], rva):
        ops = ins.operands
        if ins.mnemonic == "mov" and len(ops) == 2 and ops[1].type == capstone.x86.X86_OP_IMM:
            if ops[1].imm == 0x5052474E:          # 'PRGN'
                seen_prgn = True
            elif seen_prgn and ins.op_str.startswith("r8d"):
                target = ins
                break
    if target is None:
        print("SELFTEST: could not find the OpenRecord version immediate")
        return False
    d = tempfile.mkdtemp(prefix="cosave-selftest-")
    raw = bytearray(open(os.path.join(args.b, "MFO.dll"), "rb").read())
    off = pe.get_offset_from_rva(target.address + target.imm_offset)
    raw[off] ^= 0x01
    open(os.path.join(d, "MFO.dll"), "wb").write(raw)
    shutil.copy(os.path.join(args.b, "MFO.pdb"), os.path.join(d, "MFO.pdb"))
    sp = Symbols(d)
    diff = total = 0
    for path in args.saves:
        try:
            cs = parse(path)
        except Exception:  # noqa
            continue
        mfo = cs["plugins"].get("MFO0")
        if not mfo:
            continue
        recs = {t: (v, x) for t, v, x in mfo if t in ("PRGN", "FWPN")}
        ra, rb = run_one(sa, args.a, recs), run_one(sp, d, recs)
        total += 1
        diff += ra["written"] != rb["written"]
    ok = total > 0 and diff == total
    print(f"SELFTEST (B's PRGN record version {target.op_str} patched): {diff}/{total} saves DIFFERENT -> "
          f"{'OK' if ok else 'HOLE'}")
    return ok


if __name__ == "__main__":
    main()
