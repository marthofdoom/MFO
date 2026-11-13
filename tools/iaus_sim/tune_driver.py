#!/usr/bin/env python3
"""The whole per-class IAUS curve tuning, unattended and resumable (marth 11-12: "The sim should be used to find all the ideal edge
case numbers"; "If fable thinks it can save tokens by building a script that can tune, it should"). HOST-ONLY, standard library only.

    tools/iaus_sim/tune_driver.py --bin <sim binary> --out <dir> [--smoke] [--classes a,b] [--worlds builtin,LoreRim,Tuxborn,Vanilla]
                                  [--stages prep,rejudge,classes,joint,edge,globals,consts,floor,rating,recalib,summary] [--jobs 12]

Runbook, verdicts and the decisions this script may NOT take: Docs/research/2026-11/iaus-sim-v2.md section 33.

RESUME    every simulator result is appended to <out>/cache.jsonl the moment it exists and every stage is a pure function of the
          cache, so running the SAME command again continues where it died (finished work replays in seconds). <out>/progress.log
          has one line per finished step; <out>/summary.json and <out>/summary.md are rewritten after every stage.
WORLDS    built-in (no list data) and the lists of ~/.cache/mfo-list-balance (test input only). Every world prices a potion with ITS
          OWN potion-worth table (potion_worth.py; the built-in one is tools/iaus_sim/PotionWorth.h).
STAGES    prep      the world inputs exist, each list's potion-worth table is measured.
          rejudge   values an earlier round left open, re-judged under RULE (default: the Warrior stamina-drink midpoint 0.15).
          classes   per class: every knob the class table can move (curve parameters, Piece points, consideration weights, family
                    weights, edge scalars) -> liveness screen -> coordinate descent over the live ones, most sensitive first, an
                    accepted value carried into the next sweep -> holdout -> many-seed gates -> fresh-seed cross-world confirmatory
                    -> Knight guard floor. The T1 rows (melee, ranged, mage) are searched first and their accepted values are
                    every later class's baseline. One dashboard results file per finished sweep.
          joint     the whole final table against the shipped one on all 15 cells (128 seeds, three configs), and each T1 value
                    left out in turn. A failure is an open decision: no value is written.
          edge      marth "Widen to x4 / x0.25": only knobs whose accepted value sits at the edge of the range tried are searched
                    again past it, same RULE; a wider value that passes is an open decision (never shipped).
          globals   the shared Tuning winners of the earlier round, confirmed on fresh seeds on every world with the per-class gate
                    and the decision-switching rate next to J.
          consts    the compile-time constants never probed (const_probe.sh variants).
          floor     the Knight line's guard floor per world with the final table, and why the unguarded blows found no guard.
          rating    the rating battery per world with the final table, classes under their tier flagged.
          recalib   the built-in potion-worth table re-measured under the accepted values; each accepted value confirmed again with it.
          summary   summary.json + summary.md.

RULE (fixed before any result of this driver was read; a number is ACCEPTED only when every line holds):
  train        built-in world, the default roads (g3), seeds 1..N: the best value of a sweep has paired d > 2.5 se and d >= 0.15 J.
  holdout      seeds 1001.., never used to pick: d > 1.5 se.
  gates        marth, Q-gate-rule "Gate on many seeds": judged on the train + holdout seeds, never on a six-seed set. With stealth
               off and as shipped, no class may get WORSE, and a drop counts only beyond noise: worse = d < -2 se and d < -0.05 J
               (when the 15 cells are judged at once, -2.5 se); and never more than 0.5 J down (Thief, Mercenary 0.25) when
               that drop is itself outside one se. A T1 row (melee / ranged / mage) is also the companion in other classes' fights, so its value is gated
               on all 15 cells at all three configs; any other row moves its own class only.
  cross-world  fresh seeds 4001.. on every world: the equal-weight mean of the worlds' d is > 2 se, no world is worse (d < -2 se),
               and as shipped the mean over the worlds is not worse and inside the band. A list's actors are test input: a value
               that is best on one list only is never accepted.
  joint        the whole final table against the shipped one on all 15 cells, on the train + holdout seeds, at all three configs:
               no cell worse (the 15-at-once bar), no 15-cell mean worse, and no T1 value whose removal makes the table better.
  knight       the Knight line's guard floor (>= 60% under two or more own-taunted foes, measured on 36 seeds) holds on every
               world, default and as shipped; where the shipped table is already under it, the value may not lower it by more than 0.5.
  fixed        marth's School Circle (floor 0.35, shape 1.5, mastery bump 0.15, order, home schools) is never a knob. Pure 0 / 1
               gates and Step vetoes are rules, never knobs. A knob never moves outside its valid range (no negative offset, floor
               or level; a boost never under 1; a Piece point at 0 stays a veto and no other point may reach 0).
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import math
import os
import re
import subprocess
import sys
import threading
import time

INF = float("inf")
CONFIGS = {"g3": [], "off": ["--stealth", "off"], "shipped": ["--stealth", "off", "--no-fill-swing", "--no-estimates"]}
BETA1_TREE = "melee,ranged,mage,knight,warrior,mercenary,archer,thief,evoker,conjurer,healer,warlock".split(",")
BASE = ["melee-off", "ranged-off", "mage-off"]
CELLS = BETA1_TREE + BASE
T1 = {"melee", "ranged", "mage"}                 # also the companions in every other class's fights
KNIGHTS = ["knight", "great-knight", "ebony-warrior"]
TIGHT = {"thief", "mercenary"}
CACHE_DIR = os.path.expanduser("~/.cache/mfo-list-balance/")
HERE = os.path.dirname(os.path.abspath(__file__))

FULL = dict(probe_seeds=6, train=64, hold=64, confirm=96, max_knobs=250, passes=2, min_probe=0.0, global_seeds=96, list_global_seeds=48,
            const_seeds=48, floor_seeds=36, rating_seeds=6, calib_seeds=24, calib_fights=400, list_calib_seeds=12, force=False)
SMOKE = dict(probe_seeds=2, train=4, hold=4, confirm=4, max_knobs=4, passes=1, min_probe=0.0, global_seeds=4, list_global_seeds=2,
             const_seeds=2, floor_seeds=2, rating_seeds=1, calib_seeds=2, calib_fights=40, list_calib_seeds=1, force=True)

A = None            # parsed arguments
CFG = None          # FULL or SMOKE
POOL = None
EMIT = cf.ThreadPoolExecutor(max_workers=1)   # the dashboard files
CACHE = {}          # (key, seed) -> (J, switches, minutes)
TEXT = {}           # key -> stdout (rating / floor / list-uses runs)
LOCK = threading.Lock()
SUMMARY = {"rule": __doc__[__doc__.index("RULE ("):].strip(), "open_decisions": [], "stages": {}}


# ── plumbing ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────
def log(msg):
    line = "%s  %s" % (time.strftime("%m-%d %H:%M:%S"), msg)
    print(line, flush=True)
    with open(os.path.join(A.out, "progress.log"), "a") as f:
        f.write(line + "\n")


_HASH = {}


def sha(path):
    """sha256 of a file (first 16 hex), re-read only when its size or mtime changed. A missing file is an error, never an empty hash."""
    st = os.stat(path)
    k = (path, st.st_size, st.st_mtime_ns)
    if k not in _HASH:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for blk in iter(lambda: f.read(1 << 20), b""):
                h.update(blk)
        _HASH[k] = h.hexdigest()[:16]
    return _HASH[k]


def ident(world, binary=None):
    """EVERYTHING a result of this world depends on besides its arguments: the simulator binary (PotionWorth.h and every curve are
    compiled into it), the world's .sim.json and .combatmods.json, and its potion-worth table. Part of every cache key, so a result
    measured on another binary, another list extract or another potion table is never replayed as this one's."""
    out = ["bin:" + sha(binary or A.bin)]
    if world == "builtin":
        if A.builtin_worth:
            out.append("worth:" + sha(A.builtin_worth))
    elif world is not None:
        check_world(world)
        out += ["sim:" + sha(CACHE_DIR + world + ".sim.json"), "mods:" + sha(CACHE_DIR + world + ".combatmods.json"), "worth:" + sha(os.path.join(A.out, "potworth-%s.txt" % world))]
    return "|".join(out)


def check_world(world, need_worth=True):
    """Review F8: EVERY stage that reads a list checks its companion files here (world_args and ident call it), not only stage_prep.
    The simulator reads <list>.combatmods.json BESIDE the .sim.json and takes "absent" for "no such mod", so a missing file would
    silently drop the list's combat mods; a missing potion-worth table would be refused by the simulator only later."""
    if world == "builtin":
        return
    for f in (CACHE_DIR + world + ".sim.json", CACHE_DIR + world + ".combatmods.json"):
        if not os.path.exists(f):
            raise SystemExit("missing %s (tools/list_balance/compile_world.py / extract_combat_mods.py): a list is never run without its companion files" % f)
    if need_worth and not os.path.exists(os.path.join(A.out, "potworth-%s.txt" % world)):
        raise SystemExit("missing %s: run the prep stage on this --out first (it measures each list's potion worth)" % os.path.join(A.out, "potworth-%s.txt" % world))


def world_args(world):
    check_world(world)
    if world == "builtin":
        return ["--potion-worth", A.builtin_worth] if A.builtin_worth else []
    return ["--list", CACHE_DIR + world + ".sim.json", "--potion-worth", os.path.join(A.out, "potworth-%s.txt" % world)]


def key_of(world, classes, cfg, curves, sets, binary):
    return json.dumps([world, classes, cfg, sorted(curves), sets or "", ident(world, binary)], separators=(",", ":"))


def sim_cmd(world, classes, cfg, curves, sets, binary):
    cmd = [binary or A.bin, "--no-json", "--threads", "1", "--print-switch", "--classes", classes, "--fights", "48"] + world_args(world) + CONFIGS[cfg]
    for c in curves:
        cmd += ["--curve", c]
    if sets:
        cmd += ["--set", sets]
    return cmd


def run_chunk(k, cmd, seeds):
    r = subprocess.run(cmd + ["--seeds", ",".join(str(s) for s in seeds)], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("the simulator failed (exit %d): %s\n%s\n%s" % (r.returncode, " ".join(cmd), r.stdout[-400:], r.stderr[-400:]))
    out = r.stdout
    js = {int(m.group(1)): float(m.group(2)) for m in re.finditer(r"^SEED (\d+)  J=([-0-9.]+)", out, re.M)}
    sw = {int(m.group(1)): (float(m.group(2)), float(m.group(3))) for m in re.finditer(r"^SWITCH (\d+) (\d+) ([0-9.]+)", out, re.M)}
    if set(js) != set(seeds) or set(sw) != set(seeds):
        raise RuntimeError("sim gave seeds %s for %s: %s\n%s" % (sorted(js), seeds, " ".join(cmd), out[-600:]))
    with LOCK:
        with open(os.path.join(A.out, "cache.jsonl"), "a") as f:
            for s in seeds:
                v = (js[s], sw.get(s, (0.0, 0.0))[0], sw.get(s, (0.0, 0.0))[1])
                CACHE[(k, s)] = v
                f.write(json.dumps({"k": k, "s": s, "v": v}) + "\n")


def jmany(reqs):
    """reqs: list of (world, classes, cfg, curves, sets, binary, seeds). Runs what the cache lacks, all at once; returns per-request
    lists of (J, switches, minutes), in seed order."""
    futs = []
    for world, classes, cfg, curves, sets, binary, seeds in reqs:
        k = key_of(world, classes, cfg, curves, sets, binary)
        miss = [s for s in seeds if (k, s) not in CACHE]
        cmd = sim_cmd(world, classes, cfg, curves, sets, binary)
        for i in range(0, len(miss), 8):
            futs.append(POOL.submit(run_chunk, k, cmd, miss[i:i + 8]))
    for f in futs:
        f.result()
    return [[CACHE[(key_of(w, c, g, cv, st, b), s)] for s in seeds] for w, c, g, cv, st, b, seeds in reqs]


def req(world, classes, cfg, curves, seeds, sets=None, binary=None):
    return (world, classes, cfg, tuple(curves), sets, binary, list(seeds))


def text_run(key, cmd, world=None):
    """A cached text run. The key carries the identity of the binary and the world (ident). A run that fails is NEVER cached and
    never read as an empty result: it stops the driver with the command and the simulator's own message."""
    key = key + "#" + ident(world, cmd[0])   # cmd[0] is always the simulator binary that runs
    if key not in TEXT:
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("the simulator failed (exit %d): %s\n%s\n%s" % (r.returncode, " ".join(cmd), r.stdout[-400:], r.stderr[-400:]))
        out = r.stdout
        with LOCK:
            TEXT[key] = out
            with open(os.path.join(A.out, "cache.jsonl"), "a") as f:
                f.write(json.dumps({"t": key, "v": out}) + "\n")
    return TEXT[key]


def load_cache():
    p = os.path.join(A.out, "cache.jsonl")
    if os.path.exists(p):
        for line in open(p):
            try:
                r = json.loads(line)
            except ValueError:
                continue   # a line cut off by a kill: that result is simply run again
            if "k" in r:
                CACHE[(r["k"], r["s"])] = tuple(r["v"])
            elif "t" in r:
                TEXT[r["t"]] = r["v"]


def paired(a, b):
    """(mean, se) of b - a over the J of two same-seed runs."""
    d = [y[0] - x[0] for x, y in zip(a, b)]
    n = len(d)
    m = sum(d) / n
    v = sum((x - m) ** 2 for x in d) / (n - 1) if n > 1 else 0.0
    return m, math.sqrt(v / n)


def sw_rate(a):
    mins = sum(x[2] for x in a)
    return sum(x[1] for x in a) / mins if mins > 0 else 0.0


def worse(d, se, many=False):
    """Q-gate-rule: a drop counts only beyond noise."""
    return d < -(2.5 if many else 2.0) * se and d < -0.05


def band(cls):
    return 0.25 if cls in TIGHT else 0.5


def out_of_band(cls, d, se, k=1.0):
    """The hard band of 31.11 (0.5 J; Thief and Mercenary 0.25), held to Q-gate-rule too: the drop must itself be outside one se."""
    return d < -k * band(cls) and d < -se


def emit_run(classes, label, curves=(), sets=None):
    """ONE normal results file per finished sweep, so the dashboard shows the search moving (small on purpose)."""
    if not A.emit_runs:
        return
    if A.smoke:
        label = "SMOKE TEST, NOT A RESULT: " + label
    cmd = [A.bin, "--threads", "2", "--classes", classes, "--fights", "4", "--class-oracle", "2", "--gap-scenarios", "1", "--label", label]
    for c in curves:
        cmd += ["--curve", c]
    if sets:
        cmd += ["--set", sets]
    if "emit:" + label in TEXT:
        return   # a resumed run replays its sweeps from the cache: the dashboard file of a sweep is written once
    with LOCK:
        TEXT["emit:" + label] = ""
        with open(os.path.join(A.out, "cache.jsonl"), "a") as f:
            f.write(json.dumps({"t": "emit:" + label, "v": ""}) + "\n")
    EMIT.submit(subprocess.run, cmd, capture_output=True, text=True)   # one at a time (the run id is a timestamp), never in the search's way


def save_summary():
    json.dump(SUMMARY, open(os.path.join(A.out, "summary.json"), "w"), indent=1)
    open(os.path.join(A.out, "summary.md"), "w").write(render_md())


def open_decision(what, evidence):
    SUMMARY["open_decisions"].append({"what": what, "evidence": evidence})
    log("OPEN DECISION (the runner stops and reports this, it does not decide): %s | %s" % (what, evidence))


# ── knobs ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
def uniq(vals, shared):
    out = []
    for v in vals:
        v = round(v, 5)
        if abs(v - shared) > 1e-6 and v not in out:
            out.append(v)
    return out


def logi_mid(b, mults, steps):
    """A Logi midpoint's candidates: scaled when |b| >= 0.2, else stepped; a midpoint never crosses 0 (review F7: a negative midpoint
    used to clamp to +0.02, which flipped its sign). b = 0 has no sign to keep and steps to the positive side, as before."""
    if abs(b) >= 0.2:
        return [b * k for k in mults]
    if b < 0.0:
        return [min(-0.02, b - s) for s in steps]   # mirrored: the same distances, on the negative side
    return [max(0.02, b + s) for s in steps]


def all_knobs():
    """Every knob a class row can carry, each with its candidate values INSIDE its valid range. From the simulator's --list-uses."""
    txt = text_run("list-uses", [A.bin, "--list-uses"])
    knobs, seen, gates, pieces = [], set(), 0, {}
    if txt.count("\nUSE ") < 50 or "\nFAMILY " not in txt:
        raise RuntimeError("--list-uses printed no catalogue (a simulator without the class-tuning surface?)")
    for line in txt.splitlines():
        p = line.split()
        if p and p[0] == "PIECE":
            pieces[(int(p[1]), int(p[2]))] = [(float(p[4 + 2 * i]), float(p[5 + 2 * i])) for i in range(int(p[3]))]
    for line in txt.splitlines():
        p = line.split()
        if len(p) == 10 and p[0] == "USE":
            f, fn, c, cn, kind, a, b, fl, w = int(p[1]), p[2], int(p[3]), p[4], p[5], float(p[6]), float(p[7]), float(p[8]), float(p[9])
            if (f, c) in seen:
                continue
            seen.add((f, c))
            name, pre = "%s.%s" % (fn, cn), "{c}:%d.%d." % (f, c)
            def add(suffix, param, shared, vals, meta):
                if uniq(vals, shared):
                    knobs.append((name + suffix, pre + param + "={v}", shared, uniq(vals, shared)))
                    KNOB_META[name + suffix] = meta
            if kind == "Bin":
                if {a, b} <= {0.0, 1.0}:
                    gates += 1   # a pure 0 / 1 gate is a rule
                else:
                    for suffix, param, v in ((".lo", "a", a), (".hi", "b", b)):
                        if v > 1.0:      # a boost: never under 1
                            add(suffix, param, v, [1.0, 1.0 + 0.5 * (v - 1.0), 1.0 + 1.5 * (v - 1.0), 1.0 + 2.5 * (v - 1.0)], ("add", 1.0, INF, False))
                        elif 0.0 < v < 1.0:   # a damped level: never 0 (a veto), never over 1
                            add(suffix, param, v, [max(0.02, v * 0.5), v * 0.75, min(1.0, v * 1.33), min(1.0, v * 1.67)], ("mul", 0.02, 1.0, False))
            elif kind == "Piece":
                pts = pieces.get((f, c), [])
                top = max([1.0] + [y for _, y in pts])
                for i, (x, y) in enumerate(pts):
                    if y > 0.0:          # a point at 0 is a veto and stays one; no other point may reach 0
                        add(".y%d" % i, "py%d" % i, y, [min(top, max(0.02, y + s)) for s in (-0.3, -0.15, 0.15, 0.3)], ("add", 0.02, top, False))
                    if 0 < i < len(pts) - 1:   # an interior point's x, a quarter of the way to either neighbour (the order is kept)
                        add(".x%d" % i, "px%d" % i, x, [x - 0.25 * (x - pts[i - 1][0]), x + 0.25 * (pts[i + 1][0] - x)], ("add", pts[i - 1][0], pts[i + 1][0], True))
            else:
                if kind == "Lin":
                    add(".a", "a", a, [a * k for k in (0.5, 0.75, 1.33, 2.0)], ("mul", -INF, INF, False))
                    add(".b", "b", b, [max(min(b, 0.0), b + s) for s in (-0.2, -0.1, 0.1, 0.2)], ("add", min(b, 0.0), INF, False))   # an offset is never pushed under 0
                elif kind == "Logi":
                    add(".a", "a", a, [a * k for k in (0.5, 0.75, 1.33, 2.0)], ("mul", -INF, INF, False))
                    add(".b", "b", b, logi_mid(b, (0.6, 0.8, 1.25, 1.6), (-0.1, -0.05, 0.05, 0.1, 0.2)), ("mul", -INF, INF, False) if abs(b) >= 0.2 else ("add", -INF, -0.02, False) if b < 0.0 else ("add", 0.02, INF, False))
                else:   # Pow / Inv: the exponent, always > 0
                    add(".a", "a", a, [a * k for k in (0.5, 0.75, 1.33, 2.0)], ("mul", -INF, INF, False))
                if fl > 0.0:
                    add(".floor", "floor", fl, [max(0.01, fl * 0.5), min(1.0, fl * 1.5)], ("mul", 0.01, 1.0, False))
            if kind != "Bin" and w > 0.0:
                add(".w", "w", w, [w * k for k in (0.5, 0.75, 1.5, 2.0)], ("mul", -INF, INF, False))   # the consideration's exponent for this class (inert on a gate or a boost: the liveness screen drops it)
        elif len(p) == 4 and p[0] == "FAMILY" and float(p[3]) > 0.0 and p[2] not in ("use_scroll", "equip"):
            knobs.append(("family." + p[2], "{c}:family.%s={v}" % p[1], 1.0, [0.75, 0.87, 1.15, 1.33]))   # the class's multiplier on a whole family: small on purpose (the class card)
            KNOB_META["family." + p[2]] = ("mul", -INF, INF, False)
    return knobs, gates


KNOB_META = {}   # name -> (mode "mul" | "add", lo, hi, strict): how all_knobs moved the knob and its valid range; the edge stage widens inside it


def class_knobs(cls, knobs):
    out = list(knobs)
    if cls in KNIGHTS:
        out.append(("scalar.drinkHealthLine", "{c}:scalar.drinkHealthLine={v}", 0.34, [0.20, 0.25, 0.30, 0.40, 0.50]))
        KNOB_META["scalar.drinkHealthLine"] = ("add", 0.02, 0.98, False)
    return out


# ── the acceptance path (one place: sweeps, re-judgements and re-confirmations all go through it) ────────────────────────────
def knight_floor(world, cls, curves, sets=None):
    out = {}
    for cfg in ("g3", "shipped"):
        num = den = 0.0
        why = [0.0] * 6
        def one(seed):
            cmd = [A.bin, "--no-json", "--threads", "1", "--classes", cls, "--seed", str(seed), "--fights", "8", "--rating", "0", "--guard-why", "--baseline", "/dev/stdout"] + world_args(world) + CONFIGS[cfg]
            for c in curves:
                cmd += ["--curve", c]
            if sets:
                cmd += ["--set", sets]
            return text_run("floor:" + json.dumps(cmd[1:]), cmd, world)
        for txt in list(POOL.map(one, range(1, CFG["floor_seeds"] + 1))):   # all seeds at once; a failed run raises here
            rows = {}
            for m in re.finditer(r"^%s\|([^|]+)\|(guard2%%|inc2/f|ung2[a-z]+%%)=([-0-9.e]+)" % re.escape(cls), txt, re.M):
                rows.setdefault(m.group(1), {})[m.group(2)] = float(m.group(3))
            for r in rows.values():
                n = r.get("inc2/f", 0.0)
                num += r.get("guard2%", 0.0) * n
                den += n
                for i, nm in enumerate(("ung2stagger%", "ung2rest%", "ung2hurt%", "ung2drink%", "ung2busy%", "ung2free%")):
                    why[i] += r.get(nm, 0.0) * n
        if not den > 0.0:   # no blow under two or more own-taunted foes in any fight: the tally measured nothing, and "nothing" must never read as a floor that holds
            raise RuntimeError("Knight floor: %s on %s (%s) has no blow under 2+ own-taunted foes in %d seeds: the tally is empty (wrong class, a broken taunt road, or a changed output format)" % (cls, world, cfg, CFG["floor_seeds"]))
        out[cfg] = round(num / den, 1)
        out[cfg + "_why"] = dict(zip(("staggered", "rest_after_hold", "hurt_drinks_first", "drinking", "mid_action", "free"), [round(x / den, 1) for x in why]))
    return out


def judge(cls, kept, spec, train_d=None, forced=False, fresh0=4001, sets=None, base_sets=None, replace=None):
    """Holdout, many-seed gates, cross-world confirmatory, Knight floor for ONE candidate (a --curve spec, or a --set string in `sets`)
    on top of the kept specs. Returns the row; row["accepted"] is the verdict. forced (smoke runs, re-judgements): every step is
    run and reported even after one fails, and the verdict is still the rule's."""
    cur = [k for k in kept if k != replace] + ([spec] if spec else [])   # replace: the edge stage moves an accepted value, never stacks two specs on one parameter
    row = {"class": cls, "spec": spec or sets, "accepted": False, "why": "", "steps": {}}
    fails = []
    hold = range(1001, 1001 + CFG["hold"])
    b, v = jmany([req("builtin", cls, "g3", kept, hold, base_sets), req("builtin", cls, "g3", cur, hold, sets)])
    hd, hse = paired(b, v)
    row["steps"]["holdout"] = [round(hd, 3), round(hse, 3)]
    if not hd > 1.5 * hse:
        fails.append("holdout: not confirmed")
    if fails and not forced:
        row["why"] = fails[0]
        return row
    # gates, on the train + holdout seeds (Q-gate-rule: many seeds)
    seeds = list(range(1, CFG["train"] + 1)) + list(hold)
    cells = CELLS if cls in T1 or cls == "beta1" else [cls]
    cfgs = ("g3", "off", "shipped") if len(cells) > 1 else ("off", "shipped")
    reqs = [req("builtin", c, g, kept if x == 0 else cur, seeds, base_sets if x == 0 else sets) for c in cells for g in cfgs for x in (0, 1)]
    res = jmany(reqs)
    gate = {}
    for i, (c, g) in enumerate([(c, g) for c in cells for g in cfgs]):
        d, se = paired(res[2 * i], res[2 * i + 1])
        gate["%s/%s" % (c, g)] = [round(d, 3), round(se, 3)]
        if worse(d, se, len(cells) > 1) or out_of_band(c, d, se):
            fails.append("gate: %s gets worse at %s (%+.2f, se %.2f)" % (c, g, d, se))
    row["steps"]["gates"] = gate
    if fails and not forced:
        row["why"] = fails[0]
        return row
    # cross-world confirmatory, fresh seeds
    fresh = range(fresh0, fresh0 + CFG["confirm"])
    target = cls if cls != "beta1" else None
    conf = {}
    for g in ("g3", "shipped"):
        ds = []
        for w in A.worlds:
            if target:
                b, v = jmany([req(w, target, g, kept, fresh, base_sets), req(w, target, g, cur, fresh, sets)])
                d, se = paired(b, v)
            else:   # a global: the mean over the 15 cells
                res = jmany([req(w, c, g, kept if x == 0 else cur, fresh, base_sets if x == 0 else sets) for c in CELLS for x in (0, 1)])
                per = [[y[0] - x[0] for x, y in zip(res[2 * i], res[2 * i + 1])] for i in range(len(CELLS))]
                dd = [sum(col) / len(col) for col in zip(*per)]
                d = sum(dd) / len(dd)
                se = math.sqrt(sum((x - d) ** 2 for x in dd) / (len(dd) - 1) / len(dd)) if len(dd) > 1 else 0.0
            conf["%s/%s" % (w, g)] = [round(d, 3), round(se, 3)]
            ds.append((d, se))
            if g == "g3" and worse(d, se):
                fails.append("cross-world: worse on %s (%+.2f, se %.2f)" % (w, d, se))
        md = sum(x[0] for x in ds) / len(ds)
        mse = math.sqrt(sum(x[1] ** 2 for x in ds)) / len(ds)
        conf["mean/" + g] = [round(md, 3), round(mse, 3)]
        if g == "g3" and not md > 2.0 * mse:
            fails.append("cross-world: the mean over the worlds does not beat noise (%+.2f, se %.2f)" % (md, mse))
        if g == "shipped" and (worse(md, mse) or out_of_band(cls, md, mse)):
            fails.append("cross-world: worse as shipped (%+.2f, se %.2f)" % (md, mse))
    row["steps"]["cross_world"] = conf
    if fails and not forced:
        row["why"] = fails[0]
        return row
    if cls in KNIGHTS or cls in ("beta1", "melee"):   # the Knight line's floor (a T1 melee row and a global can move the Knight's fights too)
        fl = {}
        for kc in ([cls] if cls in KNIGHTS else ["knight"]):
            for w in A.worlds:
                o, n = knight_floor(w, kc, kept, base_sets), knight_floor(w, kc, cur, sets)
                for g in ("g3", "shipped"):
                    fl["%s/%s/%s" % (kc, w, g)] = [o[g], n[g]]
                    if n[g] < 60.0 and (o[g] >= 60.0 or n[g] < o[g] - 0.5):
                        fails.append("Knight guard floor: %s on %s %s %.1f -> %.1f" % (kc, w, g, o[g], n[g]))
        row["steps"]["knight_floor"] = fl
    row["accepted"] = not fails
    row["why"] = "ACCEPTED: holdout, many-seed gates, cross-world on fresh seeds" if not fails else "; ".join(fails)
    if train_d is not None:
        row["train"] = train_d
    return row


def measure_worth(tab, world, seeds, curves=()):
    """A potion-worth table, measured once and reused ONLY while everything it was measured from is the same (the binary, the world's
    .sim.json and .combatmods.json, the seeds and fights, the curves): the stamp beside it says what that was."""
    src = None if world == "builtin" else CACHE_DIR + world + ".sim.json"
    stamp = json.dumps(["bin:" + sha(A.bin)] + ([] if src is None else ["sim:" + sha(src), "mods:" + sha(CACHE_DIR + world + ".combatmods.json")]) + [seeds, CFG["calib_fights"], sorted(curves)])
    if os.path.exists(tab) and os.path.exists(tab + ".inputs") and open(tab + ".inputs").read() == stamp:
        return
    log("measuring the potion worth of %s%s" % (world, " (its inputs changed: the table on disk is not reused)" if os.path.exists(tab) else ""))
    cmd = [sys.executable, os.path.join(HERE, "potion_worth.py"), "--bin", A.bin, "--seeds", str(seeds), "--fights", str(CFG["calib_fights"]), "--jobs", str(A.jobs), "--out", tab + ".tmp"]
    if src:
        cmd += ["--list", src]
    if curves:
        cmd += ["--curves", " ".join(curves)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("potion_worth.py failed: %s\n%s" % (" ".join(cmd), (r.stdout + r.stderr)[-600:]))
    os.replace(tab + ".tmp", tab)
    open(tab + ".inputs", "w").write(stamp)


# ── stages ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────
def stage_prep():
    st = {"worlds": {}}
    hdr = open(os.path.join(HERE, "PotionWorth.h")).read()
    if hdr.count('{ "') < 20 and not A.builtin_worth:
        raise SystemExit("tools/iaus_sim/PotionWorth.h is not calibrated: run `tools/iaus_sim/potion_worth.py --bin %s --out <file> --header`, rebuild the simulator, start again" % A.bin)
    for w in A.worlds:
        if w == "builtin":
            st["worlds"][w] = "PotionWorth.h" if not A.builtin_worth else A.builtin_worth
            continue
        check_world(w, need_worth=False)   # the table is measured just below
        tab = os.path.join(A.out, "potworth-%s.txt" % w)
        measure_worth(tab, w, CFG["list_calib_seeds"])
        st["worlds"][w] = tab
    chk = text_run("print-classes", [A.bin, "--print-classes"])
    st["school_circle_and_armor"] = {"mercenary_light": bool(re.search(r"T2 mercenary .* light", chk)), "warrior_heavy": bool(re.search(r"T2 warrior .* heavy", chk)),
                                     "archer_light": bool(re.search(r"T2 archer .* light", chk)), "thief_light": bool(re.search(r"T2 thief .* light", chk))}
    if not all(st["school_circle_and_armor"].values()):
        raise SystemExit("the armor model is not the class cards': %s" % st["school_circle_and_armor"])
    st["identity"] = {w: ident(w) for w in A.worlds}   # what every cached result of this run is keyed on
    SUMMARY["stages"]["prep"] = st
    log("prep: done %s" % st["identity"])


def stage_rejudge():
    rows = []
    for spec in A.rejudge.split():
        cls = spec.split(":")[0]
        train = range(1, CFG["train"] + 1)
        b, v = jmany([req("builtin", cls, "g3", [], train), req("builtin", cls, "g3", [spec], train)])
        d, se = paired(b, v)
        row = judge(cls, [], spec, train_d=[round(d, 3), round(se, 3)], forced=True, fresh0=8001)
        if not (d > 2.5 * se and d >= 0.15):
            row["accepted"] = False
            row["why"] = "train: does not beat noise under RULE (%+.2f, se %.2f); " % (d, se) + row["why"]
        rows.append(row)
        log("rejudge %s: %s" % (spec, row["why"]))
    SUMMARY["stages"]["rejudge"] = rows


def search_class(cls, knobs):
    probe = range(1, CFG["probe_seeds"] + 1)
    train = range(1, CFG["train"] + 1)
    kn = class_knobs(cls, knobs)
    # THE COMBINED TABLE: a T1 row (melee / ranged / mage) is the companion in every other class's fights, so the T1 rows are searched
    # FIRST and their accepted values are part of every later class's baseline (`base`): a later value is found, gated and confirmed in
    # the world the final table will make, not in the shipped one. The joint stage then gates the whole final table once more.
    done_cls = SUMMARY["stages"].get("classes", {})
    base = [k for t in sorted(T1) if t != cls and done_cls.get(t, {}).get("complete") for k in done_cls[t]["kept"]]
    # LIVENESS: the two outermost values of every knob on a few seeds; a knob that changes no seed's J is dead for this class
    reqs = [req("builtin", cls, "g3", base, probe)]
    for name, tmpl, shared, vals in kn:
        for v in (vals[0], vals[-1]):
            reqs.append(req("builtin", cls, "g3", base + [tmpl.format(c=cls, v=v)], probe))
    res = jmany(reqs)
    base_j, live, dead = res[0], [], []
    for i, (name, tmpl, shared, vals) in enumerate(kn):
        best = 0.0
        for r in (res[1 + 2 * i], res[2 + 2 * i]):
            d = [y[0] - x[0] for x, y in zip(base_j, r)]
            if any(abs(x) > 1e-9 for x in d):
                best = max(best, abs(sum(d) / len(d)), 1e-6)
        (live if best > CFG["min_probe"] else dead).append((best, name, tmpl, shared, vals))
    live.sort(key=lambda x: -x[0])
    log("classes %s: %d knobs, %d live, %d dead" % (cls, len(kn), len(live), len(dead)))
    # a value the rejudge stage accepted for this class is the starting table, and its knob is not searched again
    kept, rows, done = [r["spec"] for r in SUMMARY["stages"].get("rejudge", []) if r.get("accepted") and r["class"] == cls], [], set()
    for ps in range(CFG["passes"]):
        accepted_this_pass = False
        for sens, name, tmpl, shared, vals in live[:CFG["max_knobs"]]:
            if name in done or any(k.split("=")[0] == tmpl.format(c=cls, v=0).split("=")[0] for k in kept):
                continue   # a knob that took a value is not searched twice
            res = jmany([req("builtin", cls, "g3", base + kept, train)] + [req("builtin", cls, "g3", base + kept + [tmpl.format(c=cls, v=v)], train) for v in vals])
            cands = sorted(((paired(res[0], r) + (v,)) for r, v in zip(res[1:], vals)), key=lambda x: -x[0])
            d, se, v = cands[0]
            spec = tmpl.format(c=cls, v=v)
            row = {"class": cls, "knob": name, "shared": shared, "best": v, "probe": round(sens, 3), "pass": ps + 1, "train": [round(d, 3), round(se, 3)],
                   "sweep": [[c[2], round(c[0], 3), round(c[1], 3)] for c in sorted(cands, key=lambda x: x[2])], "accepted": False, "spec": spec}
            passed = d > 2.5 * se and d >= 0.15
            if passed or (CFG["force"] and not rows):   # a smoke run pushes its first sweep through every later step
                j = judge(cls, base + kept, spec, forced=not passed)
                row["steps"], row["why"] = j["steps"], j["why"]
                row["accepted"] = passed and j["accepted"]
                if not passed:
                    row["why"] = "train: does not beat noise (later steps were run for the smoke test only); " + j["why"]
            else:
                row["why"] = "train: does not beat noise" if not d > 2.5 * se else "train: beats noise but under 0.15 J"
            if row["accepted"]:
                kept.append(spec)
                done.add(name)
                accepted_this_pass = True
            rows.append(row)
            emit_run(cls, "tune %s %s %s -> %s (%s; train %+.2f J, se %.2f)" % (cls, name, shared, v, "ACCEPTED" if row["accepted"] else "rejected", d, se), base + kept)
            log("classes %s pass %d  %-34s %-8g -> %-8g train %+.2f (se %.2f)  %s" % (cls, ps + 1, name, shared, v, d, se, row["why"][:140]))
            SUMMARY["stages"].setdefault("classes", {})[cls] = {"kept": kept, "base": base, "rows": rows, "live": [[n, round(x, 3)] for x, n, _, _, _ in live], "dead": [n for _, n, _, _, _ in dead], "complete": False}
            save_summary()
        if not accepted_this_pass:
            break   # a second pass only looks again after something changed
    out = {"kept": kept, "base": base, "rows": rows, "live": [[n, round(x, 3)] for x, n, _, _, _ in live], "dead": [n for _, n, _, _, _ in dead], "complete": True,
           "not_searched": [n for _, n, _, _, _ in live[CFG["max_knobs"]:]]}
    if kept:   # what the accepted values change in his play, for the class-card read (the runner reports it; a card deviation is an open decision)
        cmd = [A.bin, "--no-json", "--threads", "2", "--classes", cls, "--seeds", "1,2,3,4,5,6", "--rating", "0", "--baseline", "/dev/stdout"] + world_args("builtin")
        for c in base:
            cmd += ["--curve", c]
        b = text_run("metrics:" + cls + ":" + " ".join(base), list(cmd), "builtin")
        for c in kept:
            cmd += ["--curve", c]
        n = text_run("metrics:" + cls + ":" + " ".join(base + kept), cmd, "builtin")
        pick = lambda t: {m.group(1): float(m.group(2)) for m in re.finditer(r"^%s\|natural\|([^=]+)=([-0-9.e]+)" % re.escape(cls), t, re.M)}
        ob, nb = pick(b), pick(n)
        ch = sorted(((abs(nb[k] - ob[k]) / (abs(ob[k]) + 1.0), k, ob[k], nb[k]) for k in ob if k in nb and abs(nb[k] - ob[k]) > 1e-9), reverse=True)[:12]
        out["play_changes_natural"] = [[k, o, w] for _, k, o, w in ch]
        open_decision("class card read for %s" % cls, "accepted %s; the largest changes in his play are in summary.json stages.classes.%s.play_changes_natural: confirm none leaves his class card before writing" % (" ".join(kept), cls))
    # runbook open decision 7: many accepted values for one class, or a consideration weight at the edge of what was tried, say the shared
    # catalogue (not the class) is off: reported before anything is written
    edge = [r["spec"] for r in rows if r["accepted"] and r["knob"].endswith(".w") and (r["best"] >= 2.0 * r["shared"] - 1e-6 or r["best"] <= 0.5 * r["shared"] + 1e-6)]
    if len(kept) > 10:
        open_decision("%s took %d accepted values (more than 10)" % (cls, len(kept)), "a sign the shared catalogue, not the class, is off: report before writing any of %s" % " ".join(kept))
    if edge:
        open_decision("%s: a consideration weight accepted at the edge of the range tried (x2 or x0.5)" % cls, "%s: the best weight may lie outside what was searched, and so far from the catalogue's that the shared curve is in question" % " ".join(edge))
    SUMMARY["stages"].setdefault("classes", {})[cls] = out
    save_summary()
    return out


def stage_classes():
    knobs, gates = all_knobs()
    SUMMARY["stages"]["knobs"] = {"count": len(knobs), "pure_gates_never_touched": gates, "unrateable": ["scalar.touchSpentSec (the lab opens no touch window)"],
                                  "fixed": ["School Circle: schoolPeak / schoolFloor / schoolShape / schoolMasteredBump"]}
    order = A.classes.split(",") if A.classes else gap_order()
    order = [c for c in sorted(T1) if c in order] + [c for c in order if c not in T1]   # the T1 rows first: their values are every later class's baseline
    for cls in order:   # what this stage is about to decide again is forgotten first, so a resumed run meets the same baselines as the first one
        SUMMARY["stages"].get("classes", {}).pop(cls, None)
    missing = [t for t in sorted(T1) if t not in order and not SUMMARY["stages"].get("classes", {}).get(t, {}).get("complete")]
    if missing and not A.smoke:
        raise SystemExit("--classes without %s: the T1 rows are searched first (their accepted values are every other class's baseline). Run them in this --out first, or include them." % ", ".join(missing))
    for cls in order:
        search_class(cls, knobs)


def gap_order():
    """The classes furthest from the class oracle first (the per-class gap of a small report run), else the fixed order."""
    try:
        path = os.path.join(A.out, "gap-run.json")
        if not os.path.exists(path):
            subprocess.run([A.bin, "--threads", str(A.jobs), "--classes", "beta1", "--fights", "4", "--class-oracle", "4", "--gap-scenarios", "2", "--label", "tune_driver gap order", "--json", path], capture_output=True, text=True)
        gaps = {g["class"]: g["gap"] for g in json.load(open(path)).get("class_gaps", []) if g.get("class") in CELLS}
        order = sorted(CELLS, key=lambda c: -gaps.get(c, 0.0))
        SUMMARY["stages"]["gap_order"] = [[c, gaps.get(c)] for c in order]
        return order
    except Exception as e:   # the order is a priority, never a result: fall back, and say so
        log("gap order unavailable (%s): the fixed order is used" % e)
        return CELLS


def cpp_line(spec):
    """The ClassCurves.cpp statement of an accepted --curve spec (pasted into SeedSearchedValues, with its evidence as a comment)."""
    txt = text_run("print-classes", [A.bin, "--print-classes"])
    slugs = [m.group(1) for m in re.finditer(r"^\s+T\d (\S+)", txt, re.M)]   # table order: rows 0..34, then the base cells 35..37
    cls, rest = spec.split(":", 1)
    path, val = rest.split("=")
    row = "t[%d /* %s */]" % (slugs.index(cls), cls)
    v = ("%s" % float(val)) + "f"
    if path.startswith("scalar."):
        return 'SetScalar(%s, "%s", %s);' % (row, path[7:], v)
    if path.startswith("family."):
        return "SetFamilyWeight(%s, static_cast<Family>(%s), %s);" % (row, path[7:], v)
    f, c, param = path.split(".")
    if param == "w":
        return "SetWeight(%s, static_cast<Family>(%s), static_cast<Cons>(%s), %s);" % (row, f, c, v)
    return 'SetCurveParam(%s, static_cast<Family>(%s), static_cast<Cons>(%s), sh.curve[%s], "%s", %s);' % (row, f, c, c, param, v)


def final_curves():
    out = []
    for cls, r in SUMMARY["stages"].get("classes", {}).items():
        out += r.get("kept", [])
    for r in SUMMARY["stages"].get("rejudge", []):
        if r.get("accepted") and r["spec"] not in out:
            out.append(r["spec"])
    return out


def stage_joint():
    """THE WHOLE FINAL TABLE against the shipped one, once more, on all 15 cells: the values were accepted one at a time, each in the
    table that existed when it was searched, and the companions' rows (T1) move everyone's fights. On the 128 train + holdout seeds, at
    the default roads, with stealth off and as shipped: no cell may be worse (the 15-at-once bar), no pooled mean may be worse. And
    each T1 value is left out in turn: the table without it must not be better than the table with it."""
    curves = final_curves()
    st = {"table": curves, "cells": {}, "pooled": {}, "leave_one_out": [], "verdict": "no accepted value: nothing to gate"}
    if curves:
        seeds = list(range(1, CFG["train"] + 1)) + list(range(1001, 1001 + CFG["hold"]))
        fails = []

        def table_vs(a, b):   # per cell and config, and pooled over the 15 cells: b - a
            res = jmany([req("builtin", c, g, x, seeds) for c in CELLS for g in CONFIGS for x in (a, b)])
            cells, pooled, i = {}, {}, 0
            for c in CELLS:
                for g in CONFIGS:
                    cells["%s/%s" % (c, g)] = paired(res[i], res[i + 1])
                    pooled.setdefault(g, []).append([y[0] - x[0] for x, y in zip(res[i], res[i + 1])])
                    i += 2
            out = {}
            for g in CONFIGS:
                dd = [sum(col) / len(col) for col in zip(*pooled[g])]
                d = sum(dd) / len(dd)
                out[g] = (d, math.sqrt(sum((x - d) ** 2 for x in dd) / (len(dd) - 1) / len(dd)) if len(dd) > 1 else 0.0)
            return cells, out
        cells, pooled = table_vs([], curves)
        for k, (d, se) in cells.items():
            st["cells"][k] = [round(d, 3), round(se, 3)]
            if worse(d, se, True) or out_of_band(k.split("/")[0], d, se):
                fails.append("%s worse (%+.2f, se %.2f)" % (k, d, se))
        for g, (d, se) in pooled.items():
            st["pooled"][g] = [round(d, 3), round(se, 3)]
            if worse(d, se):
                fails.append("the 15-cell mean is worse at %s (%+.2f, se %.2f)" % (g, d, se))
        for spec in [c for c in curves if c.split(":")[0] in T1]:
            _, lo = table_vs([c for c in curves if c != spec], curves)
            st["leave_one_out"].append({"spec": spec, "contribution": {g: [round(d, 3), round(se, 3)] for g, (d, se) in lo.items()}})
            for g, (d, se) in lo.items():
                if worse(d, se):
                    fails.append("T1 value %s makes the final table worse at %s (%+.2f, se %.2f over the 15 cells)" % (spec, g, d, se))
        st["verdict"] = "PASS" if not fails else "FAIL: " + "; ".join(fails)
        log("joint: %d values, pooled g3 %+.2f (se %.2f) off %+.2f shipped %+.2f  %s" % (len(curves), pooled["g3"][0], pooled["g3"][1], pooled["off"][0], pooled["shipped"][0], st["verdict"]))
        if fails:
            open_decision("JOINT GATE FAILED: the final table as a whole", "write NO value from this run until this is decided: %s" % "; ".join(fails))
    SUMMARY["stages"]["joint"] = st


EDGE_MUL = {"up": (3.0, 4.0), "down": (0.35, 0.25)}   # marth 11-12 "Widen to x4 / x0.25" (the classes stage tried x0.5 .. x2)
EDGE_ADD = (2.0, 4.0)                                 # an additive knob: 2 and 4 times its largest step in the edge's direction


def edge_of(name, shared, tried, best):
    """'up' / 'down' when the accepted value is the outermost value tried on that side of the shared one, else None."""
    mode = KNOB_META.get(name, ("add",))[0]
    pos = (lambda v: v / shared) if mode == "mul" and shared != 0.0 else (lambda v: v)
    xs = [pos(v) for v in tried] + [pos(shared)]
    if pos(best) >= max(xs) - 1e-9 and pos(best) > pos(shared):
        return "up"
    if pos(best) <= min(xs) + 1e-9 and pos(best) < pos(shared):
        return "down"
    return None


def widen(name, shared, tried, side):
    """The values past the edge, INSIDE the knob's valid range (KNOB_META). [] = the edge is the valid limit: nothing to widen."""
    mode, lo, hi, strict = KNOB_META.get(name, ("add", -INF, INF, False))
    if mode == "mul" and shared != 0.0:
        vals = [shared * k for k in EDGE_MUL[side]]
    else:
        step = max([abs(v - shared) for v in tried if (v > shared) == (side == "up")] or [0.0])
        vals = [shared + (k * step if side == "up" else -k * step) for k in EDGE_ADD]
    out = []
    for v in vals:
        v = round(min(hi, max(lo, v)), 5)
        if strict and not lo < v < hi:
            continue
        if strict and v in (lo, hi):
            continue
        if all(abs(v - t) > 1e-6 for t in list(tried) + [shared] + out):
            out.append(v)
    return out


def stage_edge():
    """marth 11-12 ("Widen to x4 / x0.25"): ONLY the knobs whose ACCEPTED value sits at the edge of the range the classes stage
    tried are searched again, past that edge (multiplicative knobs to x4 / x0.25 of the shared value, additive ones to 2 and 4
    times the largest step tried), inside each knob's valid range, under the same RULE, with the accepted value as the baseline
    (the new value REPLACES it). The table is never changed here: the edge value stays written; a widened value that passes every
    line of the rule is far from the shared curve by construction and becomes an OPEN DECISION with its evidence (never shipped)."""
    knobs, _ = all_knobs()
    train = range(1, CFG["train"] + 1)
    rows = []
    for cls, r in SUMMARY["stages"].get("classes", {}).items():
        if not r.get("complete"):
            continue
        meta = {n: (t, sh) for n, t, sh, _ in class_knobs(cls, knobs)}
        for row in [x for x in r["rows"] if x.get("accepted")]:
            tried = [c[0] for c in row["sweep"]]
            side = edge_of(row["knob"], row["shared"], tried, row["best"])
            if not side:
                continue
            tmpl, shared = meta[row["knob"]]
            vals = widen(row["knob"], shared, tried, side)
            e = {"class": cls, "knob": row["knob"], "shared": shared, "accepted": row["best"], "side": side, "widened": vals, "sweep": [], "verdict": ""}
            if not vals:
                e["verdict"] = "EDGE IS THE VALID LIMIT: nothing to widen; the accepted value stands"
            else:
                base = r["base"] + r["kept"]
                res = jmany([req("builtin", cls, "g3", base, train)] + [req("builtin", cls, "g3", [k for k in base if k != row["spec"]] + [tmpl.format(c=cls, v=v)], train) for v in vals])
                cands = sorted(((paired(res[0], x) + (v,)) for x, v in zip(res[1:], vals)), key=lambda c: -c[0])
                e["sweep"] = [[c[2], round(c[0], 3), round(c[1], 3)] for c in sorted(cands, key=lambda c: c[2])]
                d, se, v = cands[0]
                e["train"] = [round(d, 3), round(se, 3)]
                passed = d > 2.5 * se and d >= 0.15
                if passed or (CFG["force"] and not any("steps" in x for x in rows)):   # a smoke run pushes its first widened sweep through the judge
                    spec = tmpl.format(c=cls, v=v)
                    j = judge(cls, base, spec, forced=not passed, replace=row["spec"])
                    j["accepted"] = passed and j["accepted"]
                    e["steps"], e["best"] = j["steps"], v
                    if j["accepted"]:
                        e["verdict"] = "WIDER VALUE PASSES THE RULE: reported, never shipped (far from the shared curve)"
                        open_decision("%s %s: a value past the edge passes the rule" % (cls, row["knob"]), "shared %g, accepted (edge, %s) %g, widened %g: train %+.2f (se %.2f) over the accepted value, holdout %+.2f, cross-world mean %+.2f (se %.2f). Far from the shared curve: marth or the coordinator decides; the accepted %g stays written until then (summary.json stages.edge)"
                                      % (cls, row["knob"], shared, side, row["best"], v, d, se, j["steps"]["holdout"][0], j["steps"]["cross_world"]["mean/g3"][0], j["steps"]["cross_world"]["mean/g3"][1], row["best"]))
                    else:
                        e["verdict"] = "the accepted edge value holds (%s: %s)" % ("a wider value beat it on train only" if passed else "SMOKE: judged without a train pass", j["why"])
                else:
                    e["verdict"] = "the accepted edge value holds (no wider value beats it: train %+.2f, se %.2f)" % (d, se)
            rows.append(e)
            log("edge %s %s %g (accepted %g, %s) widened %s: %s" % (cls, row["knob"], shared, row["best"], side, vals, e["verdict"]))
            SUMMARY["stages"]["edge"] = rows
            save_summary()
    SUMMARY["stages"]["edge"] = rows
    if not rows:
        log("edge: no accepted value sits at the edge of its range")


GLOBALS = [("t.momentum", 1.25, [1.0, 1.05, 1.1, 1.15, 1.2]), ("t.powerCd", 10.0, [5.0, 7.0]), ("t.postureMomentum", 1.5, [1.75, 2.0])]


def stage_globals():
    curves = final_curves()
    seeds = range(5001, 5001 + CFG["global_seeds"])
    rows = []
    for name, shipped, vals in (GLOBALS[:1] if CFG["force"] else GLOBALS):
        for v in (vals[:1] if CFG["force"] else vals):
            st = "%s=%s" % (name, v)
            res = jmany([req("builtin", c, g, curves, seeds, s) for c in CELLS for g in CONFIGS for s in (None, st)])
            row = {"set": st, "shipped": shipped, "per_class": {}, "switch_per_min": {}, "verdict": ""}
            fails, i, pooled = [], 0, {}
            for c in CELLS:
                for g in CONFIGS:
                    b, n = res[i], res[i + 1]
                    i += 2
                    d, se = paired(b, n)
                    row["per_class"]["%s/%s" % (c, g)] = [round(d, 3), round(se, 3)]
                    pooled.setdefault(g, []).append([y[0] - x[0] for x, y in zip(b, n)])
                    if worse(d, se, True) or out_of_band(c, d, se):
                        fails.append("%s worse at %s (%+.2f, se %.2f)" % (c, g, d, se))
                    if g == "g3":
                        row["switch_per_min"][c] = [round(sw_rate(b), 1), round(sw_rate(n), 1)]
            for g in CONFIGS:
                dd = [sum(col) / len(col) for col in zip(*pooled[g])]
                d = sum(dd) / len(dd)
                se = math.sqrt(sum((x - d) ** 2 for x in dd) / (len(dd) - 1) / len(dd)) if len(dd) > 1 else 0.0
                row["pooled_" + g] = [round(d, 3), round(se, 3)]
            d, se = row["pooled_g3"]
            rise = max((n / o - 1.0) if o > 0 else 0.0 for o, n in row["switch_per_min"].values())
            row["max_switch_rise_pct"] = round(100.0 * rise, 1)
            if not (d > 2.5 * se and d >= 0.15):
                row["verdict"] = "KEPT: does not beat noise on fresh seeds"
            elif fails:
                row["verdict"] = "REJECTED: " + "; ".join(fails[:4])
            else:
                lists = {}
                for w in [w for w in A.worlds if w != "builtin"]:
                    ls = range(5001, 5001 + CFG["list_global_seeds"])
                    r2 = jmany([req(w, c, "g3", curves, ls, s) for c in CELLS for s in (None, st)])
                    per = [[y[0] - x[0] for x, y in zip(r2[2 * k], r2[2 * k + 1])] for k in range(len(CELLS))]
                    dd = [sum(col) / len(col) for col in zip(*per)]
                    ld = sum(dd) / len(dd)
                    lse = math.sqrt(sum((x - ld) ** 2 for x in dd) / (len(dd) - 1) / len(dd)) if len(dd) > 1 else 0.0
                    lists[w] = [round(ld, 3), round(lse, 3)]
                    if worse(ld, lse):
                        fails.append("worse on %s (%+.2f, se %.2f)" % (w, ld, lse))
                row["lists_g3"] = lists
                kf = {}
                for w in A.worlds:   # a shared value moves the Knight's fights too: his guard floor must hold with it
                    o, n = knight_floor(w, "knight", curves), knight_floor(w, "knight", curves, st)
                    for g in ("g3", "shipped"):
                        kf["%s/%s" % (w, g)] = [o[g], n[g]]
                        if n[g] < 60.0 and (o[g] >= 60.0 or n[g] < o[g] - 0.5):
                            fails.append("Knight guard floor on %s %s %.1f -> %.1f" % (w, g, o[g], n[g]))
                row["knight_floor"] = kf
                if fails:
                    row["verdict"] = "REJECTED: " + "; ".join(fails)
                elif rise > 0.10:
                    row["verdict"] = "BEATS NOISE, NOT APPLIED: decision switching rises by %.0f%% for some class and J does not price switching" % (100.0 * rise)
                else:
                    row["verdict"] = "PASSES RULE"
            rows.append(row)
            emit_run("beta1", "tune global %s (%s; pooled %+.2f J, se %.2f)" % (st, row["verdict"][:40], d, se), curves, st)
            log("globals %s: pooled g3 %+.2f (se %.2f) off %+.2f shipped %+.2f  max switching rise %.0f%%  %s" % (st, d, se, row["pooled_off"][0], row["pooled_shipped"][0], 100.0 * rise, row["verdict"]))
            SUMMARY["stages"]["globals"] = rows
            save_summary()
    for r in rows:
        if r["verdict"].startswith("PASSES") or r["verdict"].startswith("BEATS"):
            open_decision("global %s" % r["set"], "%s; pooled g3 %+.2f (se %.2f), switching rise %.0f%%. A shared default is a design value: marth or the coordinator decides (t.powerCd is also a ruling: 10 s rows; t.momentum is the anti-thrash term)" % (r["verdict"], r["pooled_g3"][0], r["pooled_g3"][1], r["max_switch_rise_pct"]))
    SUMMARY["stages"]["globals"] = rows


CONSTS = [("kCloseSlack", 1.15, [1.0, 1.3]), ("kMinimumFrac", 0.4, [0.3, 0.5]), ("kRangeVeto", 1.15, [1.0, 1.3]), ("kPostureDist", 1200.0, [900.0, 1500.0])]


def stage_consts():
    curves = final_curves()
    seeds = range(5001, 5001 + CFG["const_seeds"])
    rows = []
    for name, shipped, vals in (CONSTS[:1] if CFG["force"] else CONSTS):
        for v in (vals[:1] if CFG["force"] else vals):
            vb = os.path.join(A.out, "bin-%s-%s" % (name, v))
            if not os.path.exists(vb):
                r = subprocess.run([os.path.join(HERE, "const_probe.sh"), name, str(v), vb], capture_output=True, text=True)
                if r.returncode != 0 or not os.path.exists(vb):
                    rows.append({"const": name, "value": v, "verdict": "PROBE FAILED TO BUILD: " + r.stderr[-200:]})
                    continue
            res = jmany([req("builtin", c, g, curves, seeds, None, b) for c in CELLS for g in CONFIGS for b in (None, vb)])
            row, i, pooled, same = {"const": name, "was": shipped, "value": v}, 0, {}, True
            for c in CELLS:
                for g in CONFIGS:
                    pooled.setdefault(g, []).append([y[0] - x[0] for x, y in zip(res[i], res[i + 1])])
                    same = same and all(abs(x) < 1e-9 for x in pooled[g][-1])
                    i += 2
            for g in CONFIGS:
                dd = [sum(col) / len(col) for col in zip(*pooled[g])]
                d = sum(dd) / len(dd)
                se = math.sqrt(sum((x - d) ** 2 for x in dd) / (len(dd) - 1) / len(dd)) if len(dd) > 1 else 0.0
                row[g] = [round(d, 3), round(se, 3)]
            d, se = row["g3"]
            row["verdict"] = "NO EFFECT IN THE LAB: every fight is identical (nothing reads it here, or the probe did not reach it)" if same else "BEATS NOISE" if d > 2.5 * se and d >= 0.15 and row["off"][0] > -2 * row["off"][1] and row["shipped"][0] > -2 * row["shipped"][1] else "the shipped value holds"
            if row["verdict"] == "BEATS NOISE":
                open_decision("constant %s %s -> %s" % (name, shipped, v), "pooled g3 %+.2f (se %.2f): a constexpr in native/iaus, changed only by its own reviewed edit (kRangeVeto also exists as literals in three Catalogue.cpp curves)" % (d, se))
            rows.append(row)
            log("consts %s=%s: g3 %+.2f (se %.2f) off %+.2f shipped %+.2f  %s" % (name, v, d, se, row["off"][0], row["shipped"][0], row["verdict"]))
            SUMMARY["stages"]["consts"] = rows
            save_summary()
    SUMMARY["stages"]["consts"] = rows


def stage_floor():
    curves = final_curves()
    out = {}
    for w in A.worlds:
        for cls in (KNIGHTS[:1] if CFG["force"] else KNIGHTS):
            out["%s/%s" % (cls, w)] = fl = knight_floor(w, cls, curves)
            log("floor %s on %s: default %.1f, as shipped %.1f  (why, as shipped: %s)" % (cls, w, fl["g3"], fl["shipped"], fl["shipped_why"]))
            if min(fl["g3"], fl["shipped"]) < 60.0:
                open_decision("Knight guard floor under 60%%: %s on %s" % (cls, w), "default %.1f, as shipped %.1f; unguarded blows by cause (%% of all blows under 2+ own-taunted foes), as shipped: %s. marth's floor is a hard requirement (Q-S-5): the fix is a mechanism change (the hold / renewal / rest of the guard, or a taunt-count term), not a table value" % (fl["g3"], fl["shipped"], fl["shipped_why"]))
    SUMMARY["stages"]["floor"] = out


def stage_rating():
    curves = final_curves()
    out = {}
    for w in A.worlds:
        cmd = [A.bin, "--no-json", "--threads", str(A.jobs), "--rating-only", "--rating", "3" if not CFG["force"] else "1", "--classes", "beta1", "--seeds", ",".join(str(s) for s in range(1, CFG["rating_seeds"] + 1))] + world_args(w)
        for c in curves:
            cmd += ["--curve", c]
        txt = text_run("rating:" + json.dumps(cmd[1:]), cmd, w)
        rows = []
        for m in re.finditer(r"^(\S+)\s+T(\d)\s+([-0-9.]+)\s+([-0-9.]+)\s+(\d+)/(\d+)\s+([-+0-9.]+)\s*(BELOW)?", txt, re.M):
            rows.append({"class": m.group(1), "tier": int(m.group(2)), "rating": float(m.group(3)), "se": float(m.group(4)), "rank": "%s/%s" % (m.group(5), m.group(6)), "vs_tier": float(m.group(7)), "below": bool(m.group(8))})
        if len(rows) != len(CELLS):
            raise RuntimeError("rating on %s: %d class rows parsed, %d expected (a failed run or a changed table format):\n%s" % (w, len(rows), len(CELLS), txt[-600:]))
        out[w] = rows
        below = [r["class"] for r in rows if r["below"]]
        log("rating %s: %d classes, below their tier: %s" % (w, len(rows), ", ".join(below) or "none"))
        if below:
            open_decision("rating: classes under their tier on %s" % w, "%s (summary.json stages.rating.%s; the ranged line was already under in 32.5, a possible lab artefact of the bow model)" % (", ".join(below), w))
    SUMMARY["stages"]["rating"] = out


def stage_recalib():
    """The potion-worth table was measured under the shipped drink curves. Measure it again under the accepted values and confirm every
    accepted value once more with the new table (fresh seeds 9001..). A value that fails is an open decision, never silently kept."""
    curves = final_curves()
    st = {"accepted": curves, "rows": []}
    if curves:
        tab = os.path.join(A.out, "potworth-builtin-final.txt")
        measure_worth(tab, "builtin", CFG["calib_seeds"], curves)
        old = A.builtin_worth
        A.builtin_worth = tab
        seeds = range(9001, 9001 + CFG["confirm"])
        for spec in curves:
            cls = spec.split(":")[0]
            rest = [c for c in curves if c != spec]
            b, v = jmany([req("builtin", cls, "g3", rest, seeds), req("builtin", cls, "g3", curves, seeds)])
            d, se = paired(b, v)
            ok = not worse(d, se)
            st["rows"].append({"spec": spec, "d": round(d, 3), "se": round(se, 3), "holds": ok})
            log("recalib %s: %+.2f (se %.2f) with the re-measured potion worth: %s" % (spec, d, se, "holds" if ok else "DOES NOT HOLD"))
            if not ok:
                open_decision("accepted value %s loses with the re-measured potion worth" % spec, "%+.2f (se %.2f) on fresh seeds: do not write it; report" % (d, se))
        A.builtin_worth = old
        st["table"] = tab
    SUMMARY["stages"]["recalib"] = st


def render_md():
    s = SUMMARY["stages"]
    L = ["# IAUS per-class tuning: summary (%s%s)" % (time.strftime("%Y-%m-%d %H:%M"), ", SMOKE RUN: no number here is a result" if CFG["force"] else ""), "",
         "Worlds: %s. Rule: tune_driver.py docstring (RULE) and iaus-sim-v2.md section 33." % ", ".join(A.worlds), ""]
    if "rejudge" in s:
        L += ["## Re-judged values", "", "| Spec | Train | Verdict |", "|---|---|---|"]
        L += ["| `%s` | %+.2f (se %.2f) | %s |" % (r["spec"], r.get("train", [0, 0])[0], r.get("train", [0, 0])[1], r["why"]) for r in s["rejudge"]] + [""]
        L += ["Accepted re-judged value, line for ClassCurves.cpp: `%s`" % cpp_line(r["spec"]) for r in s["rejudge"] if r["accepted"]] + [""]
    if "classes" in s:
        L += ["## Per class", "", "| Class | Knobs live / dead | Sweeps | Accepted (write these) |", "|---|---|---|---|"]
        for c, r in s["classes"].items():
            L.append("| %s%s | %d / %d | %d | %s |" % (c, "" if r.get("complete") else " (running)", len(r["live"]), len(r["dead"]), len(r["rows"]), " ".join("`%s`" % k for k in r["kept"]) or "none"))
        L += ["", "### Accepted values, with their evidence", "", "| Class | Knob | Shared -> new | Train | Holdout | Cross-world mean (g3) | As shipped |", "|---|---|---|---|---|---|---|"]
        for c, r in s["classes"].items():
            for row in r["rows"]:
                if row["accepted"]:
                    st = row["steps"]
                    L.append("| %s | %s | %g -> %g | %+.2f (se %.2f) | %+.2f (se %.2f) | %+.2f (se %.2f) | %+.2f (se %.2f) |" % (c, row["knob"], row["shared"], row["best"], row["train"][0], row["train"][1],
                             st["holdout"][0], st["holdout"][1], st["cross_world"]["mean/g3"][0], st["cross_world"]["mean/g3"][1], st["cross_world"]["mean/shipped"][0], st["cross_world"]["mean/shipped"][1]))
        L += ["", "### Lines for ClassCurves.cpp (SeedSearchedValues), one per accepted value", "", "```cpp"]
        for c, r in s["classes"].items():
            for row in r["rows"]:
                if row["accepted"]:
                    st = row["steps"]
                    L.append("// %s %s %g -> %g: J %+.2f (se %.2f) on %d train seeds, holdout %+.2f (se %.2f), fresh-seed mean over %s %+.2f (se %.2f), as shipped %+.2f" % (c, row["knob"], row["shared"], row["best"],
                             row["train"][0], row["train"][1], CFG["train"], st["holdout"][0], st["holdout"][1], "/".join(A.worlds), st["cross_world"]["mean/g3"][0], st["cross_world"]["mean/g3"][1], st["cross_world"]["mean/shipped"][0]))
                    L.append(cpp_line(row["spec"]))
        L += ["```"]
        L += ["", "### Closest rejected (train passed, a later step failed)", ""]
        for c, r in s["classes"].items():
            for row in r["rows"]:
                if not row["accepted"] and "steps" in row and row["train"][0] > 2.5 * row["train"][1]:
                    L.append("- %s `%s`: train %+.2f (se %.2f); %s" % (c, row["spec"], row["train"][0], row["train"][1], row["why"]))
        L += ["", "### Dead knobs per class (changed no seed's J in the liveness screen)", ""]
        for c, r in s["classes"].items():
            L.append("- %s: %d dead (full list in summary.json); live and not searched (over the cap): %s" % (c, len(r["dead"]), ", ".join(r.get("not_searched", [])) or "none"))
        L.append("")
    if "joint" in s:
        j = s["joint"]
        L += ["## The whole final table against the shipped one (joint gate)", "", "Verdict: **%s**" % j["verdict"], ""]
        if j["pooled"]:
            L += ["Mean over the 15 cells: " + ", ".join("%s %+.2f (se %.2f)" % (g, v[0], v[1]) for g, v in j["pooled"].items()), ""]
            L += ["- without `%s`: the table with it is %s" % (r["spec"], ", ".join("%s %+.2f (se %.2f)" % (g, v[0], v[1]) for g, v in r["contribution"].items())) for r in j["leave_one_out"]] + [""]
    if "edge" in s:
        L += ["## Accepted values at the edge of their range, searched past it (x4 / x0.25)", "", "| Class | Knob | Shared | Accepted (side) | Widened | Train (best widened vs accepted) | Verdict |", "|---|---|---|---|---|---|---|"]
        L += ["| %s | %s | %g | %g (%s) | %s | %s | %s |" % (e["class"], e["knob"], e["shared"], e["accepted"], e["side"], " ".join("%g" % v for v in e["widened"]) or "-",
                                                    "%+.2f (se %.2f)" % tuple(e["train"]) if "train" in e else "-", e["verdict"]) for e in s["edge"]] + ([] if s["edge"] else ["| none | | | | | | |"]) + [""]
    if "globals" in s:
        L += ["## Shared Tuning scalars (fresh seeds 5001..)", "", "| Set | Pooled g3 | off | shipped | Max switching rise | Verdict |", "|---|---|---|---|---|---|"]
        L += ["| `%s` | %+.2f (se %.2f) | %+.2f | %+.2f | %.0f%% | %s |" % (r["set"], r["pooled_g3"][0], r["pooled_g3"][1], r["pooled_off"][0], r["pooled_shipped"][0], r["max_switch_rise_pct"], r["verdict"]) for r in s["globals"]] + [""]
    if "consts" in s:
        L += ["## Compile-time constants", "", "| Constant | Shipped -> probed | g3 | off | shipped | Verdict |", "|---|---|---|---|---|---|"]
        L += ["| %s | %s -> %s | %+.2f (se %.2f) | %+.2f | %+.2f | %s |" % (r["const"], r.get("was"), r["value"], r["g3"][0], r["g3"][1], r["off"][0], r["shipped"][0], r["verdict"]) if "g3" in r else "| %s | %s | | | | %s |" % (r["const"], r["value"], r["verdict"]) for r in s["consts"]] + [""]
    if "floor" in s:
        L += ["## Knight line guard floor (>= 60%)", "", "| Class / world | Default | As shipped | Unguarded as shipped, by cause |", "|---|---|---|---|"]
        L += ["| %s | %.1f | %.1f | %s |" % (k, v["g3"], v["shipped"], ", ".join("%s %.1f" % kv for kv in v["shipped_why"].items())) for k, v in s["floor"].items()] + [""]
    if "rating" in s:
        L += ["## Rating (Tx) per world", ""]
        for w, rows in s["rating"].items():
            L += ["### %s" % w, "", "| Class | Tier | Rating | se | Rank | vs tier | |", "|---|---|---|---|---|---|---|"]
            L += ["| %s | T%d | %.2f | %.2f | %s | %+.2f | %s |" % (r["class"], r["tier"], r["rating"], r["se"], r["rank"], r["vs_tier"], "BELOW" if r["below"] else "") for r in rows] + [""]
    if "recalib" in s:
        L += ["## Accepted values under the re-measured potion worth", ""] + ["- `%s`: %+.2f (se %.2f) %s" % (r["spec"], r["d"], r["se"], "holds" if r["holds"] else "DOES NOT HOLD") for r in s["recalib"]["rows"]] + [""]
    L += ["## OPEN DECISIONS (the runner reports these and does not decide)", ""] + (["- **%s**: %s" % (d["what"], d["evidence"]) for d in SUMMARY["open_decisions"]] or ["- none"]) + [""]
    return "\n".join(L)


def main():
    global A, CFG, POOL
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True, help="the built simulator (SIM_OUT of tools/iaus_sim/run.sh)")
    ap.add_argument("--out", required=True, help="the run's directory: cache.jsonl, progress.log, summary.json, summary.md")
    ap.add_argument("--smoke", action="store_true", help="a few seeds and knobs, every stage exercised; its numbers are NOT results")
    ap.add_argument("--classes", help="comma list; default the 15 cells, the furthest from the oracle first")
    ap.add_argument("--worlds", default="builtin,LoreRim,Tuxborn,Vanilla")
    ap.add_argument("--stages", default="prep,rejudge,classes,joint,edge,globals,consts,floor,rating,recalib,summary")
    ap.add_argument("--rejudge", default="warrior:6.32.b=0.15", help="space-separated --curve specs to re-judge under RULE")
    ap.add_argument("--builtin-worth", help="a potion-worth table for the built-in world instead of PotionWorth.h")
    ap.add_argument("--jobs", type=int, default=12)
    ap.add_argument("--no-emit-runs", dest="emit_runs", action="store_false", help="do not write one dashboard results file per finished sweep")
    A = ap.parse_args()
    A.worlds = A.worlds.split(",")
    A.bin = os.path.abspath(A.bin)
    CFG = SMOKE if A.smoke else FULL
    os.makedirs(A.out, exist_ok=True)
    POOL = cf.ThreadPoolExecutor(max_workers=A.jobs)
    load_cache()
    prev = os.path.join(A.out, "summary.json")
    if os.path.exists(prev):   # a resumed or one-stage run sees what the earlier stages decided (the accepted values feed the later stages)
        try:
            old = json.load(open(prev))
            if old.get("config", {}).get("smoke") == A.smoke:
                SUMMARY["stages"], SUMMARY["open_decisions"] = old.get("stages", {}), [d for d in old.get("open_decisions", []) if d.get("stage")]
        except ValueError:
            pass   # cut off by a kill: every stage it held replays from the cache
    SUMMARY["config"] = dict(CFG, smoke=A.smoke, worlds=A.worlds, bin=A.bin)
    log("START%s stages=%s worlds=%s cache=%d results" % (" (SMOKE)" if A.smoke else "", A.stages, ",".join(A.worlds), len(CACHE)))
    stages = {"prep": stage_prep, "rejudge": stage_rejudge, "classes": stage_classes, "joint": stage_joint, "edge": stage_edge, "globals": stage_globals, "consts": stage_consts, "floor": stage_floor, "rating": stage_rating, "recalib": stage_recalib}
    for name in A.stages.split(","):
        if name == "summary":
            continue
        SUMMARY["open_decisions"] = [d for d in SUMMARY["open_decisions"] if d.get("stage") != name]
        n0 = len(SUMMARY["open_decisions"])
        stages[name]()
        for d in SUMMARY["open_decisions"][n0:]:
            d["stage"] = name
        save_summary()
        log("STAGE %s done" % name)
    save_summary()
    EMIT.shutdown(wait=True)
    log("DONE: %s/summary.md, %d open decision(s)" % (A.out, len(SUMMARY["open_decisions"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
