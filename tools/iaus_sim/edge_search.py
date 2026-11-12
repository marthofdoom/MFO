#!/usr/bin/env python3
"""Edge-number search for the IAUS per-class curve table (native/iaus/ClassCurves.cpp) and the shared Tuning scalars.

marth 11-12: "The sim should be used to find all the ideal edge case numbers, thats the point of a sim for iaus".
HOST-ONLY, standard library only. Drives the built simulator binary; never read by the DLL.

    tools/iaus_sim/edge_search.py --bin <sim binary> per-class [--classes a,b] [--list L.sim.json] [--out result.json]
    tools/iaus_sim/edge_search.py --bin <sim binary> global    [--params t.momentum,t.softCd] [--out result.json]
    tools/iaus_sim/edge_search.py --bin <sim binary> gates --curves "<class>:<spec> ..."      # the three 31.11 gates, class by class
    tools/iaus_sim/edge_search.py --bin <sim binary> floor --curves "..."                     # the Knight line's guard floor

METHOD (one value at a time, so every number in the table has its own evidence):
  objective   J of `--classes <slug>` (the class's own scenarios and its natural fights), 48 fights per scenario and 24 natural
              fights per seed, at the tuning target (the default roads, --stealth g3).
  train       seeds 1..N (default 48). For each candidate value the per-seed J is compared with the baseline's per-seed J on
              the SAME seeds: mean paired difference d and its standard error se.
  accept      the best value is kept only when ALL of these hold:
                1. train:   d > 2 se and d >= 0.30 J                         (it beats the noise on the seeds it was picked on)
                2. holdout: on seeds 1001..1000+N, never used to pick, d > 0 and d > 1 se   (the pick is not a selection artefact)
                3. gates:   on train + holdout seeds, `--stealth off` and the game as shipped (`--stealth off --no-fill-swing
                            --no-estimates`) lose nothing: d >= -1 se and d >= -0.5 J (Thief and Mercenary -0.25 J)
              anything else is reported as "kept: does not beat noise" and the shared value stays.
  order       per class the parameters are searched in a fixed order and each accepted value is carried into the next search.
The final table is then held to the literal gates of iaus-sim-v2.md 31.11 (`gates`) and the Knight floor (`floor`) by the caller.
"""
import argparse
import concurrent.futures as cf
import json
import math
import re
import subprocess
import sys

CONFIGS = {"g3": [], "off": ["--stealth", "off"], "shipped": ["--stealth", "off", "--no-fill-swing", "--no-estimates"]}
BETA1_TREE = "melee,ranged,mage,knight,warrior,mercenary,archer,thief,evoker,conjurer,healer,warlock".split(",")
BASE = ["melee-off", "ranged-off", "mage-off"]
CASTERS = {"mage", "evoker", "conjurer", "healer", "warlock", "melee-off", "ranged-off", "mage-off"}   # gate.spellsOnClass in the lab
KNIGHTS = {"knight", "great-knight", "ebony-warrior"}
TIGHT = {"thief", "mercenary"}   # 31.11: the two classes whose gate band is 0.5 J, not 1.0

# The per-class edge constants ClassCurves reaches. Family / consideration indexes are the catalogue's (--list-params):
# 5 drink_health, 6 drink_stamina, 7 drink_magicka, 19 block; 32 need (DrinkNeed), 88 guard_line (BoostGuardLine).
def class_params(cls):
    p = [("drinkHealth.mid", "{c}:5.32.b={v}", 0.34, [0.20, 0.25, 0.30, 0.38, 0.42, 0.46, 0.50, 0.55, 0.60, 0.70]),
         ("drinkHealth.slope", "{c}:5.32.a={v}", -11.0, [-5.0, -7.0, -9.0, -14.0, -18.0, -25.0, -40.0])]
    if cls in CASTERS:
        p.append(("drinkMagicka.mid", "{c}:7.32.b={v}", 0.34, [0.15, 0.20, 0.25, 0.30, 0.40, 0.50, 0.60]))
        p.append(("touchSpentSec", "{c}:scalar.touchSpentSec={v}", 3.0, [0.0, 1.0, 2.0, 4.0, 6.0]))
    else:
        p.append(("drinkStamina.mid", "{c}:6.32.b={v}", 0.34, [0.15, 0.20, 0.25, 0.30, 0.40, 0.50, 0.60]))
    if cls in KNIGHTS:
        p.append(("guardLine.boost", "{c}:19.88.b={v}", 2.0, [1.0, 1.25, 1.5, 2.5, 3.0, 4.0]))
        p.append(("drinkHealthLine(explicit)", "{c}:scalar.drinkHealthLine={v}", None, [0.20, 0.25, 0.30, 0.34, 0.40, 0.50]))
    return p


GLOBAL_PARAMS = {  # name: (shipped value, candidates)
    "t.momentum": (1.25, [1.0, 1.1, 1.4, 1.6]), "t.postureMomentum": (1.5, [1.0, 1.25, 2.0]), "t.preempt": (0.25, [0.1, 0.4, 0.6]),
    "t.actFloor": (0.20, [0.1, 0.15, 0.3]), "t.softCd": (4.0, [2.0, 3.0, 6.0, 8.0]), "t.occHold": (4.0, [0.0, 2.0, 6.0, 8.0]),
    "t.tauntCd": (30.0, [15.0, 20.0, 45.0]), "t.powerCd": (10.0, [5.0, 7.0, 15.0]), "t.disengageCd": (25.0, [12.0, 18.0, 35.0]),
    "t.noHealDrink": (0.4167, [0.2, 0.3, 0.55, 0.7, 1.0]), "t.noHealRetreat": (0.25, [0.15, 0.4, 0.6]),
    "t.emergencyHpHigh": (0.35, [0.25, 0.45, 0.55]), "t.emergencyHpLow": (0.10, [0.05, 0.2]), "t.groundCd": (3.0, [1.5, 6.0]),
    "t.blockCd": (2.0, [1.0, 3.0, 4.0]), "t.blockBase": (0.0, [0.25, 0.5]), "t.bashCd": (4.0, [2.0, 8.0]), "t.backOffCd": (6.0, [3.0, 9.0, 12.0]),
    "t.stepCd": (3.0, [1.5, 6.0]), "t.regroupCd": (6.0, [3.0, 12.0]), "t.kite": (0.0, [300.0, 500.0]),
}

ARGS = None
POOL = None
CACHE = {}


def run_sim(extra):
    cmd = [ARGS.bin, "--no-json", "--threads", "1"] + (["--list", ARGS.list] if ARGS.list else []) + extra
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def seeds_arg(seeds):
    return ",".join(str(s) for s in seeds)


def j_per_seed(classes, seeds, cfg, curves=(), sets=None):
    """Per-seed J of `--classes <classes>` (cached)."""
    key = (classes, tuple(seeds), cfg, tuple(curves), sets)
    if key not in CACHE:
        extra = ["--classes", classes, "--seeds", seeds_arg(seeds), "--fights", "48"] + CONFIGS[cfg]
        for c in curves:
            extra += ["--curve", c]
        if sets:
            extra += ["--set", sets]
        out = run_sim(extra)
        js = [float(m.group(1)) for m in re.finditer(r"^SEED \d+  J=([-0-9.]+)", out, re.M)]
        if len(js) != len(seeds):
            raise RuntimeError("sim gave %d seed lines for %d seeds: %s\n%s" % (len(js), len(seeds), " ".join(extra), out[-400:]))
        CACHE[key] = js
    return CACHE[key]


def paired(a, b):
    """(mean, se) of b - a."""
    d = [y - x for x, y in zip(a, b)]
    n = len(d)
    m = sum(d) / n
    v = sum((x - m) ** 2 for x in d) / (n - 1) if n > 1 else 0.0
    return m, math.sqrt(v / n)


def mean(a):
    return sum(a) / len(a)


def search_class(cls):
    train, hold = list(range(1, ARGS.seeds + 1)), list(range(1001, 1001 + ARGS.seeds))
    kept, rows = [], []   # kept = the --curve specs accepted so far
    for name, tmpl, shared, values in class_params(cls):
        base_t = j_per_seed(cls, train, "g3", kept)
        cands = []
        for v in values:
            spec = tmpl.format(c=cls, v=v)
            d, se = paired(base_t, j_per_seed(cls, train, "g3", kept + [spec]))
            cands.append((d, se, v, spec))
        cands.sort(key=lambda x: -x[0])
        d, se, v, spec = cands[0]
        row = {"class": cls, "param": name, "shared": shared, "best": v, "train_d": round(d, 3), "train_se": round(se, 3),
               "sweep": [[c[2], round(c[0], 3), round(c[1], 3)] for c in sorted(cands, key=lambda x: x[2])], "accepted": False, "why": ""}
        if not (d > 2 * se and d >= 0.30):
            row["why"] = "train: over 2 se but under the 0.30 J floor" if d > 2 * se else "train: does not beat noise"
        else:
            hd, hse = paired(j_per_seed(cls, hold, "g3", kept), j_per_seed(cls, hold, "g3", kept + [spec]))
            row["hold_d"], row["hold_se"] = round(hd, 3), round(hse, 3)
            if not (hd > 0 and hd > hse):
                row["why"] = "holdout: not confirmed"
            else:
                ok, allseeds = True, train + hold
                for cfg in ("off", "shipped"):
                    gd, gse = paired(j_per_seed(cls, allseeds, cfg, kept), j_per_seed(cls, allseeds, cfg, kept + [spec]))
                    row[cfg + "_d"], row[cfg + "_se"] = round(gd, 3), round(gse, 3)
                    if gd < -gse or gd < (-0.25 if cls in TIGHT else -0.5):
                        ok, row["why"] = False, "gate %s: loses" % cfg
                if ok and cls in KNIGHTS:
                    fl = knight_floor(cls, kept + [spec])
                    row["guard_floor"] = fl
                    if min(fl.values()) < 60.0:
                        ok, row["why"] = False, "Knight guard floor under 60%"
                if ok:
                    row["accepted"], row["why"] = True, "accepted"
                    kept.append(spec)
        rows.append(row)
        print("%-11s %-26s shared %-6s best %-6s train %+6.2f (se %.2f)%s  %s" % (
            cls, name, shared, v, d, se, ("  hold %+5.2f (se %.2f)" % (row["hold_d"], row["hold_se"])) if "hold_d" in row else "",
            row["why"] + ("  off %+.2f shipped %+.2f" % (row.get("off_d", 0), row.get("shipped_d", 0)) if "off_d" in row else "")), flush=True)
    return {"class": cls, "kept": kept, "rows": rows, "values_tried": sum(len(p[3]) for p in class_params(cls))}


def knight_floor(cls, curves):
    """Share of the melee blows that land on him under 2+ of his own taunted foes that meet his guard: blows-weighted over the
    brackets and seeds 1..6 (the measure of iaus-sim-v2.md 31.12), in the default config and as shipped."""
    out = {}
    for cfg in ("g3", "shipped"):
        num = den = 0.0
        for seed in range(1, 7):
            extra = ["--classes", cls, "--seed", str(seed), "--fights", "8", "--rating", "0"] + CONFIGS[cfg]
            for c in curves:
                extra += ["--curve", c]
            txt = run_sim(extra)
            m = re.search(r"^%s\|ALL\s+([0-9.]+)\s+([0-9.]+)" % re.escape(cls), txt, re.M)
            if m:
                num += float(m.group(1)) * float(m.group(2))
                den += float(m.group(2))
        out[cfg] = round(num / den, 1) if den else -1.0
    return out


def cmd_per_class():
    classes = ARGS.classes.split(",") if ARGS.classes else BETA1_TREE + BASE
    res = list(POOL.map(search_class, classes))
    print("\n%d (class, parameter, value) candidates tried over %d parameters: with a 2 se bar on the best of each sweep, a few train passes are expected by chance alone; the holdout is what removes them" % (sum(r["values_tried"] for r in res), sum(len(r["rows"]) for r in res)))
    print("\nACCEPTED per class (the --curve specs to carry into native/iaus/ClassCurves.cpp):")
    for r in res:
        print("  %-11s %s" % (r["class"], " ".join(r["kept"]) if r["kept"] else "(nothing beats noise: the shared values stay)"))
    if ARGS.out:
        json.dump(res, open(ARGS.out, "w"), indent=1)


def cmd_global():
    params = ARGS.params.split(",") if ARGS.params else list(GLOBAL_PARAMS)
    scope = "beta1"
    train, hold = list(range(1, ARGS.seeds + 1)), list(range(1001, 1001 + ARGS.seeds))
    curves = ARGS.curves.split() if ARGS.curves else []

    def one(name):
        shared, values = GLOBAL_PARAMS[name]
        base = j_per_seed(scope, train, "g3", curves)
        cands = []
        for v in values:
            d, se = paired(base, j_per_seed(scope, train, "g3", curves, "%s=%s" % (name, v)))
            cands.append((d, se, v))
        best = max(cands)
        row = {"param": name, "shared": shared, "best": best[2], "train_d": round(best[0], 3), "train_se": round(best[1], 3),
               "sweep": [[c[2], round(c[0], 3), round(c[1], 3)] for c in sorted(cands, key=lambda x: x[2])], "verdict": "kept: does not beat noise"}
        if best[0] > 2 * best[1] and best[0] >= 0.15:
            st = "%s=%s" % (name, best[2])
            hd, hse = paired(j_per_seed(scope, hold, "g3", curves), j_per_seed(scope, hold, "g3", curves, st))
            row["hold_d"], row["hold_se"] = round(hd, 3), round(hse, 3)
            row["verdict"] = "holdout: not confirmed"
            if hd > 0 and hd > hse:
                row["verdict"] = "BEATS NOISE at g3"
                for cfg in ("off", "shipped"):
                    gd, gse = paired(j_per_seed(scope, train + hold, cfg, curves), j_per_seed(scope, train + hold, cfg, curves, st))
                    row[cfg + "_d"], row[cfg + "_se"] = round(gd, 3), round(gse, 3)
                    if gd < -gse:
                        row["verdict"] = "beats noise at g3 but LOSES at %s" % cfg
        print("%-20s shipped %-7s best %-7s train %+5.2f (se %.2f)%s  %s   sweep %s" % (
            name, shared, best[2], best[0], best[1], ("  hold %+5.2f (se %.2f)" % (row["hold_d"], row["hold_se"])) if "hold_d" in row else "",
            row["verdict"] + ("  off %+.2f shipped %+.2f" % (row.get("off_d", 0), row.get("shipped_d", 0)) if "off_d" in row else ""),
            " ".join("%s:%+.2f" % (c[0], c[1]) for c in row["sweep"])), flush=True)
        return row
    res = list(POOL.map(one, params))
    if ARGS.out:
        json.dump(res, open(ARGS.out, "w"), indent=1)


def cmd_gates():
    """The three gates of iaus-sim-v2.md 31.11, literally: seeds 1..6, 24 fights per scenario, class by class and the two sets."""
    curves = ARGS.curves.split() if ARGS.curves else []
    seeds = "1,2,3,4,5,6"

    def j(classes, cfg, cv):
        extra = ["--classes", classes, "--seeds", seeds] + CONFIGS[cfg]
        for c in cv:
            extra += ["--curve", c]
        if ARGS.sets:
            extra += ["--set", ARGS.sets]
        m = re.search(r"HEADLINE  J mean=([-0-9.]+)", run_sim(extra))
        return float(m.group(1))
    names = BETA1_TREE + BASE + ["beta1-tree", "beta1"]
    jobs = [(n, cfg, tuple(cv)) for n in names for cfg in CONFIGS for cv in ([], curves)]
    vals = dict(zip(jobs, POOL.map(lambda a: j(a[0], a[1], list(a[2])), jobs)))
    print("%-12s %s" % ("class", "   ".join("%-8s old    new     d" % c for c in CONFIGS)))
    bad = []
    for n in names:
        line = "%-12s" % n
        for cfg in CONFIGS:
            o, w = vals[(n, cfg, ())], vals[(n, cfg, tuple(curves))]
            band = 0.0 if n.startswith("beta1") else (0.5 if n in TIGHT else 1.0)
            flag = "" if w >= o - band - 1e-9 else " FAIL"
            if flag:
                bad.append((n, cfg))
            line += "   %8s %6.2f %6.2f %+5.2f%s" % ("", o, w, w - o, flag)
        print(line)
    print("GATES %s (band: a class may not fall more than 1.0 J under its old value, Thief and Mercenary 0.5; a set mean may not fall)" % ("PASS" if not bad else "FAIL " + str(bad)))
    return 0 if not bad else 1


def cmd_floor():
    curves = ARGS.curves.split() if ARGS.curves else []
    ok = True
    for cls in sorted(KNIGHTS):
        own = [c for c in curves]
        o, w = knight_floor(cls, []), knight_floor(cls, own)
        print("%-14s guard under 2+ own-taunted foes: default %5.1f -> %5.1f   as shipped %5.1f -> %5.1f" % (cls, o["g3"], w["g3"], o["shipped"], w["shipped"]))
        ok &= min(w.values()) >= 60.0
    print("KNIGHT FLOOR (>= 60%%) %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    global ARGS, POOL
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["per-class", "global", "gates", "floor"])
    ap.add_argument("--bin", required=True, help="the built simulator (run.sh's SIM_OUT)")
    ap.add_argument("--list", help="a <list>.sim.json (test input only); default the built-in world")
    ap.add_argument("--classes", help="comma list (per-class); default the 12 Beta 1 classes + the 3 base cells")
    ap.add_argument("--params", help="comma list (global); default every Tuning scalar of GLOBAL_PARAMS")
    ap.add_argument("--curves", help="space-separated --curve specs to apply (gates, floor, global)")
    ap.add_argument("--sets", help="a --set string to apply (gates)")
    ap.add_argument("--seeds", type=int, default=48, help="seeds in the train set and in the holdout set (default 48)")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--out")
    ARGS = ap.parse_args()
    POOL = cf.ThreadPoolExecutor(max_workers=ARGS.jobs)
    return {"per-class": cmd_per_class, "global": cmd_global, "gates": cmd_gates, "floor": cmd_floor}[ARGS.mode]() or 0


if __name__ == "__main__":
    sys.exit(main())
