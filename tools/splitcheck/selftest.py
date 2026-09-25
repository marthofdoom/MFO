#!/usr/bin/env python3
"""selftest -- NEGATIVE self-tests: prove splitcheck / linecheck FAIL on the
defects a split must never hide. Each case plants ONE defect in a copy of the
branch build (a binary patch of MFO.dll, a byte edit of MFO.pdb, or a swapped
pair of source lines) and runs the real tool on it. Exit 0 = every case behaved
as required; 1 = a case did not (the tool has a hole).

  selftest.py --a MAIN_DIR --b BRANCH_DIR --work SCRATCH [--tu-map MAP]
              [--a0 MAIN_NOINLINE_DIR --b0 BRANCH_NOINLINE_DIR]
              [--lc-old-rev REV --lc-old OLD... --lc-new NEW...]

Every *_DIR holds MFO.dll + MFO.pdb (a CI `MFO-dll` artifact). Cases:
  P0  control: branch vs branch                            -> must PASS
  N1  a constant changed in an inline-DRIFT function       -> must FAIL naming it
  N2  a constant changed in a std::function wrapper of an
      MFO lambda (a library-shaped name holding MFO code)  -> must FAIL naming it
  N3  an anonymous-namespace twin REBOUND: one TU's code
      pointed at another TU's same-named file-local       -> must FAIL naming it
  N4  a named data symbol deleted from the branch PDB       -> must FAIL (only in A)
  N5  two adjacent statements swapped in a new source file  -> linecheck must FAIL (SEAM)
  N1b the same drift function, constant v -> v+1           -> must FAIL naming it
  N9  the last byte of a NAMED const aggregate > 16 bytes   -> must FAIL naming it
  With --a0/--b0 (the /Od proof builds, each with its CI build record):
  P1  the real pair with --proof                            -> PASS-PROVEN (or PASS)
  N6  a constant changed in the proof build                 -> --proof must FAIL
  N7  an unnamed ASCII literal changed (byte 0)             -> --proof must FAIL
  N8  a UTF-8 literal changed past byte 16                  -> --proof must FAIL
  N9b the last byte of an unnamed read-only aggregate > 16  -> --proof must FAIL
  N10 a MUTABLE header static that two moved functions
      shared (one copy in A0) now read as two copies (B0)   -> must FAIL as a STATE split
  N10a the same split reached at an offset into the variable -> must FAIL as a STATE split
  N10b per-TU copies of ONE same-named accessor read their own
      TU's copy in B0 (one copy in A0)                     -> must FAIL as a STATE split
  N12 an unnamed object's bytes set to "AB\\0" in B0 only (a
      content-derived boundary in one build)               -> --proof must FAIL
  N11 A0's build record names B's commit                    -> --proof must REFUSE (provenance)
  N11b a DLL that is not the one its build record hashes    -> --proof must REFUSE (provenance)
A planted copy gets a build record describing IT (same commit, its own SHA-256),
so N6-N9b/N10 must be caught by the comparison, not by the provenance check.
"""
import argparse
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

import capstone

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import splitcheck as SC  # noqa: E402

X = capstone.x86


def load(d):
    return SC.Image(os.path.join(d, "MFO.dll"), os.path.join(d, "MFO.pdb"))


def run_sc(a, b, work, tu_map=None, proof=None, strict=False, copies_ok=False):
    j = os.path.join(work, "sc_%08x.json" % random.getrandbits(32))
    cmd = [sys.executable, os.path.join(HERE, "splitcheck.py"),
           os.path.join(a, "MFO.dll"), os.path.join(a, "MFO.pdb"),
           os.path.join(b, "MFO.dll"), os.path.join(b, "MFO.pdb"), "--json", j, "--max", "0"]
    if tu_map:
        cmd += ["--tu-map", tu_map]
    if strict:
        cmd += ["--strict"]
    if copies_ok:
        cmd += ["--copies-ok"]
    if proof:
        a0, b0 = proof
        cmd += ["--proof", os.path.join(a0, "MFO.dll"), os.path.join(a0, "MFO.pdb"),
                os.path.join(b0, "MFO.dll"), os.path.join(b0, "MFO.pdb")]
    p = subprocess.run(cmd, capture_output=True, text=True)
    res = json.load(open(j))
    res["_exit"] = p.returncode
    return res


def write_info(dst, info):
    import hashlib
    info = dict(info)
    info["dll_sha256"] = hashlib.sha256(open(os.path.join(dst, "MFO.dll"), "rb").read()).hexdigest()
    open(os.path.join(dst, "build-info.txt"), "w").write(" ".join(f"{k}={v}" for k, v in info.items()) + "\n")


def copy_build(src, dst):
    """A copy of a build, WITH a build record beside it that describes the
    copy (same commit and flags, the copy's own SHA-256): a planted defect must
    be caught by the comparison itself, not by the provenance check."""
    os.makedirs(dst, exist_ok=True)
    for f in ("MFO.dll", "MFO.pdb"):
        shutil.copy(os.path.join(src, f), os.path.join(dst, f))
    info, _p = SC.read_build_info(os.path.join(src, "MFO.dll"))
    if info:
        write_info(dst, info)
    return dst


def patch_dll(dst, rva, data):
    import pefile
    path = os.path.join(dst, "MFO.dll")
    pe = pefile.PE(path, fast_load=True)
    off = pe.get_offset_from_rva(rva)
    pe.close()
    raw = bytearray(open(path, "rb").read())
    raw[off:off + len(data)] = data
    open(path, "wb").write(raw)
    info, p = SC.read_build_info(path)
    if info and p == os.path.join(dst, "build-info.txt"):
        write_info(dst, info)


def imm_candidates(cmp, img, f, own_only=True):
    """cmp/mov reg, imm8/imm32 instructions in f's MFO code (own code, or an
    inlined MFO body such as a lambda inlined into its std::function wrapper)."""
    spans = cmp.own_spans(img, f) if own_only else []
    out = []
    for ins in cmp.disasm(img, f["rva"], f["size"]):
        off = ins.address - f["rva"]
        if any(a <= off < b for a, b, _n in spans) and not cmp.in_mfo_inlinee(img, f, off):
            continue
        ops = ins.operands
        if ins.mnemonic in ("cmp", "mov") and len(ops) == 2 and ops[0].type == X.X86_OP_REG \
                and ops[1].type == X.X86_OP_IMM and 2 <= ops[1].imm <= 0x1000 and ins.imm_size in (1, 4) \
                and ops[0].reg not in cmp.FRAME_REGS:
            out.append(ins)
    return out


def plant_imm(cmp, img, f, dst, plus_one=False):
    """Change one own-code immediate of f in dst's DLL: to a value that appears
    nowhere else in f, or (plus_one) to v+1 -- the small realistic edit a
    helper-attribution rule could wrongly excuse."""
    cands = imm_candidates(cmp, img, f)
    if not cands:
        return None
    ins = cands[0]
    old = ins.operands[1].imm
    new = 0x5B if ins.imm_size == 1 else 0x3A7
    if new == old:
        new += 2
    if plus_one:
        new = old + 1
    patch_dll(dst, ins.address + ins.imm_offset, new.to_bytes(ins.imm_size, "little"))
    return f"{f['name'][:120]} +{ins.address - f['rva']:#x}: {ins.mnemonic} {ins.op_str}  imm {old:#x} -> {new:#x}"


def named_in(fails, name):
    want = SC.norm(SC.untag(name))
    return any(SC.norm(SC.untag(x["name"])) == want for x in fails)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--a0")
    ap.add_argument("--b0")
    ap.add_argument("--tu-map")
    ap.add_argument("--work", default=None)
    ap.add_argument("--lc-old-rev")
    ap.add_argument("--lc-old", nargs="*", default=[])
    ap.add_argument("--lc-new", nargs="*", default=[])
    args = ap.parse_args()
    work = args.work or tempfile.mkdtemp(prefix="splitcheck-selftest-")
    os.makedirs(work, exist_ok=True)
    results = []

    def record(case, ok, detail):
        results.append((case, ok, detail))
        print(f"{'OK  ' if ok else 'HOLE'} {case}: {detail}", flush=True)

    # ---- P0 control -----------------------------------------------------------
    r = run_sc(args.b, args.b, work, args.tu_map)
    record("P0 branch vs itself", r["result"] == "PASS", f"{r['result']} (exit {r['_exit']})")

    base = run_sc(args.a, args.b, work, args.tu_map)
    A, B = load(args.a), load(args.b)
    cmp = SC.Cmp(A, B)
    b_by_name = {}
    for f in B.procs:
        b_by_name.setdefault(f["name"], []).append(f)
    base_fail = {SC.norm(SC.untag(x["name"])) for x in base["fail"]}

    # ---- N1 constant in an inline-drift function --------------------------------
    target, note = None, None
    for d in base["inline_drift"]:
        n = d["name"]
        if SC.library_only(n) or SC.norm(SC.untag(n)) in base_fail:
            continue
        fs = b_by_name.get(n) or []
        if len(fs) == 1 and imm_candidates(cmp, B, fs[0]):
            target = fs[0]
            break
    if target is None:
        record("N1 drift-function constant", False, "no inline-drift MFO function with an own-code immediate")
    else:
        dst = copy_build(args.b, os.path.join(work, "n1"))
        note = plant_imm(cmp, B, target, dst)
        r = run_sc(args.a, dst, work, args.tu_map)
        record("N1 drift-function constant", r["result"] == "FAIL" and named_in(r["fail"], target["name"]),
               f"{r['result']}, patched {note}; named in FAIL: {named_in(r['fail'], target['name'])}")
        # N1b: the same function, the realistic edit v -> v+1
        dst = copy_build(args.b, os.path.join(work, "n1b"))
        note = plant_imm(cmp, B, target, dst, plus_one=True)
        r = run_sc(args.a, dst, work, args.tu_map)
        record("N1b drift-function constant v->v+1", r["result"] == "FAIL" and named_in(r["fail"], target["name"]),
               f"{r['result']}, patched {note}; named in FAIL: {named_in(r['fail'], target['name'])}")

    # ---- N2 constant in a std::function wrapper of an MFO lambda ------------------
    target = None
    for f in sorted(B.procs, key=lambda f: f["name"]):
        n = f["name"]
        if re.match(r"^std::_Func_impl_no_alloc<", n) and "MFO::" in n and "_Do_call" in n \
                and SC.norm(SC.untag(n)) not in base_fail and len(b_by_name[n]) == 1 \
                and imm_candidates(cmp, B, f):
            target = f
            break
    if target is None:
        record("N2 MFO lambda wrapper constant", False, "no std::_Func_impl_no_alloc<MFO lambda>::_Do_call with an immediate")
    else:
        dst = copy_build(args.b, os.path.join(work, "n2"))
        note = plant_imm(cmp, B, target, dst)
        r = run_sc(args.a, dst, work, args.tu_map)
        record("N2 MFO lambda wrapper constant", r["result"] == "FAIL" and named_in(r["fail"], target["name"]),
               f"{r['result']}, patched {note}; named in FAIL: {named_in(r['fail'], target['name'])}")

    # ---- N3 anonymous-namespace twin rebind --------------------------------------
    # twins: one data name defined at 2+ addresses by different TUs (the PDB
    # drops "anonymous namespace" from data names, so they read identically)
    by_untag = {}
    for n, rva, mod in B.datas:
        if n in B.twins and n.startswith("MFO::") and mod is not None:
            by_untag.setdefault(n, []).append((rva, mod, n))
    done = False
    for name, copies in sorted(by_untag.items()):
        mods = {m for _r, m, _n in copies}
        if len(copies) < 2 or len(mods) < 2:
            continue
        for rva_x, mod_x, _nx in copies:
            other = [c for c in copies if c[1] != mod_x]
            rva_y = other[0][0]
            # prefer real code over the variable's own initializer / atexit destructor
            for f in sorted(B.procs, key=lambda f: ("dynamic " in f["name"], f["name"])):
                if f.get("mod") != mod_x:
                    continue
                for ins in cmp.disasm(B, f["rva"], f["size"]):
                    if not (X.X86_OP_MEM in [o.type for o in ins.operands]):
                        continue
                    mem = [o for o in ins.operands if o.type == X.X86_OP_MEM][0]
                    if mem.mem.base != X.X86_REG_RIP:
                        continue
                    tgt = ins.address + ins.size + mem.mem.disp
                    if tgt != rva_x:
                        continue
                    dst = copy_build(args.b, os.path.join(work, "n3"))
                    new_disp = rva_y - (ins.address + ins.size)
                    patch_dll(dst, ins.address + ins.disp_offset, new_disp.to_bytes(4, "little", signed=True))
                    r = run_sc(args.a, dst, work, args.tu_map)
                    ok = r["result"] == "FAIL" and named_in(r["fail"], f["name"])
                    record("N3 anon-twin rebind", ok,
                           f"{r['result']}, {f['name'][:90]} +{ins.address - f['rva']:#x} now points at "
                           f"{name} of {os.path.basename(other[0][1] or '?')} instead of {os.path.basename(mod_x or '?')}; "
                           f"named in FAIL: {named_in(r['fail'], f['name'])}")
                    done = True
                    break
                if done:
                    break
            if done:
                break
        if done:
            break
    if not done:
        record("N3 anon-twin rebind", False, "no module-tagged twin data with a RIP reference found")

    # ---- N4 named data symbol deleted from the branch PDB --------------------------
    cand = None
    for n, rva, mod in B.datas:
        if n.startswith("MFO::") and SC.TAG not in n and "`" not in n and mod is not None:
            leaf = n.rsplit("::", 1)[-1]
            if re.fullmatch(r"[A-Za-z_]\w{5,}", leaf):
                pdb = open(os.path.join(args.b, "MFO.pdb"), "rb").read()
                if pdb.count(leaf.encode() + b"\0") >= 1 and sum(1 for m, _r, _mm in B.datas if m.endswith("::" + leaf)) == 1:
                    cand = (n, leaf)
                    break
    if cand is None:
        record("N4 data symbol deleted", False, "no candidate")
    else:
        n, leaf = cand
        dst = copy_build(args.b, os.path.join(work, "n4"))
        pdbp = os.path.join(dst, "MFO.pdb")
        raw = open(pdbp, "rb").read()
        gone = (leaf[:-1] + ("Q" if leaf[-1] != "Q" else "R")).encode()
        raw = raw.replace(leaf.encode() + b"\0", gone + b"\0")
        open(pdbp, "wb").write(raw)
        r = run_sc(args.a, dst, work, args.tu_map)
        hit = any(SC.untag(x[0]) == n and "ONLY IN A" in x[1] for x in r["data_differing"])
        record("N4 data symbol deleted", r["result"] == "FAIL" and hit,
               f"{r['result']}, {n} renamed away in the branch PDB; reported ONLY IN A: {hit}")

    # ---- N5 a reorder that keeps the line multiset ---------------------------------
    if args.lc_old_rev and args.lc_old and args.lc_new:
        root = os.path.join(work, "n5")
        shutil.rmtree(root, ignore_errors=True)
        swapped = None
        for p in args.lc_new:
            dstp = os.path.join(root, p)
            os.makedirs(os.path.dirname(dstp), exist_ok=True)
            shutil.copy(p, dstp)
        for p in args.lc_new:
            if not p.endswith(".cpp") or swapped:
                continue
            L = open(p, encoding="utf-8", errors="replace").read().split("\n")
            for i in range(len(L) - 1):
                a, b = L[i], L[i + 1]
                ia, ib = len(a) - len(a.lstrip()), len(b) - len(b.lstrip())
                if ia >= 8 and ia == ib and a.rstrip().endswith(";") and b.rstrip().endswith(";") \
                        and a.strip() != b.strip() and not a.strip().startswith(("//", "return")) \
                        and not b.strip().startswith(("//", "return")) and "=" in a and "(" in b:
                    L[i], L[i + 1] = b, a
                    open(os.path.join(root, p), "w", encoding="utf-8").write("\n".join(L))
                    swapped = f"{p}:{i + 1}-{i + 2}"
                    break
        cmd = [sys.executable, os.path.join(HERE, "linecheck.py"), "--old-rev", args.lc_old_rev,
               "--old", *args.lc_old, "--new", *args.lc_new]
        ctl = subprocess.run(cmd, capture_output=True, text=True)
        record("P2 linecheck control", ctl.returncode == 0, f"exit {ctl.returncode}")
        r = subprocess.run(cmd + ["--new-root", root], capture_output=True, text=True)
        ok = r.returncode == 1 and "SEAM" in r.stdout
        record("N5 statement reorder", ok, f"exit {r.returncode}, swapped {swapped}; SEAM reported: {'SEAM' in r.stdout}")
    else:
        record("N5 statement reorder", False, "not run (--lc-old-rev/--lc-old/--lc-new not given)")

    # ---- N6 / P1 the no-inline proof -------------------------------------------------
    if args.a0 and args.b0:
        r = run_sc(args.a, args.b, work, args.tu_map, proof=(args.a0, args.b0))
        record("P1 real pair with --proof", r["result"] == "PASS-PROVEN" or r["result"] == "PASS",
               f"{r['result']} (exit {r['_exit']}), proven {len(r.get('proven', []))}")
        B0 = load(args.b0)
        cmp0 = SC.Cmp(B0, B0)
        target = next((f for f in sorted(B0.procs, key=lambda f: f["name"])
                       if f["name"].startswith("MFO::") and not f["name"].startswith("`")
                       and imm_candidates(cmp0, B0, f)), None)
        dst = copy_build(args.b0, os.path.join(work, "n6"))
        note = plant_imm(cmp0, B0, target, dst)
        r = run_sc(args.a, args.b, work, args.tu_map, proof=(args.a0, dst))
        record("N6 constant changed in the no-optimizer proof build",
               r["result"] == "FAIL" and not r.get("provenance"),
               f"{r['result']} (exit {r['_exit']}), patched {note}")
        # N7: one character of an UNNAMED string literal (the /Od build has no
        # ??_C symbols; the tool compares such literals by their text)
        lit = None
        for f in sorted(B0.procs, key=lambda f: f["name"]):
            if not f["name"].startswith("MFO::"):
                continue
            for ins in cmp0.disasm(B0, f["rva"], f["size"]):
                mem = [o for o in ins.operands if o.type == X.X86_OP_MEM]
                if ins.mnemonic != "lea" or not mem or mem[0].mem.base != X.X86_REG_RIP:
                    continue
                t = ins.address + ins.size + mem[0].mem.disp
                c = B0.resolve(t)[0]
                s = B0.cstring(t)
                if B0.sec_of(t) == ".rdata" and c[0] == "sym" and c[2] and len(s) >= 6 and s.isascii() \
                        and s[:1].isalpha():
                    lit = (f, t, s)
                    break
            if lit:
                break
        if lit is None:
            record("N7 unnamed string literal changed in the proof build", False, "no candidate literal")
        else:
            f, t, s = lit
            dst = copy_build(args.b0, os.path.join(work, "n7"))
            patch_dll(dst, t, bytes([s[0] ^ 0x20]))       # flip the case of its first letter
            r = run_sc(args.a, args.b, work, args.tu_map, proof=(args.a0, dst))
            record("N7 unnamed string literal changed in the proof build",
                   r["result"] == "FAIL" and not r.get("provenance"),
                   f"{r['result']} (exit {r['_exit']}), {f['name'][:80]} literal {s[:40]!r} first letter case-flipped")

        # N8: a UTF-8 literal (non-ASCII bytes, e.g. an em-dash) changed PAST
        # byte 16, in the proof build
        lit = None
        for f in sorted(B0.procs, key=lambda f: f["name"]):
            if not f["name"].startswith("MFO::"):
                continue
            for ins in cmp0.disasm(B0, f["rva"], f["size"]):
                mem = [o for o in ins.operands if o.type == X.X86_OP_MEM]
                if not mem or mem[0].mem.base != X.X86_REG_RIP:
                    continue
                t = ins.address + ins.size + mem[0].mem.disp
                s = B0.cstring(t)
                if not B0.readonly_data(t) or len(s) < 32 or s.isascii() or not SC.is_text(s):
                    continue
                k = next((i for i in range(20, len(s)) if chr(s[i]).isalpha() and s[i] < 128), None)
                if k is not None:
                    lit = (f, t, s, k)
                    break
            if lit:
                break
        if lit is None:
            record("N8 UTF-8 literal changed past byte 16 (proof build)", False, "no candidate literal")
        else:
            f, t, s, k = lit
            dst = copy_build(args.b0, os.path.join(work, "n8"))
            patch_dll(dst, t + k, bytes([s[k] ^ 0x20]))
            r = run_sc(args.a, args.b, work, args.tu_map, proof=(args.a0, dst))
            record("N8 UTF-8 literal changed past byte 16 (proof build)",
                   r["result"] == "FAIL" and not r.get("provenance"),
                   f"{r['result']} (exit {r['_exit']}), {f['name'][:70]} literal {s[:36]!r}... byte {k} case-flipped")

        # N9b: the LAST byte of a NAMED read-only aggregate > 16 bytes that is
        # OUTSIDE compare_data's MFO:: scope (a library/CommonLib table), so only
        # the code reference sees it: the same name used to be enough there.
        # (/Od has no unnamed non-text read-only object > 16 bytes to plant in.)
        cand = None
        for f in sorted(B0.procs, key=lambda f: f["name"]):
            if not f["name"].startswith("MFO::"):
                continue
            for ins in cmp0.disasm(B0, f["rva"], f["size"]):
                mem = [o for o in ins.operands if o.type == X.X86_OP_MEM]
                if not mem or mem[0].mem.base != X.X86_REG_RIP:
                    continue
                t = ins.address + ins.size + mem[0].mem.disp
                if not B0.readonly_data(t):
                    continue
                kind, names, off, _r = B0.resolve(t)[0]
                sz = B0.data_size.get(t) or 0
                if kind != "sym" or off != 0 or sz <= 16 or any("`" in x or SC.DATA_SCOPE.match(x)
                                                                 or x.startswith("??_C") for x in names):
                    continue
                body = B0.read(t, sz)
                last = max((q for q in range(16, sz) if not any(t + q - d in B0.relocs for d in range(8))),
                           default=None)
                if last is not None:
                    cand = (f, t, sz, last, body[last], sorted(names)[0])
                    break
            if cand:
                break
        if cand is None:
            record("N9b tail byte of a named out-of-scope read-only aggregate >16 bytes (proof build)", False,
                   "no candidate")
        else:
            f, t, sz, last, v, nm = cand
            dst = copy_build(args.b0, os.path.join(work, "n9b"))
            patch_dll(dst, t + last, bytes([v ^ 0x01]))
            r = run_sc(args.a, args.b, work, args.tu_map, proof=(args.a0, dst))
            record("N9b tail byte of a named out-of-scope read-only aggregate >16 bytes (proof build)",
                   r["result"] == "FAIL" and not r.get("provenance"),
                   f"{r['result']} (exit {r['_exit']}), {nm[:60]} ({sz} bytes, read by {f['name'][:50]}) "
                   f"byte {last} flipped")

        # N10: a MUTABLE header static read by two functions the split moved
        # into different TUs. Planted consistently in both proof builds: in A0
        # both functions read the old TU's copy (one shared piece of state), in
        # B0 each reads its own new TU's copy (the state is now split in two).
        # Every function then compares identical; only the state split is wrong.
        A0 = load(args.a0)
        cmpa = SC.Cmp(A0, A0)
        tu = SC.load_tu_map(args.tu_map) if args.tu_map else {}
        plan = None
        by_name_a = {}
        for n, r_, m in A0.datas:
            by_name_a.setdefault(SC.norm(n), []).append((r_, m))
        by_name_b = {}
        for n, r_, m in B0.datas:
            by_name_b.setdefault(SC.norm(n), []).append((r_, m))
        for var, la in sorted(by_name_a.items()):
            lb = by_name_b.get(var, [])
            if len(la) < 2 or len(lb) <= len(la) or all(A0.is_const_data(r_) for r_, _m in la):
                continue
            ma_copy = {m: r_ for r_, m in la}
            mb_copy = {m: r_ for r_, m in lb}
            for ma, news in tu.items():
                if ma not in ma_copy:
                    continue
                nb = [m for m in news if m in mb_copy]
                if len(nb) < 2:
                    continue
                # one function per new TU, present in A0 in the old TU, with a
                # RIP-relative lea/mov we can redirect at the same offset in both
                picks = []
                for mb in nb[:6]:
                    for fb in sorted(B0.procs, key=lambda p: p["name"]):
                        if fb.get("mod") != mb or not fb["name"].startswith("MFO::") or "dynamic" in fb["name"]:
                            continue
                        fa = next((p for p in A0.procs if p["name"] == fb["name"] and p.get("mod") == ma
                                   and p["size"] == fb["size"]), None)
                        if not fa:
                            continue
                        ia = [i for i in cmpa.disasm(A0, fa["rva"], fa["size"])
                              if i.mnemonic == "lea" and i.operands[1].type == X.X86_OP_MEM
                              and i.operands[1].mem.base == X.X86_REG_RIP]
                        ib = [i for i in cmp0.disasm(B0, fb["rva"], fb["size"])
                              if i.mnemonic == "lea" and i.operands[1].type == X.X86_OP_MEM
                              and i.operands[1].mem.base == X.X86_REG_RIP]
                        if ia and ib and ia[0].address - fa["rva"] == ib[0].address - fb["rva"]:
                            picks.append((fa, fb, ia[0], ib[0], mb))
                            break
                    if len(picks) == 2:
                        break
                if len(picks) == 2:
                    plan = (var, ma, ma_copy[ma], picks, mb_copy)
                    break
            if plan:
                break
        if plan is None:
            record("N10 mutable header static split across two moved functions", False,
                   "no candidate: no mutable per-TU static with copies in an old TU and two of its new TUs")
        else:
            var, ma, ra_copy, picks, mb_copy = plan

            def plant_split(tag, off):
                da = copy_build(args.a0, os.path.join(work, tag + "a"))
                db = copy_build(args.b0, os.path.join(work, tag + "b"))
                for fa, fb, ia, ib, mb in picks:
                    patch_dll(da, ia.address + ia.disp_offset,
                              (ra_copy + off - (ia.address + ia.size)).to_bytes(4, "little", signed=True))
                    patch_dll(db, ib.address + ib.disp_offset,
                              (mb_copy[mb] + off - (ib.address + ib.size)).to_bytes(4, "little", signed=True))
                return run_sc(da, db, work, args.tu_map, strict=True, copies_ok=True)
            for tag, off, label in (("n10", 0, "N10 mutable header static split across two moved functions"),
                                    ("n10x", 8, "N10a the same split, reached at an OFFSET into the variable "
                                                "([rip+g+8], a member)")):
                if off and (B0.data_size.get(mb_copy[picks[0][4]]) or 0) <= off:
                    record(label, False, f"{var} is too small for an offset access")
                    continue
                r = plant_split(tag, off)
                hit = [x for x in r["data_differing"] if SC.norm(SC.untag(x[0])) == var and "STATE" in x[1]]
                fns_ok = not any(named_in(r["fail"], fa["name"]) for fa, *_ in picks)
                record(label, r["result"] == "FAIL" and bool(hit),
                       f"{r['result']}, {var}+{off}: {picks[0][0]['name'][:44]} and {picks[1][0]['name'][:44]} share "
                       f"{os.path.basename(ma)}'s copy in A0, read the {picks[0][4]} / {picks[1][4]} copies in B0; "
                       f"reported as state split: {bool(hit)}; the functions themselves compare identical: {fns_ok}")

            # N10b: the split reaches the state through PER-TU COPIES of ONE
            # same-named function (a header static / anonymous-namespace
            # accessor): one copy in the old TU reads the old TU's variable in
            # A0; each new TU's copy reads its own TU's variable in B0. Wave 1
            # has no such accessor, so it is built from the N10 plant: the second
            # function's NAME is rewritten to the first's in both PDBs (the two
            # names are picked to have equal length), which makes them two per-TU
            # copies of one function. Readers keyed by name alone would merge the
            # two B copies into one reader and see nothing split.
            fa0, fb0, _ia0, _ib0, mb0 = picks[0]
            pair = None
            for ma2, news in sorted(tu.items()):
                if ma2 != ma:
                    continue
                cands = {}
                for fb in B0.procs:
                    mb = fb.get("mod")
                    if mb not in news or mb not in mb_copy or not fb["name"].startswith("MFO::") \
                            or "dynamic" in fb["name"] or "`" in fb["name"]:
                        continue
                    fa = next((q for q in A0.procs if q["name"] == fb["name"] and q.get("mod") == ma
                               and q["size"] == fb["size"]), None)
                    if not fa:
                        continue
                    ia = [x for x in cmpa.disasm(A0, fa["rva"], fa["size"])
                          if x.mnemonic == "lea" and x.operands[1].type == X.X86_OP_MEM
                          and x.operands[1].mem.base == X.X86_REG_RIP]
                    ib = [x for x in cmp0.disasm(B0, fb["rva"], fb["size"])
                          if x.mnemonic == "lea" and x.operands[1].type == X.X86_OP_MEM
                          and x.operands[1].mem.base == X.X86_REG_RIP]
                    if ia and ib and ia[0].address - fa["rva"] == ib[0].address - fb["rva"]:
                        cands.setdefault(len(fb["name"]), []).append((fa, fb, ia[0], ib[0], mb))
                for _ln, lst in sorted(cands.items()):
                    mods = {}
                    for c in lst:
                        mods.setdefault(c[4], c)
                    if len(mods) >= 2 and len({c[1]["name"] for c in mods.values()}) >= 2:
                        pair = list(mods.values())[:2]
                        break
            if pair is None:
                record("N10b per-TU copies of one accessor split the state", False,
                       "no two moved functions of equal name length in two new TUs to stand in for one accessor")
            else:
                da = copy_build(args.a0, os.path.join(work, "n10ba"))
                db = copy_build(args.b0, os.path.join(work, "n10bb"))
                keep, other = pair[0][1]["name"], pair[1][1]["name"]
                for d in (da, db):
                    pdbp = os.path.join(d, "MFO.pdb")
                    raw = open(pdbp, "rb").read()
                    raw = raw.replace(other.encode() + b"\0", keep.encode() + b"\0")
                    open(pdbp, "wb").write(raw)
                for fa, fb, ia, ib, mb in pair:
                    patch_dll(da, ia.address + ia.disp_offset,
                              (ra_copy - (ia.address + ia.size)).to_bytes(4, "little", signed=True))
                    patch_dll(db, ib.address + ib.disp_offset,
                              (mb_copy[mb] - (ib.address + ib.size)).to_bytes(4, "little", signed=True))
                r = run_sc(da, db, work, args.tu_map, strict=True, copies_ok=True)
                hit = [x for x in r["data_differing"] if SC.norm(SC.untag(x[0])) == var and "STATE" in x[1]]
                record("N10b per-TU copies of one accessor split the state", r["result"] == "FAIL" and bool(hit),
                       f"{r['result']}, {keep[:50]} (and {other[:40]} renamed to it): one copy in "
                       f"{os.path.basename(ma)} reads {var}'s old copy in A0; the copies in {pair[0][4]} / "
                       f"{pair[1][4]} read their own in B0; reported as state split: {bool(hit)}")

        # N12: a change that CREATES a content-derived boundary in one build:
        # bytes k..k+2 of an unnamed non-text object set to "AB\0", so B0 alone
        # sees a pooled-literal start there (review repro, rv3b/repro_b.py)
        cand = None
        for f in sorted(B0.procs, key=lambda f: f["name"]):
            if not f["name"].startswith("MFO::"):
                continue
            for ins in cmp0.disasm(B0, f["rva"], f["size"]):
                for o in ins.operands:
                    if o.type != X.X86_OP_MEM or o.mem.base != X.X86_REG_RIP:
                        continue
                    t = ins.address + ins.size + o.mem.disp
                    if not B0.readonly_data(t) or SC.is_text(B0.cstring(t), 1):
                        continue
                    kind, _n, off, _r = B0.resolve(t)[0]
                    if kind == "sym" and off == 0:
                        continue
                    ext = B0.struct_extent(t)
                    body = B0.read(t, ext)
                    za = B0.data_size.get(t - off) if kind == "sym" else None
                    if za and off <= za:
                        continue                       # inside a named object: compared by name + content
                    if any((t + q) in B0.relocs for q in range(ext)):
                        continue
                    if not body or body[0] == 0:
                        continue                       # "" is the documented residual (MFO-B92)
                    k = next((q for q in range(4, ext - 3) if body[q - 1] == 0), None)
                    if k is not None:
                        cand = (f, t, ext, k)
                        break
                if cand:
                    break
            if cand:
                break
        if cand is None:
            record("N12 a change that creates a boundary in one build only (proof build)", False, "no candidate")
        else:
            f, t, ext, k = cand
            dst = copy_build(args.b0, os.path.join(work, "n12"))
            patch_dll(dst, t + k, b"AB\0")
            r = run_sc(args.a, args.b, work, args.tu_map, proof=(args.a0, dst))
            record("N12 a change that creates a boundary in one build only (proof build)",
                   r["result"] == "FAIL" and not r.get("provenance"),
                   f"{r['result']} (exit {r['_exit']}), object at {t:#x} ({ext} bytes, read by {f['name'][:50]}) "
                   f"bytes {k}..{k + 2} = 'AB\\0'")

        # N11: the four builds do not pair up -- the proof must refuse
        info_b, _p = SC.read_build_info(os.path.join(args.b, "MFO.dll"))
        dst = copy_build(args.a0, os.path.join(work, "n11"))
        ia, _p = SC.read_build_info(os.path.join(dst, "MFO.dll"))
        if ia and info_b:
            ia["built"] = info_b.get("built", "0" * 40)          # A0 claims the BRANCH commit
            write_info(dst, ia)
        r = run_sc(args.a, args.b, work, args.tu_map, proof=(dst, args.b0))
        record("N11 mismatched build SHAs (A0 built from B's commit)",
               r["result"] == "FAIL" and bool(r.get("provenance")),
               f"{r['result']} (exit {r['_exit']}): {(r.get('provenance') or ['no provenance refusal'])[0][:110]}")
        dst = copy_build(args.b0, os.path.join(work, "n11b"))
        raw = bytearray(open(os.path.join(dst, "MFO.dll"), "rb").read())
        raw[-1] ^= 0x01                                           # DLL changed, record NOT updated
        open(os.path.join(dst, "MFO.dll"), "wb").write(raw)
        r = run_sc(args.a, args.b, work, args.tu_map, proof=(args.a0, dst))
        record("N11b a DLL that is not the one its build record describes",
               r["result"] == "FAIL" and bool(r.get("provenance")),
               f"{r['result']} (exit {r['_exit']}): {(r.get('provenance') or ['no provenance refusal'])[0][:110]}")

    # ---- N9 the tail byte of a NAMED const aggregate > 16 bytes ------------------------
    cand = None
    names_a = {}
    for n, r_, m in A.datas:
        names_a.setdefault(SC.norm(n), []).append(r_)
    for n, r_, m in sorted(B.datas):
        sz = B.data_size.get(r_) or 0
        if SC.DATA_SCOPE.match(n) and "`" not in n and sz > 16 and B.is_const_data(r_) \
                and len(names_a.get(SC.norm(n), [])) == 1 \
                and sum(1 for nn, _r, _m in B.datas if nn == n) == 1:
            body = B.read(r_, sz)
            last = max((i for i in range(16, sz) if (r_ + i) not in B.relocs), default=None)
            if last is not None:
                cand = (n, r_, sz, last, body[last])
                break
    if cand is None:
        record("N9 tail byte of a named const aggregate >16 bytes", False, "no candidate")
    else:
        n, r_, sz, last, v = cand
        dst = copy_build(args.b, os.path.join(work, "n9"))
        patch_dll(dst, r_ + last, bytes([v ^ 0x01]))
        r = run_sc(args.a, dst, work, args.tu_map)
        hit = any(SC.norm(SC.untag(x[0])) == SC.norm(n) for x in r["data_differing"])
        record("N9 tail byte of a named const aggregate >16 bytes", r["result"] == "FAIL" and hit,
               f"{r['result']}, {n} ({sz} bytes) byte {last} flipped; reported: {hit}")

    bad = [c for c, ok, _d in results if not ok]
    print(f"\n{len(results) - len(bad)}/{len(results)} cases behaved as required" + (f"; HOLES: {bad}" if bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
