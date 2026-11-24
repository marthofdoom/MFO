#!/usr/bin/env python3
"""oracle_replay.py -- the proof of the oracle race's cost cuts (iaus-sim-v4.md 15.7). READ-ONLY, host-only.

    sim ... --class-oracle N --oracle-dump dump.jsonl        (the full race; every option is then run on every stream)
    python3 tools/iaus_sim/oracle_replay.py dump.jsonl [more.jsonl]

A stream's seed does not depend on which options stand (Oracle.h OraclePolicy::Serve), so a decision's utilities are one fixed table:
option x stream. A race rule only chooses which cells of it to pay for. This tool replays rules on the dumped tables and prints, per rule:
the rollouts per decision, the speed-up over the full race, how often the pick is the full race's, and the LOSS in U per decision: the
128-stream mean of the best option minus that of the rule's pick (the full race's own loss is not 0: it may drop the option whose 128
mean is the highest). `race()` below mirrors the race in Oracle.h line for line; `--check` asserts it reproduces the dump's own picks and
rollout counts (dump under the full race, without --oracle-pair / --oracle-nmin / --oracle-prune target; the default --oracle-prune on
has already left its dominated spells out of the dump, so every rule here is on top of it).
"""
import json
import math
import sys

TWIN_MIN = 16   # Oracle.h kTwinMin


def tcrit(n, p):   # Oracle.h TCrit
    if n < 2:
        return 1e300
    q = math.sqrt(-2.0 * math.log(min(max(p, 1e-12), 0.5)))
    z = q - (2.515517 + 0.802853 * q + 0.010328 * q * q) / (1.0 + 1.432788 * q + 0.189269 * q * q + 0.001308 * q ** 3)
    d = n - 1
    z3 = z ** 3
    z5 = z3 * z * z
    z7 = z5 * z * z
    return z + (z3 + z) / (4 * d) + (5 * z5 + 16 * z3 + 3 * z) / (96 * d * d) + (3 * z7 + 19 * z5 + 17 * z3 - 15 * z) / (384 * d ** 3)


def dominates(a, b, on_foe):   # Oracle.h Dominates (the lab's followers in a dump are not stealthy casters: that term is not replayed)
    A, B = a["sp"], b["sp"]
    area = A["area"] > 0 or A["ak"] != 0
    ka, kb = A["k"], B["k"]
    if not on_foe or area:
        if A["av"] != B["av"]:
            return False
        ka = kb = 1.0
    return (a["tg"] == b["tg"] and A["src"] == 0 and B["src"] == 0 and not A["conc"] and not B["conc"] and A["touch"] == B["touch"] and A["self"] == B["self"]
            and A["row"] == B["row"] and A["area"] == B["area"] and A["ak"] == B["ak"] and not A["ctrl"] and not B["ctrl"] and A["und"] == B["und"]
            and A["cost"] <= B["cost"] and A["sec"] <= B["sec"] and A["reach"] >= B["reach"] and A["dmg"] * ka >= B["dmg"] * kb and A["dot"] * ka >= B["dot"] * kb
            and A["dotDur"] >= B["dotDur"] and A["mp"] * ka >= B["mp"] * kb and A["spd"] * ka >= B["spd"] * kb and (A["stg"] or not B["stg"])
            and A["heal"] >= B["heal"] and A["armor"] >= B["armor"])


def prune(opts, on_foe):
    idx = [i for i, o in enumerate(opts) if o["l"] == "cast_offense" and "sp" in o]
    dead = set()
    for x, i in enumerate(idx):
        if i in dead:
            continue
        for j in idx[x + 1:]:
            if j in dead:
                continue
            if not opts[j]["iaus"] and dominates(opts[i], opts[j], on_foe):
                dead.add(j)
            elif not opts[i]["iaus"] and dominates(opts[j], opts[i], on_foe):
                dead.add(i)
                break
    return dead


def race(opts, last, skip=(), nmin=8, ceil=128, p=0.001, pair=-1.0):
    """-> (pick, rollouts). pair < 0: the full race."""
    N = len(opts)
    U = [o["u"] for o in opts]
    alive = [bool(U[i]) and i not in skip for i in range(N)]
    used = prev = 0
    n = nmin
    best = None
    u = [None] * N

    def lead(a, b, n):
        d = [U[a][k] - U[b][k] for k in range(n)]
        m = sum(d) / n
        return m, math.sqrt(max(0.0, (sum(x * x for x in d) - n * m * m) / (n - 1))) if n > 1 else 0.0

    while True:
        used += sum(n - prev for i in range(N) if alive[i])
        prev = n
        best = None
        for i in range(N):
            if alive[i]:
                u[i] = sum(U[i][:n]) / n
                if best is None or u[i] > u[best]:
                    best = i
        tc, rn, live = tcrit(n, p), math.sqrt(n), 0
        for i in range(N):
            if not alive[i]:
                continue
            if i == best:
                live += 1
                continue
            d, sd = lead(best, i, n)
            if n >= 2 and (d >= 0.0 if sd <= 0.0 else d > tc * sd / rn):
                alive[i] = False
            else:
                live += 1
        if pair >= 0.0 and n >= 2 and live > 1:
            for i in range(N - 1, -1, -1):
                if not alive[i] or i == best or i == last or opts[i]["iaus"]:
                    continue
                for j in range(N):
                    if j == i or not alive[j]:
                        continue
                    d, sd = lead(j, i, n)
                    if (d >= 0.0 and j < i and n >= TWIN_MIN) if sd <= 0.0 else (pair > 0.0 and d > tc * sd / rn - pair and d > -0.5 * pair):
                        alive[i] = False
                        live -= 1
                        break
        if live <= 1 or n >= ceil:
            break
        n = min(ceil, n * 2)
    pick = best
    if 0 <= last < N and alive[last] and u[last] is not None and u[last] >= u[best] - 0.3:   # the action he is on wins within 0.3 U
        pick = last
    return pick, used


RULES = [   # name, prune ("" = as dumped, "foe" = --oracle-prune target on top of it), race arguments
    ("the full race (as dumped)", "", {}),
    ("A  --oracle-prune target", "foe", {}),
    ("B  --oracle-pair 0 (twins only)", "", {"pair": 0.0}),
    ("C  --oracle-pair 0.10", "", {"pair": 0.10}),
    ("D  --oracle-nmin 4", "", {"nmin": 4}),
    ("A+C+D  THE TUNE (tune_driver ORACLE_CUTS)", "foe", {"pair": 0.10, "nmin": 4}),
    ("A+C+D with --oracle-pair 0.25", "foe", {"pair": 0.25, "nmin": 4}),
    ("for scale: --oracle-ceiling 64", "", {"ceil": 64}),
    ("for scale: --oracle-ceiling 32", "", {"ceil": 32}),
]


def main(argv):
    check = "--check" in argv
    files = [a for a in argv if not a.startswith("--")]
    if not files:
        print(__doc__)
        return 2
    decs = []
    for f in files:
        for line in open(f):
            try:
                decs.append(json.loads(line))
            except ValueError:
                pass   # a line cut short by a stopped run
    decs = [d for d in decs if any(len(o["u"]) >= 128 for o in d["opts"])]
    if check:
        bad = sum(1 for d in decs if race(d["opts"], -1, prune(d["opts"], False)) != (d["best"], sum(o["n0"] for o in d["opts"])))
        print("check: %d decisions, %d differ from the dump's own race" % (len(decs), bad))
        return 1 if bad else 0
    print("%d decisions, %.1f options each" % (len(decs), sum(len(d["opts"]) for d in decs) / max(1, len(decs))))
    print("%-44s %9s %6s %6s %21s %8s" % ("rule", "rollouts", "x", "same", "loss U/dec (se)", "max"))
    base = None
    for name, pr, kw in RULES:
        rows = []
        for d in decs:
            opts = d["opts"]
            m = [sum(o["u"]) / len(o["u"]) if len(o["u"]) >= 128 else None for o in opts]
            pick, used = race(opts, d["last"], prune(opts, pr == "foe") if pr else (), **kw)
            rows.append((pick, used, max(x for x in m if x is not None) - (m[pick] if m[pick] is not None else min(x for x in m if x is not None))))
        if base is None:
            base = rows
        n = len(rows)
        dl = [r[2] - b[2] for r, b in zip(rows, base)]
        md = sum(dl) / n
        se = math.sqrt(sum((x - md) ** 2 for x in dl) / max(1, n - 1) / n)
        print("%-44s %9.0f %6.2f %5.1f%% %8.4f (%+.4f %.4f) %8.2f" % (name, sum(r[1] for r in rows) / n, sum(b[1] for b in base) / max(1, sum(r[1] for r in rows)),
                                                              100.0 * sum(1 for r, b in zip(rows, base) if r[0] == b[0]) / n, sum(r[2] for r in rows) / n, md, se, max(r[2] for r in rows)))
    print("loss: the 128-stream mean of the best option minus that of the pick; in brackets the difference to the full race, and its standard error")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
