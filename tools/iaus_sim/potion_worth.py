#!/usr/bin/env python3
"""What a potion is worth, measured (marth 11-12, Q-potion-value: "Sim calibrates it": price a potion by its worth in later
fights over a chain of fights, not by a fixed constant). HOST-ONLY, standard library only.

    tools/iaus_sim/potion_worth.py --bin <sim> [--list L.sim.json] [--curves "<specs>"] [--set "<sets>"] --out <table.txt> [--header]

MEASURE   `<sim> --potion-calib N`: per class cell and potion kind (0 health, 1 magicka, 2 stamina) the SAME natural fights with
          the subject's stock of that kind forced to k = 0..4. Gives d[k] = what one more potion at the START of a fight buys (the
          paired mean Utility difference between stock k and k - 1, potions themselves not charged) and P[k][u] = how often he
          drinks u of them.
CHAIN     A fight is one of CHAIN fights between two restocks, at a uniformly random place in it. With h fights still to come the
          bag's value is W_h(p) = U(p) + sum_u P[p][u] W_{h-1}(p - u), W_0 = 0, U(p) = d[1] + ... + d[p]. The k-th potion in the
          bag at the end of a fight is worth m_h[k] = W_h(k) - W_h(k - 1); the table holds its mean over h = 0 .. CHAIN - 1.
          d[k] is floored at 0 (a potion cannot be worth less than nothing: he may leave it in the bag).
OUTPUT    one line per (class, kind): `<class> <kind> <m1> <m2> <m3> <m4>` in J. The simulator's Utility charges every potion drunk
          in a fight that worth (Oracle.h PotionCost). --header also rewrites tools/iaus_sim/PotionWorth.h (the built-in world's table).

The table belongs to a WORLD (its potions and foes) and to the drink behaviour it was measured under: re-measure after the drink
curves change (tune_driver.py does), and never use one world's table on another.
"""
import argparse
import os
import subprocess
import sys

CHAIN = 8   # fights between two restocks (a dungeon). The table is reported at 4 and 16 too (--sensitivity).


def measure(a):
    cmd = [a.bin, "--no-json", "--threads", str(a.jobs), "--seeds", ",".join(str(s) for s in range(a.seed0, a.seed0 + a.seeds)), "--potion-calib", str(a.fights)]
    if a.list:
        cmd += ["--list", a.list]
    if a.classes:
        cmd += ["--classes", a.classes]
    for c in (a.curves or "").split():
        cmd += ["--curve", c]
    if a.set:
        cmd += ["--set", a.set]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    rows = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 12 and p[0] == "POTCAL":
            rows[(p[1], int(p[2]), int(p[3]))] = (int(p[4]), float(p[5]), float(p[6]), float(p[7]), [float(x) for x in p[8:13]])
    if not rows:
        raise SystemExit("no POTCAL lines from: %s\n%s" % (" ".join(cmd), out[-400:]))
    return rows


def worth(rows, cls, kind, chain):
    d = [0.0] + [max(0.0, rows[(cls, kind, k)][2]) for k in range(1, 5)]
    P = [rows[(cls, kind, k)][4] for k in range(0, 5)]
    U = [sum(d[1:p + 1]) for p in range(5)]
    W = [0.0] * 5
    acc = [0.0] * 5
    for _h in range(chain):          # W is W_h on entry; the fight at this place has h fights after it
        for k in range(1, 5):
            acc[k] += W[k] - W[k - 1]
        W = [U[p] + sum(P[p][u] * W[p - u] for u in range(p + 1)) for p in range(5)]
    return [acc[k] / chain for k in range(1, 5)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--list")
    ap.add_argument("--classes", help="comma list; default every class cell")
    ap.add_argument("--curves", help="space-separated --curve specs (the drink behaviour the table is measured under)")
    ap.add_argument("--set", help="a --set string")
    ap.add_argument("--seeds", type=int, default=24)
    ap.add_argument("--seed0", type=int, default=7001, help="first seed (7001..: used by nothing else)")
    ap.add_argument("--fights", type=int, default=400, help="fights per seed, class, kind and stock")
    ap.add_argument("--chain", type=int, default=CHAIN)
    ap.add_argument("--jobs", type=int, default=12)
    ap.add_argument("--out", required=True)
    ap.add_argument("--header", action="store_true", help="also rewrite tools/iaus_sim/PotionWorth.h (built-in world only)")
    a = ap.parse_args()
    if a.header and a.list:
        raise SystemExit("--header is the built-in world's table: not with --list")
    rows = measure(a)
    classes = sorted({k[0] for k in rows})
    world = os.path.basename(a.list).split(".")[0] if a.list else "builtin"
    head = "# potion worth, world=%s, chain=%d fights, %d fights per (class, kind, stock), curves=[%s] set=[%s]" % (world, a.chain, rows[next(iter(rows))][0], a.curves or "", a.set or "")
    lines, hdr, raw = [head, "# <class> <kind 0 health|1 magicka|2 stamina> <worth in J of the 1st, 2nd, 3rd, 4th potion left in the bag>"], [], []
    for c in classes:
        for kind in range(3):
            m = worth(rows, c, kind, a.chain)
            lines.append("%s %d %.4f %.4f %.4f %.4f" % (c, kind, m[0], m[1], m[2], m[3]))
            hdr.append('        { "%s", %d, { %.4ff, %.4ff, %.4ff, %.4ff } },' % (c, kind, m[0], m[1], m[2], m[3]))
            raw.append("# raw %s %d  d(k)=%s  se=%s  drinks/fight at stock 4=%.3f  chain 4: %s  chain 16: %s" % (
                c, kind, " ".join("%+.3f" % rows[(c, kind, k)][2] for k in range(1, 5)), " ".join("%.3f" % rows[(c, kind, k)][3] for k in range(1, 5)),
                sum(u * p for u, p in enumerate(rows[(c, kind, 4)][4])), " ".join("%.3f" % x for x in worth(rows, c, kind, 4)), " ".join("%.3f" % x for x in worth(rows, c, kind, 16))))
    open(a.out, "w").write("\n".join(lines + raw) + "\n")
    if a.header:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "PotionWorth.h")
        open(path, "w").write("""#pragma once
// iaus_sim/PotionWorth.h -- GENERATED by tools/iaus_sim/potion_worth.py --header (do not edit by hand). HOST-ONLY.
// The worth in J of the k-th potion still in the bag when a fight ends, built-in world (marth 11-12, Q-potion-value: "Sim calibrates
// it"). Method: potion_worth.py's docstring and Docs/research/2026-11/iaus-sim-v2.md section 33.
// %s
namespace Sim {
    struct PotionWorthRow { const char* cls; int kind; float m[4]; };   // kind: 0 health, 1 magicka, 2 stamina
    inline constexpr PotionWorthRow kPotionWorthBuiltin[] = {
%s
    };
}
""" % (head[2:], "\n".join(hdr)))
    print("%s: %d classes -> %s%s" % (world, len(classes), a.out, " + PotionWorth.h" if a.header else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
