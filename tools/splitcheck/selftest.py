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
  N6  (with --a0/--b0) a constant changed in the no-inline
      proof build                                           -> --proof must FAIL
  P1  (with --a0/--b0) the real pair with --proof           -> reported (PASS-PROVEN expected)
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


def run_sc(a, b, work, tu_map=None, proof=None, strict=False):
    j = os.path.join(work, "sc_%08x.json" % random.getrandbits(32))
    cmd = [sys.executable, os.path.join(HERE, "splitcheck.py"),
           os.path.join(a, "MFO.dll"), os.path.join(a, "MFO.pdb"),
           os.path.join(b, "MFO.dll"), os.path.join(b, "MFO.pdb"), "--json", j, "--max", "0"]
    if tu_map:
        cmd += ["--tu-map", tu_map]
    if strict:
        cmd += ["--strict"]
    if proof:
        a0, b0 = proof
        cmd += ["--proof", os.path.join(a0, "MFO.dll"), os.path.join(a0, "MFO.pdb"),
                os.path.join(b0, "MFO.dll"), os.path.join(b0, "MFO.pdb")]
    p = subprocess.run(cmd, capture_output=True, text=True)
    res = json.load(open(j))
    res["_exit"] = p.returncode
    return res


def copy_build(src, dst):
    os.makedirs(dst, exist_ok=True)
    for f in ("MFO.dll", "MFO.pdb"):
        shutil.copy(os.path.join(src, f), os.path.join(dst, f))
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


def plant_imm(cmp, img, f, dst):
    """Change one own-code immediate of f in dst's DLL to a value that appears
    nowhere else in f (so no helper attribution can excuse it)."""
    cands = imm_candidates(cmp, img, f)
    if not cands:
        return None
    ins = cands[0]
    old = ins.operands[1].imm
    new = 0x5B if ins.imm_size == 1 else 0x3A7
    if new == old:
        new += 2
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
        record("N6 constant changed in the no-optimizer proof build", r["result"] == "FAIL",
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
            record("N7 unnamed string literal changed in the proof build", r["result"] == "FAIL",
                   f"{r['result']} (exit {r['_exit']}), {f['name'][:80]} literal {s[:40]!r} first letter case-flipped")

    bad = [c for c, ok, _d in results if not ok]
    print(f"\n{len(results) - len(bad)}/{len(results)} cases behaved as required" + (f"; HOLES: {bad}" if bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
