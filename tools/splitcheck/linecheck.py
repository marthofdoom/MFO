#!/usr/bin/env python3
"""linecheck -- the SOURCE half of a split proof. Exit 0 = PASS, 1 = FAIL.

Three checks over the old files (at --old-rev) and the new files (at --new-rev,
default: the working tree). Lines compare after stripping trailing whitespace.

1. MULTISET. The old and new lines are the same multiset except for:
   * lines a split may DROP or WRITE freely: blank, // comment, #include,
     #pragma, namespace open/close, a lone brace;
   * EDITED lines, each paired one-to-one with its edit: `inline ` / `extern `
     added in front, or a default argument (`= value`) moved off a definition's
     parameter;
   * in HEADERS only: new declarations (a statement ending in `;`, or the
     continuation line of one) -- the prototypes / `extern`s the split exports.
   Any other old line that vanished, or new code line that appeared, is FAIL.
2. SEAMS. Every pair of adjacent non-blank lines in a new file must have been
   adjacent in an old file, unless the seam sits at a block boundary: one side
   is a line of kind 1 above, a written/edited line, the end of a block (`}`,
   `};`), or the start of the next item's comment. A statement moved inside a
   function -- a reorder -- leaves a seam between two code lines and is FAIL.
3. Everything that is not a verbatim carry is PRINTED, so the reviewer reads
   every non-moved line.

  linecheck.py --old-rev 923e72d --old native/Actuation.cpp ... \\
               [--new-rev HEAD] --new native/cast/Fire.cpp ...
"""
import argparse
import re
import subprocess
import sys
from collections import Counter

WRAPPER = re.compile(r"^\s*(#include\b|#pragma\b|namespace\b[^;]*\{?\s*$|\}\s*(//.*)?$|\{\s*$|};\s*$)")


def read(rev, path):
    if rev is None:
        txt = open(path, encoding="utf-8", errors="replace").read()
    else:
        txt = subprocess.run(["git", "show", f"{rev}:{path}"], check=True, capture_output=True,
                             text=True, errors="replace").stdout
    return [ln.rstrip() for ln in txt.split("\n")]


def trivial(ln):
    s = ln.strip()
    return s == "" or s.startswith("//") or bool(WRAPPER.match(ln))


def code(ln):
    return ln.split("//", 1)[0].rstrip()


def edit_pairs(old_line, new_line):
    """True when new_line is old_line with an allowed mechanical edit."""
    a, b = code(old_line).strip(), code(new_line).strip()
    if a == b:
        return True
    for kw in ("inline ", "extern "):
        if b == kw + a:
            return True
    # a default argument moved off a definition's parameter list
    if re.sub(r"\s*=\s*[^,()]+(?=[,)])", "", a) == b:
        return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old-rev", required=True)
    ap.add_argument("--new-rev", default=None)
    ap.add_argument("--old", nargs="+", required=True)
    ap.add_argument("--new", nargs="+", required=True)
    ap.add_argument("--new-root", default=None,
                    help="read the --new paths under this directory instead (self-tests)")
    args = ap.parse_args()

    import os
    old_files = {p: read(args.old_rev, p) for p in args.old}
    new_files = {p: read(args.new_rev if not args.new_root else None,
                         os.path.join(args.new_root, p) if args.new_root else p) for p in args.new}
    old, new = Counter(), Counter()
    where_old, where_new = {}, {}
    all_new = {}
    for p, L in old_files.items():
        for i, ln in enumerate(L, 1):
            old[ln] += 1
            where_old.setdefault(ln, f"{p}:{i}")
    for p, L in new_files.items():
        for i, ln in enumerate(L, 1):
            new[ln] += 1
            where_new.setdefault(ln, f"{p}:{i}")
            all_new.setdefault(ln, []).append(f"{p}:{i}")
    gone = old - new
    added = new - old

    fails = []
    # --- 1. multiset -------------------------------------------------------------
    gone_code = [ln for ln in gone.elements() if not trivial(ln)]
    added_code = [ln for ln in added.elements() if not trivial(ln)]
    edited = []
    for ln in list(gone_code):
        for cand in added_code:
            if edit_pairs(ln, cand):
                edited.append((ln, cand))
                gone_code.remove(ln)
                added_code.remove(cand)
                break
    header_decls = []
    for ln in list(added_code):
        # an extra occurrence of a line that also appears in a HEADER is that
        # header's copy (a definition's first line re-used as its declaration)
        in_hdr = [w for w in all_new[ln] if w.rsplit(":", 1)[0].endswith(".h")]
        c = code(ln).strip()
        if in_hdr and (c.endswith(";") or c.endswith(",") or c.endswith("(") or c.endswith("{")
                       or old[ln] > 0):
            header_decls.append(ln)
            where_new[ln] = in_hdr[-1]
            added_code.remove(ln)
    for ln in gone_code:
        fails.append(f"OLD LINE LOST       {where_old[ln]:<44} {ln.strip()[:140]}")
    for ln in added_code:
        fails.append(f"NEW CODE LINE       {where_new[ln]:<44} {ln.strip()[:140]}")

    # --- 2. seams ----------------------------------------------------------------
    pairs = set()
    for L in old_files.values():
        nb = [ln for ln in L if ln.strip()]
        pairs |= set(zip(nb, nb[1:]))
    written = set(added) | {b for _a, b in edited}
    after, before = {}, {}
    for a_, b_ in pairs:
        after.setdefault(a_, set()).add(b_)
        before.setdefault(b_, set()).add(a_)
    DEF_START = re.compile(r"^\s*(?!(if|for|while|switch|else|do|return|case)\b)[\w:<>,\*&~ ]+[\s\*&]"
                           r"[\w:~]+\s*\(.*$|^\s*(struct|class|enum)\b")
    seams, benign = [], 0
    for p, L in new_files.items():
        nb = [(i, ln) for i, ln in enumerate(L, 1) if ln.strip()]
        for (i, a), (_j, b) in zip(nb, nb[1:]):
            if (a, b) in pairs:
                continue
            if trivial(a) or trivial(b) or a in written or b in written or \
                    code(a).strip().endswith(("}", "};")) or b.strip().startswith("//"):
                benign += 1
                continue
            # a HOLE: the line between them in the old file moved elsewhere
            if after.get(a, set()) & before.get(b, set()):
                benign += 1
                continue
            # the next top-level item begins (a definition after a finished statement)
            if code(a).strip().endswith(";") and DEF_START.match(b) and \
                    len(b) - len(b.lstrip()) <= len(a) - len(a.lstrip()):
                benign += 1
                continue
            seams.append(f"SEAM (reorder?)     {p}:{i:<6} {a.strip()[:70]}  ||  {b.strip()[:70]}")
    fails += seams

    # --- report ------------------------------------------------------------------
    print(f"old lines: {sum(old.values())}   new lines: {sum(new.values())}   "
          f"only-in-old: {sum(gone.values())}   only-in-new: {sum(added.values())}   "
          f"edited: {len(edited)}   header declarations: {len(header_decls)}   "
          f"block-boundary seams: {benign}   FAIL: {len(fails)}")
    print("\nEDITED LINES (old -> new):")
    for a, b in edited:
        print(f"  {where_old[a]:<44} {a.strip()[:100]}\n  {where_new[b]:<44} {b.strip()[:100]}")
    print("\nNEW HEADER DECLARATIONS:")
    for ln in header_decls:
        print(f"  {where_new[ln]:<44} {ln.strip()[:140]}")
    print("\nONLY IN OLD (dropped wrappers / comments / includes):")
    for ln, c in sorted(gone.items(), key=lambda x: where_old[x[0]]):
        if trivial(ln):
            print(f"  {c}x  {where_old[ln]:<42} {ln.strip()[:130]}")
    print("\nONLY IN NEW (written wrappers / comments / includes):")
    for ln, c in sorted(added.items(), key=lambda x: where_new[x[0]]):
        if trivial(ln):
            print(f"  {c}x  {where_new[ln]:<42} {ln.strip()[:130]}")
    if fails:
        print(f"\nFAIL ({len(fails)}):")
        for f in fails:
            print("  " + f)
    print("\nRESULT:", "FAIL" if fails else "PASS")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
