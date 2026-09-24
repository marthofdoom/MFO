#!/usr/bin/env python3
"""linecheck -- the SOURCE half of a split proof: the old files' lines and the
new files' lines must be the same multiset, except for what the split had to
write (new file headers, namespace wrappers, declarations) and what it had to
drop (old file headers, wrappers). Both lists are printed IN FULL so a reviewer
reads every non-moved line; nothing else can hide in a pure move.

  linecheck.py --old-rev 923e72d --old native/Actuation.cpp ... \\
               [--new-rev HEAD] --new native/cast/Fire.cpp ...

Old files are read from --old-rev; new files from --new-rev (default: the
working tree). Lines compare exactly after stripping trailing whitespace.
"""
import argparse
import subprocess
import sys
from collections import Counter


def read(rev, path):
    if rev is None:
        return open(path, encoding="utf-8", errors="replace").read().split("\n")
    return subprocess.run(["git", "show", f"{rev}:{path}"], check=True, capture_output=True,
                          text=True, errors="replace").stdout.split("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old-rev", required=True)
    ap.add_argument("--new-rev", default=None)
    ap.add_argument("--old", nargs="+", required=True)
    ap.add_argument("--new", nargs="+", required=True)
    args = ap.parse_args()
    old, new = Counter(), Counter()
    where_old, where_new = {}, {}
    for p in args.old:
        for i, ln in enumerate(read(args.old_rev, p), 1):
            ln = ln.rstrip()
            old[ln] += 1
            where_old.setdefault(ln, f"{p}:{i}")
    for p in args.new:
        for i, ln in enumerate(read(args.new_rev, p), 1):
            ln = ln.rstrip()
            new[ln] += 1
            where_new.setdefault(ln, f"{p}:{i}")
    gone = old - new
    added = new - old
    print(f"old lines: {sum(old.values())}   new lines: {sum(new.values())}   "
          f"only-in-old: {sum(gone.values())}   only-in-new: {sum(added.values())}")
    print("\nONLY IN OLD (dropped or edited):")
    for ln, c in sorted(gone.items(), key=lambda x: where_old[x[0]]):
        print(f"  {c}x  {where_old[ln]:<42} {ln[:150]}")
    print("\nONLY IN NEW (written by the split):")
    for ln, c in sorted(added.items(), key=lambda x: where_new[x[0]]):
        print(f"  {c}x  {where_new[ln]:<42} {ln[:150]}")


if __name__ == "__main__":
    sys.exit(main())
