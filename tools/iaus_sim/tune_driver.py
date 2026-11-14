#!/usr/bin/env python3
"""The whole per-class IAUS curve tuning, unattended and resumable (marth 11-12: "The sim should be used to find all the ideal edge
case numbers"; "If fable thinks it can save tokens by building a script that can tune, it should"). HOST-ONLY, standard library only.

    tools/iaus_sim/tune_driver.py --bin <sim binary> --out <dir> [--smoke] [--classes a,b] [--worlds builtin,LoreRim,Tuxborn,Vanilla]
                                  [--stages prep,rejudge,classes,edge,joint,gap,globals,consts,floor,rating,recalib,logistics,summary] [--jobs 12]
                                  [--decisions auto|ask] [--replay <summary.json>]

Runbook, verdicts and the decisions this script may NOT take: Docs/research/2026-11/iaus-sim-v2.md section 33.
DECISIONS (marth 11-13, Docs/research/2026-11/tuner-auto-decisions.md): with `--decisions auto` (the default) a round needs no human decision.
          What used to be an open decision is decided by a rule (the two gates below) and written to summary.json `auto_decisions`
          with the rule and the numbers, or is a report-only note that never blocks. `--decisions ask` is the old behaviour (open
          decisions, no card / potion gate). `--replay summary.json` replays an old round's open decisions through the new rules offline.

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
          gap       the oracle gap per class and world on the shipped and the final table, the divergences classified (a) curve /
                    weight, (b) missing consideration or input, (c) missing behaviour, (d) modelling suspect; (a) drives a bounded
                    extra search; (b)-(d) are open decisions and <out>/gap-ledger.md rows. Its RULE is stage_gap's docstring.
          logistics the logistics world (Logistics.h): today's loot rules against the loot oracle per class (IAUS loot policy: post-T2).
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
  card         CLASS IDENTITY (marth 11-13: "a class must play like that class"): the class's bands (Playstyles.h, parsed at run time, never
               copied) measured on the natural bracket of the built-in world: no band may end further outside its bound than the class's
               start table had it, beyond max(CARD_SLACK of the band's credit scale, 2 se of the difference).
  potions      marth 11-13: "potion spam is not allowed. Potions other than hsm are emergency options". Health / stamina / magicka restores
               are normal tools; every other potion (a.drink_buff) is EMERGENCY ONLY. No potion may be drunk more than DRINK_FIGHT_CAP
               health drinks per fight or DRINK_SHARE_CAP of the action time over all potions unless the start table already did (then it
               may not rise); non-restore potions may not rise at all, and stay under BUFF_SHARE_CAP.
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
# G2 (review of 7e883a9a): gap runs before the stages that describe the final table, because its (a) search can move the table
DEFAULT_STAGES = "prep,rejudge,classes,edge,joint,gap,globals,consts,worth,floor,rating,recalib,logistics,loot,summary"
FOR_CLASS_STAGES = "prep,classes,edge,joint,gap,floor,summary"
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
            const_seeds=48, floor_seeds=36, rating_seeds=6, calib_seeds=24, calib_fights=400, list_calib_seeds=12, force=False,
            gap_seeds=12, gap_max_seeds=48, gap_nat=4, gap_scen=1, gap_families=3, gap_knobs=12, logi_seeds=12, logi_eps=8, card_seeds=3, card_fights=48)
SMOKE = dict(probe_seeds=2, train=4, hold=4, confirm=4, max_knobs=4, passes=1, min_probe=0.0, global_seeds=4, list_global_seeds=2,
             const_seeds=2, floor_seeds=2, rating_seeds=1, calib_seeds=2, calib_fights=40, list_calib_seeds=1, force=True,
             gap_seeds=1, gap_max_seeds=2, gap_nat=1, gap_scen=1, gap_families=1, gap_knobs=1, logi_seeds=1, logi_eps=1, card_seeds=1, card_fights=8)

A = None            # parsed arguments
CFG = None          # FULL or SMOKE
POOL = None
EMIT = cf.ThreadPoolExecutor(max_workers=1)   # the dashboard files
CACHE = {}          # (key, seed) -> (J, switches, minutes)
STAGE = ["-"]       # the stage running now (cache_stats)
TEXT = {}           # key -> stdout (rating / floor / list-uses runs)
LOCK = threading.Lock()
SUMMARY = {"rule": __doc__[__doc__.index("RULE ("):].strip(), "open_decisions": [], "auto_decisions": [], "stages": {}}


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
    extra = ["uses:" + sha(uses_path())] if classes.startswith("logi:") else []   # a loot run also depends on the combat world's use counts
    return json.dumps([world, classes, cfg, sorted(curves), sets or "", ident(world, binary)] + extra, separators=(",", ":"))


def uses_path():
    return os.path.join(A.out, "uses.txt")


def sim_cmd(world, classes, cfg, curves, sets, binary):
    if classes.startswith("logi:"):   # tune 2.11.1: the logistics world's IAUS arm, J_log per seed (Logistics.h Run, --logi-score), mean over the classes
        cmd = [binary or A.bin, "--no-json", "--logistics", str(CFG["logi_eps"]), "--logi-policy", "iaus", "--logi-score", "--logi-consumables",
               "--logi-uses", uses_path(), "--classes", classes[5:]]
        return cmd + (["--set", sets] if sets else [])
    cmd = [binary or A.bin, "--no-json", "--threads", "1", "--print-switch", "--classes", classes, "--fights", "48"] + world_args(world) + CONFIGS[cfg]
    for c in curves:
        cmd += ["--curve", c]
    if sets:
        cmd += ["--set", sets]
    return cmd


import queue

WORKERS = {}        # binary -> queue of idle `sim --serve` workers (tune 2.11.1)
WORKER_LOCK = threading.Lock()


def run_worker(cmd):
    """One job on a persistent `sim --serve` worker of cmd[0] (sim.cpp main: the lists are parsed once per worker, each job is a fork of
    it that runs exactly the normal command, stderr joined to stdout). Returns (exit code, output)."""
    with WORKER_LOCK:
        q = WORKERS.setdefault(cmd[0], queue.Queue())
    try:
        w = q.get_nowait()
    except queue.Empty:
        lists = [CACHE_DIR + x + ".sim.json" for x in A.worlds if x != "builtin"]
        w = subprocess.Popen([cmd[0], "--serve"] + lists, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    if any("\t" in a or "\n" in a for a in cmd[1:]):
        raise RuntimeError("a job argument holds a TAB or a newline: %s" % cmd)
    w.stdin.write("\t".join(cmd[1:]) + "\n")
    w.stdin.flush()
    out = []
    while True:
        line = w.stdout.readline()
        if not line:
            raise RuntimeError("the simulator worker died: %s\n%s" % (" ".join(cmd), "".join(out)[-600:]))
        if line.startswith("@@END "):
            rc = int(line.split()[1])
            break
        out.append(line)
    q.put(w)
    return rc, "".join(out)


def run_chunk(k, cmd, seeds):
    full = cmd + ["--seeds", ",".join(str(s) for s in seeds)]
    if A.no_worker:
        r = subprocess.run(full, capture_output=True, text=True)
        rc, out, err = r.returncode, r.stdout, r.stderr
    else:
        rc, out = run_worker(full)
        err = ""
    if rc != 0:
        raise RuntimeError("the simulator failed (exit %d): %s\n%s\n%s" % (rc, " ".join(cmd), out[-400:], err[-400:]))
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
        with LOCK:   # marth 11-13 "productive sims as fuel": how much of each stage was served from the cache (earlier runs of this --out)
            cs = SUMMARY.setdefault("cache_stats", {}).setdefault(STAGE[0], {"seed_runs": 0, "from_cache": 0})
            cs["seed_runs"] += len(seeds)
            cs["from_cache"] += len(seeds) - len(miss)
        cmd = sim_cmd(world, classes, cfg, curves, sets, binary)
        for i in range(0, len(miss), 8):
            futs.append(POOL.submit(run_chunk, k, cmd, miss[i:i + 8]))
    for f in futs:
        f.result()
    return [[CACHE[(key_of(w, c, g, cv, st, b), s)] for s in seeds] for w, c, g, cv, st, b, seeds in reqs]


def req(world, classes, cfg, curves, seeds, sets=None, binary=None):
    return (world, classes, cfg, tuple(curves), sets, binary, list(seeds))


def text_run(key, cmd, world=None, check=None):
    """A cached text run. The key carries the identity of the binary and the world (ident). A run that fails is NEVER cached and
    never read as an empty result: it stops the driver with the command and the simulator's own message."""
    key = key + "#" + ident(world, cmd[0])   # cmd[0] is always the simulator binary that runs
    if key not in TEXT:
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("the simulator failed (exit %d): %s\n%s\n%s" % (r.returncode, " ".join(cmd), r.stdout[-400:], r.stderr[-400:]))
        out = r.stdout
        if check:
            check(out)   # an output its reader rejects raises HERE, before it is cached: a bad output is never replayed
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


def scope():
    """The classes a run is scoped to (--classes), or None for all. classes, edge and gap act on these only and keep the rest."""
    return A.classes.split(",") if A.classes else None


def cells():
    """The 15 Beta 1 cells, plus every other class searched in this --out or named by --classes (a new class is gated with the rest)."""
    extra = [c for c in (scope() or []) + list(SUMMARY["stages"].get("classes", {})) if c not in CELLS]
    return CELLS + sorted(set(extra), key=extra.index)


# ── automatic decisions (tuner-auto-decisions.md) ────────────────────────────────────────────────────────────────────────────────
CARD_ALIAS = {"melee-off": "melee", "ranged-off": "ranged", "mage-off": "mage"}   # the tree-OFF base cells play their role's card (no row of their own in Playstyles.h)
SCHEMA_RAW = {"melee_share_pct": "b.melee", "taunt_per_min": "taunt/min", "power_per_min": "pow/min", "overheal_pct": "overheal%", "overkill_pct": "overkill%",
              "double_heal_pct": "dblheal%", "heal_latency_s": "heal.s"}   # Results.h SchemaMetrics: the schema name of a band -> the --baseline name
CARD_SLACK = 0.05        # PLACEHOLDER (no marth number): a band may end this much of the sim's credit scale (max(5, 0.5 bound), Playstyles.h BandCredit) further out
DRINK_FIGHT_CAP = 1.5    # PLACEHOLDER: health drinks per fight (drinkH/f); the shipped cells sit at 0.1 to 0.3
DRINK_SHARE_CAP = 8.0    # PLACEHOLDER: % of the follower's action time spent drinking, all potions (a.drink_*)
BUFF_SHARE_CAP = 1.0     # PLACEHOLDER: % of action time on a non-restore potion (a.drink_buff): emergency only, so rare by construction
EMERGENCY_HP_PCT = 50.0  # PLACEHOLDER: an emergency drink happens at or under this health %; read from buffDrinkAt% when the simulator reports it (it does not yet)
EDGE_ROUNDS = 3          # the edge stage: the first widening and at most two more past each newly accepted value
RULES = {   # kind -> the rule that replaces asking marth (the text is also in Docs/research/2026-11/tuner-auto-decisions.md)
    "class card": "an accepted value stays only if the class's bands (Playstyles.h) and the potion limits hold against the class's start table; else it is rejected",
    "edge": "a widened value is accepted only through the normal judge and the card + potion gates; it replaces the accepted value; up to EDGE_ROUNDS widenings",
    "gap far": "a value past the range tried is accepted only through the normal judge and the card + potion gates",
    "weight at edge": "report-only: the edge stage widens exactly these weights under the same rule",
    "many values": "report-only: every value passed every gate; the count never blocks",
    "oracle": "report-only: a ledger row (b-d divergences are not tunable); never a blocker",
    "joint": "the final table must pass the joint gate and the card + potion gates against the shipped table; a failing table is withheld (summary.json ship)",
    "withheld": "a value that loses on re-measure is withheld from the table (summary.json ship.withheld)",
    "note": "report-only: nothing is written from it; never a blocker",
}
CARDS = {}


def load_cards():
    """The class bands, parsed from Playstyles.h (the simulator's own table; never copied here). {class: {"line": n, "bands": [(metric, lo, hi, line)]}}."""
    if CARDS:
        return CARDS
    cur = None
    for n, line in enumerate(open(os.path.join(HERE, "Playstyles.h")), 1):
        m = re.match(r'\s*\{ "([a-z0-9-]+)", "', line)
        if m:
            cur = m.group(1)
            CARDS[cur] = {"line": n, "bands": []}
            continue
        if cur:
            for b in re.finditer(r'at(Least|Most)\("([^"]+)", ([-0-9.]+)', line):
                v = float(b.group(3))
                CARDS[cur]["bands"].append((b.group(2), v if b.group(1) == "Least" else -INF, INF if b.group(1) == "Least" else v, n))
    return CARDS


def band_viol(v, lo, hi):
    return lo - v if v < lo else v - hi if v > hi else 0.0


def band_scale(lo, hi):
    return max(5.0, 0.5 * abs(lo if lo > -INF else hi))   # BandCredit: the credit falls to 0 at half the bound (or 5)


def play_metrics(cls, curves):
    """{metric: (mean, se)} of the class's natural bracket in the built-in world over CFG card_seeds single-seed runs (-1 = no sample, skipped)."""
    def one(sd):
        cmd = [A.bin, "--no-json", "--threads", "2", "--classes", cls, "--seed", str(sd), "--fights", str(CFG["card_fights"]), "--rating", "0", "--baseline", "/dev/stdout"] + world_args("builtin")
        for c in curves:
            cmd += ["--curve", c]
        t = text_run("card:%s:%d:%d:%s" % (cls, sd, CFG["card_fights"], " ".join(curves)), cmd, "builtin")
        return {m.group(1): float(m.group(2)) for m in re.finditer(r"^%s\|natural\|([^=]+)=([-0-9.e+]+)" % re.escape(cls), t, re.M)}
    runs = list(POOL.map(one, range(1, CFG["card_seeds"] + 1)))
    out = {}
    for k in set().union(*runs):
        xs = [r[k] for r in runs if k in r and r[k] >= 0]
        if xs:
            mu = sum(xs) / len(xs)
            out[k] = (mu, math.sqrt(sum((x - mu) ** 2 for x in xs) / (len(xs) - 1) / len(xs)) if len(xs) > 1 else 0.0)
    return out


def pm_get(m, raw):
    """(value, se) of a metric; an a.<family> share that never fired is 0 (Results.h RulingValue); None = no sample."""
    if raw in m:
        return m[raw]
    return (0.0, 0.0) if raw.startswith("a.") else None


def card_potion_gate(cls, anchor, new):
    """The two gates every automatic acceptance passes: CLASS IDENTITY (the class's bands, measured on `new`, may not end further outside
    their bounds than on `anchor`, the class's start table) and POTIONS (no spam; non-restore potions emergency only). Returns
    {"ok", "fails", "card", "potion", "card_line"}; every number is kept for summary.json."""
    a, n = play_metrics(cls, anchor), play_metrics(cls, new)
    card = load_cards().get(CARD_ALIAS.get(cls, cls))
    fails, crow, prow = [], [], []
    for metric, lo, hi, ln in (card["bands"] if card else []):
        raw = SCHEMA_RAW.get(metric, metric)
        x, y = pm_get(a, raw), pm_get(n, raw)
        if x is None or y is None:
            crow.append({"metric": metric, "line": ln, "verdict": "no sample"})
            continue
        va, vn = band_viol(x[0], lo, hi), band_viol(y[0], lo, hi)
        tol = max(CARD_SLACK * band_scale(lo, hi), 2.0 * math.sqrt(x[1] ** 2 + y[1] ** 2))
        bad = vn > va + tol
        crow.append({"metric": metric, "line": ln, "band": [None if lo == -INF else lo, None if hi == INF else hi], "start": round(x[0], 3), "new": round(y[0], 3), "outside_start": round(va, 3), "outside_new": round(vn, 3), "tol": round(tol, 3), "ok": not bad})
        if bad:
            fails.append("card %s: %s %.2f -> %.2f (band %s, %.2f outside, was %.2f, tolerance %.2f)" % (CARD_ALIAS.get(cls, cls), metric, x[0], y[0], "[%s, %s]" % ("-" if lo == -INF else lo, "-" if hi == INF else hi), vn, va, tol))
    def drink_total(m):
        parts = [pm_get(m, k) for k in ("a.drink_health", "a.drink_stamina", "a.drink_magicka", "a.drink_buff")]
        return (sum(p[0] for p in parts), math.sqrt(sum(p[1] ** 2 for p in parts)))
    checks = [("health drinks per fight", pm_get(a, "drinkH/f"), pm_get(n, "drinkH/f"), DRINK_FIGHT_CAP, False),
              ("all potions, % of action time", drink_total(a), drink_total(n), DRINK_SHARE_CAP, False),
              ("non-restore potions, % of action time (emergency only)", pm_get(a, "a.drink_buff"), pm_get(n, "a.drink_buff"), BUFF_SHARE_CAP, True)]
    for name, x, y, cap, strict in checks:
        if x is None or y is None:
            prow.append({"metric": name, "verdict": "no sample"})
            continue
        tol = max(0.05 * cap, 2.0 * math.sqrt(x[1] ** 2 + y[1] ** 2))
        raised = y[0] > x[0] + tol
        bad = raised and (strict or y[0] > cap)   # a rise beyond noise is refused for a non-restore potion always, for a restore only when it ends over the cap; a start already over the cap is not made worse
        prow.append({"metric": name, "cap": cap, "start": round(x[0], 3), "new": round(y[0], 3), "tol": round(tol, 3), "ok": not bad})
        if bad:
            fails.append("potions: %s %.2f -> %.2f (cap %g%s)" % (name, x[0], y[0], cap, ", may not rise" if strict else ""))
    if "buffDrinkAt%" in n:   # the emergency check proper, once the simulator reports it (needs-sim-metric, tuner-auto-decisions.md)
        h = n["buffDrinkAt%"][0]
        prow.append({"metric": "health % at a non-restore drink", "cap": EMERGENCY_HP_PCT, "new": round(h, 2), "ok": h <= EMERGENCY_HP_PCT})
        if h > EMERGENCY_HP_PCT:
            fails.append("potions: a non-restore potion drunk at %.0f%% health (emergency is <= %g%%)" % (h, EMERGENCY_HP_PCT))
    else:
        prow.append({"metric": "health % at a non-restore drink", "verdict": "needs sim metric buffDrinkAt% (not reported yet): the rise + share cap above stand in"})
    return {"ok": not fails, "fails": fails, "card": crow, "potion": prow, "card_line": card["line"] if card else None}


def final_card_check(cls, anchor, fixed, kept):
    """The whole accepted set of a class against his start table. Values accepted one at a time were each gated against the table of their
    time, so interactions can only surface here: while the set fails, its LAST value is dropped. Returns (gate, dropped)."""
    dropped = []
    while True:
        g = card_potion_gate(cls, anchor, fixed + kept)
        if g["ok"] or not kept:
            return g, dropped
        dropped.append(kept.pop())


def auto_decision(kind, what, verdict, evidence, cls=None, **numbers):
    """A decision the driver took by rule (verdict: accepted / rejected / note). The rule and the numbers are in summary.json auto_decisions."""
    d = {"kind": kind, "what": what, "verdict": verdict, "rule": RULES[kind], "evidence": evidence, **({"cls": cls} if cls else {}), **({"numbers": numbers} if numbers else {})}
    SUMMARY.setdefault("auto_decisions", []).append(d)
    log("AUTO DECISION %s [%s]: %s | %s" % (verdict.upper(), kind, what, evidence))


def open_decision(what, evidence, cls=None, kind="note", verdict="note"):
    """cls: the class a decision belongs to, so a stage re-run for some classes (--classes) replaces only THEIR decisions.
    --decisions auto: a call that has no gate of its own becomes a report-only note (or, with verdict, a rejection) in auto_decisions."""
    if A.decisions == "auto":
        auto_decision(kind, what, verdict, evidence, cls=cls)
        return
    SUMMARY["open_decisions"].append({"what": what, "evidence": evidence, **({"cls": cls} if cls else {})})
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
            if (fn, cn) == ("equip", "melee_dist") and ("equip.ranged_dist.b") in KNOB_META and kind == "Logi":
                # BATCH 3 (iaus-sim-v2.md 35): the posture EDGE as ONE knob, both midpoints at once (sim alias equip.crossover; written as the
                # two SetCurveParam lines). A single midpoint moves the crossover only half way, so coordinate descent cannot reach a far edge.
                knobs.append(("equip.crossover", "{c}:equip.crossover={v}", b, [round(b * k, 5) for k in (0.25, 0.35, 0.5, 0.75, 1.33)]))
                KNOB_META["equip.crossover"] = ("mul", 0.02, 1.0, False)
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
    if getattr(A, "knobs", None):   # --knobs: a scoped search (batch 3: the posture edge); every gate of RULE is unchanged
        out = [k for k in out if re.search(A.knobs, k[0])]
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
    gated = cells() if cls in T1 or cls == "beta1" else [cls]   # a T1 value moves every class it is a companion of, a new class too
    cfgs = ("g3", "off", "shipped") if len(gated) > 1 else ("off", "shipped")
    reqs = [req("builtin", c, g, kept if x == 0 else cur, seeds, base_sets if x == 0 else sets) for c in gated for g in cfgs for x in (0, 1)]
    res = jmany(reqs)
    gate = {}
    for i, (c, g) in enumerate([(c, g) for c in gated for g in cfgs]):
        d, se = paired(res[2 * i], res[2 * i + 1])
        gate["%s/%s" % (c, g)] = [round(d, 3), round(se, 3)]
        if worse(d, se, len(gated) > 1) or out_of_band(c, d, se):
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
        if row["accepted"] and A.decisions == "auto":
            g = card_potion_gate(cls, [], [spec])
            row["card_potion"] = g
            if not g["ok"]:
                row["accepted"], row["why"] = False, "card / potion gate: " + "; ".join(g["fails"])
                auto_decision("class card", "rejudge %s" % spec, "rejected", row["why"], cls=cls, gate=g)
        rows.append(row)
        log("rejudge %s: %s" % (spec, row["why"]))
    SUMMARY["stages"]["rejudge"] = rows


RACE_Z = 3.0   # the racing bound (tune 2.11.1, coordinator 11-13 "drop a candidate once it is clearly behind")


def race(base_req, cand_reqs, seeds):
    """RACING (tune 2.11.1). A sweep's candidates are measured on growing prefixes of the train seeds (16, 32, 48, then all) instead of all
    seeds at once. After each look, candidate c (paired d_c, se_c against the sweep's base on that prefix) is DROPPED when its upper bound
    d_c + RACE_Z se_c is under the train bar's 0.15 J, or under the lower bound d_l - RACE_Z se_l of the look's leader. With RACE_Z = 3 the
    normal-approximation chance that a dropped candidate's full-seed d would have reached either line is under 0.14% per look and candidate
    (three looks: under 0.41%). Survivors, and so every value that can pass the train bar or lead the sweep, are measured on ALL the seeds,
    and the bar itself (d > 2.5 se, d >= 0.15) is applied to the full-seed numbers only: racing decides which candidates are worth the full
    seeds, never whether one is accepted. --no-race measures every candidate on every seed (the old driver, the proof's reference).
    Returns (base result on all seeds, [(result on all seeds or None, n seeds it was dropped at or len(seeds))])."""
    seeds = list(seeds)
    looks = [n for n in (16, 32, 48) if n < len(seeds)] if not A.no_race else []
    alive = list(range(len(cand_reqs)))
    dropped = {}
    for n in looks:
        sub = lambda r: r[:6] + (seeds[:n],)
        res = jmany([sub(base_req)] + [sub(cand_reqs[i]) for i in alive])
        st = {i: paired(res[0], r) for i, r in zip(alive, res[1:])}
        lead = max(alive, key=lambda i: st[i][0])
        lcb = st[lead][0] - RACE_Z * st[lead][1]
        keep = [i for i in alive if st[i][0] + RACE_Z * st[i][1] >= 0.15 and (i == lead or st[i][0] + RACE_Z * st[i][1] >= lcb)]
        for i in alive:
            if i not in keep:
                dropped[i] = n
        alive = keep
        if not alive:
            break
    res = jmany([base_req] + [cand_reqs[i] for i in alive])
    full = dict(zip(alive, res[1:]))
    return res[0], [(full.get(i), dropped.get(i, len(seeds))) for i in range(len(cand_reqs))]


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
            b0, rr = race(req("builtin", cls, "g3", base + kept, train), [req("builtin", cls, "g3", base + kept + [tmpl.format(c=cls, v=v)], train) for v in vals], train)
            raced = [(v, n) for (r, n), v in zip(rr, vals) if r is None]
            cands = sorted([(paired(b0, r) + (v,)) for (r, n), v in zip(rr, vals) if r is not None], key=lambda x: -x[0]) or [(-INF, 0.0, vals[0])]
            d, se, v = cands[0]
            spec = tmpl.format(c=cls, v=v)
            row = {"class": cls, "knob": name, "shared": shared, "best": v, "probe": round(sens, 3), "pass": ps + 1, "train": [round(d, 3), round(se, 3)],
                   "sweep": [[c[2], round(c[0], 3), round(c[1], 3)] for c in sorted(cands, key=lambda x: x[2]) if c[0] > -INF], "raced_out": raced, "accepted": False, "spec": spec}
            passed = d > 2.5 * se and d >= 0.15
            if passed or (CFG["force"] and not rows):   # a smoke run pushes its first sweep through every later step
                j = judge(cls, base + kept, spec, forced=not passed)
                row["steps"], row["why"] = j["steps"], j["why"]
                row["accepted"] = passed and j["accepted"]
                if not passed:
                    row["why"] = "train: does not beat noise (later steps were run for the smoke test only); " + j["why"]
                smoke_probe = CFG["force"] and not row["accepted"] and not any("card_potion" in r2 for r2 in rows)   # a smoke run exercises the gate on its first forced value too
                if (row["accepted"] or smoke_probe) and A.decisions == "auto":   # CLASS IDENTITY + POTIONS: the value in the table it joins, against the class's start table
                    g = card_potion_gate(cls, base, base + kept + [spec])
                    row["card_potion"] = g
                    if smoke_probe:
                        auto_decision("class card", "SMOKE %s %s -> %g" % (cls, name, v), "note", "SMOKE ONLY: the gate run on a value the train step did not accept, to exercise it: gate %s%s" % ("holds" if g["ok"] else "REJECTS", "" if g["ok"] else " (" + "; ".join(g["fails"]) + ")"), cls=cls, gate=g)
                    elif not g["ok"]:
                        row["accepted"], row["why"] = False, "card / potion gate: " + "; ".join(g["fails"])
                        auto_decision("class card", "%s %s -> %g" % (cls, name, v), "rejected", row["why"], cls=cls, gate=g)
            else:
                row["why"] = ("train: does not beat noise" if not d > 2.5 * se else "train: beats noise but under 0.15 J") if d > -INF else "train: every value raced out (under the 0.15 J bar or behind the leader at %g sigma)" % RACE_Z
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
    if kept and A.decisions == "auto":   # the accepted set as a whole against his card (each value was gated one at a time)
        g, dropped = final_card_check(cls, base, base, kept)
        for sp in dropped:
            for r in rows:
                if r["spec"] == sp and r["accepted"]:
                    r["accepted"], r["why"] = False, "card / potion gate on the whole set: " + "; ".join(g["fails"])
        auto_decision("class card", "class card read for %s" % cls, "rejected" if dropped else "accepted",
                      "accepted %s%s: every band of his card and the potion limits hold against his start table" % (" ".join(kept) or "none", ("; DROPPED by the whole-set gate: " + " ".join(dropped)) if dropped else ""), cls=cls, gate=g, dropped=dropped)
    if kept:   # what the accepted values change in his play, for the class-card read (informational: the card gate above decides)
        # The simulator writes a --baseline file only in single-seed mode (a --seeds run prints SEED lines and nothing else), so this read
        # was ALWAYS empty before batch 3 (found 11-13: every play_changes_natural of every run was []). One seed, 48 fights per cell.
        cmd = [A.bin, "--no-json", "--threads", "2", "--classes", cls, "--seed", "1", "--fights", "48", "--rating", "0", "--baseline", "/dev/stdout"] + world_args("builtin")
        for c in base:
            cmd += ["--curve", c]
        b = text_run("metrics1:" + cls + ":" + " ".join(base), list(cmd), "builtin")
        for c in kept:
            cmd += ["--curve", c]
        n = text_run("metrics1:" + cls + ":" + " ".join(base + kept), cmd, "builtin")
        pick = lambda t: {m.group(1): float(m.group(2)) for m in re.finditer(r"^%s\|natural\|([^=]+)=([-0-9.e]+)" % re.escape(cls), t, re.M)}
        ob, nb = pick(b), pick(n)
        ch = sorted(((abs(nb[k] - ob[k]) / (abs(ob[k]) + 1.0), k, ob[k], nb[k]) for k in ob if k in nb and abs(nb[k] - ob[k]) > 1e-9), reverse=True)[:12]
        out["play_changes_natural"] = [[k, o, w] for _, k, o, w in ch]
        if A.decisions == "ask":
            open_decision("class card read for %s" % cls, "accepted %s; the largest changes in his play are in summary.json stages.classes.%s.play_changes_natural: confirm none leaves his class card before writing" % (" ".join(kept), cls), cls=cls)
    # runbook open decision 7: many accepted values for one class, or a consideration weight at the edge of what was tried, say the shared
    # catalogue (not the class) is off: reported before anything is written
    edge = [r["spec"] for r in rows if r["accepted"] and r["knob"].endswith(".w") and (r["best"] >= 2.0 * r["shared"] - 1e-6 or r["best"] <= 0.5 * r["shared"] + 1e-6)]
    if len(kept) > 10:
        open_decision("%s took %d accepted values (more than 10)" % (cls, len(kept)), "a sign the shared catalogue, not the class, is off: report before writing any of %s" % " ".join(kept), cls=cls, kind="many values")
    if edge:
        open_decision("%s: a consideration weight accepted at the edge of the range tried (x2 or x0.5)" % cls, "%s: the best weight may lie outside what was searched, and so far from the catalogue's that the shared curve is in question" % " ".join(edge), cls=cls, kind="weight at edge")
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
    if path == "equip.crossover":   # the posture edge: both distance curves' midpoint (Family::Posture 8, Cons 35 ranged_dist / 36 melee_dist)
        return "\n".join('SetCurveParam(%s, static_cast<Family>(8), static_cast<Cons>(%d), sh.curve[%d], "b", %s);' % (row, k, k, v) for k in (35, 36))
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
    for cls, v in SUMMARY["stages"].get("gap", {}).get("kept", {}).items():   # the gap stage's (a) search
        out += [k for k in v if k not in out]
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

        def table_vs(a, b):   # per cell and config, and pooled over the cells: b - a
            res = jmany([req("builtin", c, g, x, seeds) for c in cells() for g in CONFIGS for x in (a, b)])
            per, pooled, i = {}, {}, 0
            for c in cells():
                for g in CONFIGS:
                    per["%s/%s" % (c, g)] = paired(res[i], res[i + 1])
                    pooled.setdefault(g, []).append([y[0] - x[0] for x, y in zip(res[i], res[i + 1])])
                    i += 2
            out = {}
            for g in CONFIGS:
                dd = [sum(col) / len(col) for col in zip(*pooled[g])]
                d = sum(dd) / len(dd)
                out[g] = (d, math.sqrt(sum((x - d) ** 2 for x in dd) / (len(dd) - 1) / len(dd)) if len(dd) > 1 else 0.0)
            return per, out
        per, pooled = table_vs([], curves)
        for k, (d, se) in per.items():
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
        if A.decisions == "auto":   # CLASS IDENTITY + POTIONS of every cell in the final table, against the shipped one
            st["card_potion"] = {}
            for c in cells():
                g = card_potion_gate(c, [], curves)
                st["card_potion"][c] = g
                fails += ["%s: %s" % (c, f) for f in g["fails"]]
        st["verdict"] = "PASS" if not fails else "FAIL: " + "; ".join(fails)
        log("joint: %d values, pooled g3 %+.2f (se %.2f) off %+.2f shipped %+.2f  %s" % (len(curves), pooled["g3"][0], pooled["g3"][1], pooled["off"][0], pooled["shipped"][0], st["verdict"]))
        if fails:
            open_decision("JOINT GATE FAILED: the final table as a whole", "write NO value from this run: %s" % "; ".join(fails), kind="joint", verdict="rejected")
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
    # Batch 3 (marth 11-13, "only if small"): a multiplicative knob whose tried values already reach x0.25 / x4 of the shared value got
    # nothing past its edge here ("EDGE IS THE VALID LIMIT" for equip.crossover 0.1125 while its limit is 0.02). Keep only the values PAST
    # the outermost tried on this side, and with none, step on from that outermost value (x0.7 / x0.5 down, x1.4 / x2 up), inside the range.
    if mode == "mul" and shared != 0.0:
        side_t = [t for t in tried if (t / shared > 1.0) == (side == "up")]
        if side_t:
            outer = max(side_t, key=lambda t: abs(math.log(abs(t / shared))) if t / shared > 0 else INF)
            past = (lambda v: v / shared > outer / shared + 1e-9) if side == "up" else (lambda v: v / shared < outer / shared - 1e-9)
            out = [v for v in out if past(v)]
            if not out:
                for k in ((1.4, 2.0) if side == "up" else (0.7, 0.5)):
                    v = round(min(hi, max(lo, outer * k)), 5)
                    if past(v) and all(abs(v - x) > 1e-6 for x in out):
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
    rows = [e for e in SUMMARY["stages"].get("edge", []) if scope() and e["class"] not in scope()]   # a scoped run keeps the other classes' rows
    for cls, r in SUMMARY["stages"].get("classes", {}).items():
        if not r.get("complete") or (scope() and cls not in scope()):
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
                e["rounds"] = []
                for rnd in range(EDGE_ROUNDS if A.decisions == "auto" else 1):   # auto: a widened value that is accepted is widened again (bounded)
                    if rnd:
                        vals = widen(row["knob"], shared, tried, side)
                        if not vals:
                            e["verdict"] += "; the range's valid limit is reached"
                            break
                    base = r["base"] + r["kept"]
                    res = jmany([req("builtin", cls, "g3", base, train)] + [req("builtin", cls, "g3", [k for k in base if k != row["spec"]] + [tmpl.format(c=cls, v=v)], train) for v in vals])
                    cands = sorted(((paired(res[0], x) + (v,)) for x, v in zip(res[1:], vals)), key=lambda c: -c[0])
                    e["sweep"] += [[c[2], round(c[0], 3), round(c[1], 3)] for c in sorted(cands, key=lambda c: c[2])]
                    d, se, v = cands[0]
                    e["train"] = [round(d, 3), round(se, 3)]
                    passed = d > 2.5 * se and d >= 0.15
                    if passed or (CFG["force"] and not any("steps" in x for x in rows) and not rnd):   # a smoke run pushes its first widened sweep through the judge
                        spec = tmpl.format(c=cls, v=v)
                        j = judge(cls, base, spec, forced=not passed, replace=row["spec"])
                        j["accepted"] = passed and j["accepted"]
                        e["steps"], e["best"] = j["steps"], v
                        if j["accepted"] and A.decisions == "auto":
                            g = card_potion_gate(cls, r["base"], [k for k in base if k != row["spec"]] + [spec])
                            e["card_potion"] = g
                            if not g["ok"]:
                                j["accepted"], j["why"] = False, "card / potion gate: " + "; ".join(g["fails"])
                                auto_decision("edge", "%s %s %g -> %g (past the edge)" % (cls, row["knob"], row["best"], v), "rejected", j["why"], cls=cls, gate=g, train=e["train"], holdout=j["steps"]["holdout"])
                        if j["accepted"] and A.decisions == "auto":   # the wider value replaces the accepted one in the class's table
                            r["kept"] = [spec if k == row["spec"] else k for k in r["kept"]]
                            auto_decision("edge", "%s %s %g -> %g (past the edge)" % (cls, row["knob"], row["best"], v), "accepted",
                                          "shared %g, edge (%s) %g, widened %g: train %+.2f (se %.2f) over the previous value, holdout %+.2f, cross-world mean %+.2f (se %.2f); card and potion gates hold" % (shared, side, row["best"], v, d, se, j["steps"]["holdout"][0], j["steps"]["cross_world"]["mean/g3"][0], j["steps"]["cross_world"]["mean/g3"][1]),
                                          cls=cls, gate=e["card_potion"], round=rnd + 1)
                            row["spec"], row["best"] = spec, v
                            e["accepted_now"], e["verdict"] = v, "WIDER VALUE ACCEPTED (round %d): judge + card + potion gates hold; it replaces the edge value" % (rnd + 1)
                            tried = list(tried) + list(vals)
                            e["rounds"].append({"widened": vals, "best": v, "accepted": True})
                            side = edge_of(row["knob"], shared, tried, v)
                            if not side:
                                break
                            continue
                        if j["accepted"]:   # ask mode: reported, never shipped
                            e["verdict"] = "WIDER VALUE PASSES THE RULE: reported, never shipped (far from the shared curve)"
                            open_decision("%s %s: a value past the edge passes the rule" % (cls, row["knob"]), "shared %g, accepted (edge, %s) %g, widened %g: train %+.2f (se %.2f) over the accepted value, holdout %+.2f, cross-world mean %+.2f (se %.2f). Far from the shared curve: marth or the coordinator decides; the accepted %g stays written until then (summary.json stages.edge)"
                                          % (shared, side, row["best"], v, d, se, j["steps"]["holdout"][0], j["steps"]["cross_world"]["mean/g3"][0], j["steps"]["cross_world"]["mean/g3"][1], row["best"]), cls=cls)
                        else:
                            e["verdict"] = "the accepted %sedge value holds (%s: %s)" % ("(widened) " if rnd else "", "a wider value beat it on train only" if passed else "SMOKE: judged without a train pass", j["why"])
                    else:
                        e["verdict"] = "the accepted %sedge value holds (no wider value beats it: train %+.2f, se %.2f)" % ("(widened) " if rnd else "", d, se)
                    e["rounds"].append({"widened": vals, "best": v, "accepted": False})
                    break
            rows.append(e)
            log("edge %s %s %g (accepted %g, %s) widened %s: %s" % (cls, row["knob"], shared, row["best"], side, vals, e["verdict"]))
            SUMMARY["stages"]["edge"] = rows
            save_summary()
    SUMMARY["stages"]["edge"] = rows
    if not rows:
        log("edge: no accepted value sits at the edge of its range")


GLOBALS = [("t.momentum", 1.25, [1.0, 1.05, 1.1, 1.15, 1.2]), ("t.powerCd", 10.0, [5.0, 7.0]), ("t.postureMomentum", 1.5, [1.75, 2.0])]


# ── the gap stage ────────────────────────────────────────────────────────────────────────────────────────────────────────────
# What an extended action of the oracle (Oracle.h Ext) corresponds to in the catalogue (None = no family), what road the game has for
# it (iaus-sim-v2.md section 25), and where the oracle's version is known to be MORE FAVOURABLE than the game's (class d, with the
# evidence). Edited only with a source; a label missing here is classified by the generic rule and says so.
EXT_FAMILY = {"ext:block": "block", "ext:bash": "bash", "ext:hide": "disengage", "ext:flee": "disengage", "ext:scroll": "use_scroll", "ext:to_player": "regroup",
              "ext:to_healer": "regroup", "ext:step_back": "give_ground", "ext:sprint_back": "back_off", "ext:get_sight": None}
EXT_ROAD = {"ext:step_back": "no road today (section 25: PROVEN LIMIT; needs a Harbinger combat area centred on a point, tier A); GiveGround is lab-only",
            "ext:sprint_back": "the BackOff family (batch I, H3 run) where its gate allows; otherwise no road (section 25)",
            "ext:to_player": "ROAD EXISTS (Harbinger ch.24 CombatApproach): the Regroup family is built and never generated (Snapshot::regroupRoad, section 25.1)",
            "ext:to_healer": "ROAD EXISTS (Harbinger ch.24): the Regroup family is built and never generated (section 25.1)",
            "ext:get_sight": "ROAD EXISTS for casts only (apmf/OffenseApproach.cpp, ch.24); no IAUS family",
            "ext:scroll": "card S3 (the lab's --scroll-road), planned", "ext:bash": "a timed bash has no road (section 25: PROVEN LIMIT); the Bash family needs the leaf promote",
            "ext:block": "Harbinger held guard (batch S, Q-melee-1)", "ext:hide": "card G3 hide run (Disengage family, its class gate)", "ext:flee": "card G3 flee (Disengage family, its 30% line)"}
FIDELITY = {"ext:block": "the oracle's guard (Oracle.h Apply, Ext::Block) is raised for 1.5 s at any moment with NO cooldown and NO rest after it; the game's "
                         "Harbinger guard holds 2.5 s, its row cools 4 s and a 1.5 s rest follows a hold (Q-melee-1, section 33.2)",
            "ext:bash": "the oracle's bash (Oracle.h Apply, Ext::Bash) has NO cooldown and lands on demand; IAUS's Bash family is held to Tuning::bashCooldownSec "
                        "and the game has no road for a timed bash at all (section 25)"}
GAP_SEEDS0 = 6001   # a fresh range (1.., 1001.., 3001.., 4001.., 5001.., 8001.., 9001.. are used by the other stages)


def gap_cmd(world, cls, curves, seed, subject_only=True):
    cmd = [A.bin, "--no-json", "--threads", "1", "--seed", str(seed), "--classes", cls, "--fights", "1", "--rating", "0",
           "--class-oracle", str(CFG["gap_nat"]), "--gap-scenarios", str(CFG["gap_scen"]), "--gap-dump"] + (["--oracle-subject-only"] if subject_only else []) + world_args(world)
    for c in curves:
        cmd += ["--curve", c]
    return cmd


def gap_parse(txt, cls, seed):
    """One gap run's dump. A run that printed no GAPFIGHT line for its class is an ERROR (never read as a gap of 0)."""
    fights = [(m.group(1), int(m.group(2)), float(m.group(3)), float(m.group(4))) for m in re.finditer(r"^GAPFIGHT %s (\S+) (\d+) ([-0-9.]+) ([-0-9.]+)$" % re.escape(cls), txt, re.M)]
    if len(fights) < CFG["gap_nat"]:
        raise RuntimeError("gap run of %s seed %d printed %d fights (%d natural expected): a changed dump format or a failed pass:\n%s" % (cls, seed, len(fights), CFG["gap_nat"], txt[-600:]))
    sit = {m.group(1): (int(m.group(2)), float(m.group(3))) for m in re.finditer(r"^GAPSIT %s (\S+) (\d+) ([-0-9.]+)$" % re.escape(cls), txt, re.M)}
    gen = {m.group(1): int(m.group(2)) for m in re.finditer(r"^GAPGEN %s (\S+) (\d+)$" % re.escape(cls), txt, re.M)}
    dec = re.search(r"^GAPDEC %s (\d+)$" % re.escape(cls), txt, re.M)
    return {"fights": [(seed, j, k, ui, uo) for k, j, ui, uo in fights], "sit": sit, "gen": gen, "dec": int(dec.group(1)) if dec else 0}


def gap_runs(tasks):
    """tasks: (world, cls, curves, seed, subject_only). All at once on the pool; each run is a cached text run (resumable)."""
    def one(t):
        w, cls, curves, seed, so = t
        cmd = gap_cmd(w, cls, curves, seed, so)
        return gap_parse(text_run("gap:" + json.dumps(cmd[1:]), cmd, w, check=lambda t: gap_parse(t, cls, seed)), cls, seed)
    return list(POOL.map(one, tasks))


def mse(xs):
    n = len(xs)
    m = sum(xs) / n if n else 0.0
    return m, (math.sqrt(sum((x - m) ** 2 for x in xs) / (n - 1) / n) if n > 1 else 0.0)


def gap_merge(parts):
    out = {"fights": [], "sit": {}, "gen": {}, "dec": 0}
    for p in parts:
        out["fights"] += p["fights"]
        out["dec"] += p["dec"]
        for k, (n, du) in p["sit"].items():
            a = out["sit"].get(k, (0, 0.0))
            out["sit"][k] = (a[0] + n, a[1] + du)
        for k, n in p["gen"].items():
            out["gen"][k] = out["gen"].get(k, 0) + n
    return out


def classify(label_i, label_o, kind, gen):
    """(a) curve / weight, (b) missing consideration or input (a gate, a veto, a broken ruling), (c) missing behaviour (the oracle's
    action has no candidate IAUS ever generates for this class), (d) a modelling suspect (the oracle's version of the action is known
    to be more favourable than the game's: FIDELITY). Returns (class, why, families the (a) search may move)."""
    fi = label_i.rstrip("!")
    if label_o in FIDELITY:
        return "d", FIDELITY[label_o], []
    if label_i.endswith("!"):
        return "b", "IAUS picked %s, which the class's rulings forbid at that moment (a missing gate in IAUS)" % fi, []
    if kind == "ext":
        fam = EXT_FAMILY.get(label_o, "?")
        road = EXT_ROAD.get(label_o, "not in the road table: classified by the generic rule")
        if fam and gen.get(fam, 0) > 0:
            return "b", "the %s family exists and was offered %d times in these fights, but its gate did not offer it here; road: %s" % (fam, gen[fam], road), []
        return "c", "IAUS never generates %s for this class (%s); road: %s" % (label_o, "no family" if not fam else "family %s offered 0 times" % fam, road), []
    if kind == "unranked":
        return "b", "IAUS generated %s and scored it under the floor or vetoed it (a veto, a gate or a missing consideration)" % label_o, []
    fams = sorted({f for f in (fi, label_o) if f not in ("idle",) and not f.startswith("ext:")})
    return "a", "%s: IAUS ranked the oracle's pick lower (%s)" % (kind, " / ".join(fams) or "the act floor"), fams


def stage_gap():
    """THE FINAL ORACLE GAP (marth 11-12: "getting close to oracle should be a goal"; re-scope: the driver itself iterates through what
    needs it). Per class and world, on the shipped table and on the final table, the SAME fights (seeds 6001..) played by IAUS and by
    the class oracle planning for the subject only (--oracle-subject-only: his companions play IAUS in both arms, so the gap is his own).
    RULE of this stage (written before any of its results was read):
      size     12 seeds x (4 natural + 1 of each of his own named scenarios); a cell with under 2 fights or whose final-table se > max(0.5 U, |gap| / 4) gets
               12 more seeds, up to 48. The shipped table is measured on the same seeds, so the tuning's effect is a PAIRED difference.
      classes  every divergence cluster of the subject (Oracle.h SitRec) is classified by `classify` (a / b / c / d) with its cost
               (the oracle's dU summed per fight: an ordering, not an additive share of the gap).
      (a)      the families of the (a) clusters that carry the most dU (up to 3 per class) drive a BOUNDED extra search: the class's live
               knobs of those families that took no value (up to 12, most sensitive first), at the MIDPOINTS between the shared value
               and each value already tried (inside the range: accepted under RULE like any sweep) and past the outermost ones
               (widen: a pass is an open decision, never shipped). An accepted value re-runs the joint gate and re-measures that gap.
      (b)-(d)  open decisions with their evidence; the ledger rows go to <out>/gap-ledger.md for the coordinator to paste."""
    old = SUMMARY["stages"].pop("gap", {})   # this stage's own earlier values are decided again (for the classes in scope; the others are kept)
    order = scope() or cells()
    keep = lambda part: {k: v for k, v in old.get(part, {}).items() if k.split("/")[0] not in order}
    if scope():
        SUMMARY["stages"]["gap"] = {"kept": keep("kept")}   # the other classes' accepted values stay in final_curves
    shipped, final = [], final_curves()
    tables = {"shipped": shipped, "final": final}
    st = {"rule": stage_gap.__doc__[stage_gap.__doc__.index("RULE"):], "tables": {"final": final}, "cells": keep("cells"), "clusters": keep("clusters"), "kept": keep("kept"), "search": keep("search")}
    nseeds = {(w, c): CFG["gap_seeds"] for w in A.worlds for c in order}
    data = {}
    while True:   # measure; then grow only the cells whose se is too wide, up to the cap
        tasks = [(w, c, tables[t], s, True) for (w, c), n in nseeds.items() for t in ("shipped", "final") for s in range(GAP_SEEDS0, GAP_SEEDS0 + n) if t == "final" or final]
        res = gap_runs(tasks)
        for t, r in zip(tasks, res):
            data[(t[0], t[1], "final" if t[2] is final else "shipped", t[3])] = r
        grow = []
        for (w, c), n in nseeds.items():
            fin = gap_merge([data[(w, c, "final", s)] for s in range(GAP_SEEDS0, GAP_SEEDS0 + n)])
            g, se = mse([uo - ui for _, _, _, ui, uo in fin["fights"]])
            if (len(fin["fights"]) < 2 or se > max(0.5, abs(g) / 4.0)) and n < CFG["gap_max_seeds"]:   # one fight has no se: it is never "small enough"
                grow.append((w, c))
        if not grow:
            break
        for k in grow:
            nseeds[k] = min(CFG["gap_max_seeds"], nseeds[k] + CFG["gap_seeds"])
        log("gap: %d cell(s) get more seeds (se too wide): %s" % (len(grow), ", ".join("%s/%s->%d" % (w, c, nseeds[(w, c)]) for w, c in grow)))
    allf = gap_runs([("builtin", c, final, s, False) for c in order for s in range(GAP_SEEDS0, GAP_SEEDS0 + CFG["gap_seeds"])]) if "builtin" in A.worlds else []
    for c in order:
        pooled = []
        for w in A.worlds:
            n = nseeds[(w, c)]
            fin = gap_merge([data[(w, c, "final", s)] for s in range(GAP_SEEDS0, GAP_SEEDS0 + n)])
            shp = gap_merge([data[(w, c, "final" if not final else "shipped", s)] for s in range(GAP_SEEDS0, GAP_SEEDS0 + n)])
            df = {(s, j): uo - ui for s, j, _, ui, uo in fin["fights"]}
            ds = {(s, j): uo - ui for s, j, _, ui, uo in shp["fights"]}
            g, se = mse(list(df.values()))
            g0, se0 = mse(list(ds.values()))
            ch, chse = mse([df[k] - ds[k] for k in df if k in ds])
            nat = mse([uo - ui for _, _, k, ui, uo in fin["fights"] if k == "nat"])
            cell = {"gap": round(g, 3), "se": round(se, 3), "fights": len(df), "seeds": n, "nat_gap": round(nat[0], 3), "shipped_gap": round(g0, 3), "shipped_se": round(se0, 3),
                    "tuning_change": [round(ch, 3), round(chse, 3)], "iaus_u": round(mse([ui for *_, ui, _ in fin["fights"]])[0], 2), "oracle_u": round(mse([uo for *_, uo in fin["fights"]])[0], 2),
                    "decisions": fin["dec"]}
            if w == "builtin" and allf:
                af = gap_merge([r for r, cc in zip(allf, [c2 for c2 in order for _ in range(CFG["gap_seeds"])]) if cc == c])
                cell["all_followers_gap"] = [round(x, 3) for x in mse([uo - ui for *_, ui, uo in af["fights"]])]
            st["cells"]["%s/%s" % (c, w)] = cell
            pooled.append(fin)
            log("gap %s on %s: %+.2f (se %.2f, %d fights) shipped %+.2f, tuning %+.2f (se %.2f)" % (c, w, g, se, len(df), g0, ch, chse))
        # the divergences of the final table, pooled over the worlds, one row per (iaus pick, oracle pick, kind)
        allp = gap_merge(pooled)
        nf = max(1, len(allp["fights"]))
        rows = {}
        for key, (n, du) in allp["sit"].items():
            li, lo, kind, sit = key.split("|")
            r = rows.setdefault((li, lo, kind), {"iaus": li, "oracle": lo, "kind": kind, "n": 0, "dU_per_fight": 0.0, "situations": {}})
            r["n"] += n
            r["dU_per_fight"] += du / nf
            r["situations"][sit] = r["situations"].get(sit, 0.0) + du / nf
        tot = sum(r["dU_per_fight"] for r in rows.values()) or 1.0
        out = []
        for r in sorted(rows.values(), key=lambda r: -r["dU_per_fight"]):
            cl, why, fams = classify(r["iaus"], r["oracle"], r["kind"], allp["gen"])
            r.update({"class": cl, "why": why, "families": fams, "share": round(r["dU_per_fight"] / tot, 3), "dU_per_fight": round(r["dU_per_fight"], 3),
                      "situations": [[k, round(v, 3)] for k, v in sorted(r["situations"].items(), key=lambda kv: -kv[1])[:3]]})
            out.append(r)
        st["clusters"][c] = {"fights": nf, "decisions": allp["dec"], "dU_per_fight": round(tot, 3), "by_class": {k: round(sum(r["dU_per_fight"] for r in out if r["class"] == k), 3) for k in "abcd"},
                             "rows": out, "generated": allp["gen"]}
        SUMMARY["stages"]["gap"] = st
        save_summary()
    st["search_changed"] = gap_search(st, order)
    gap_ledger(st, order)
    SUMMARY["stages"]["gap"] = st


def gap_search(st, order):
    """(a): the bounded extra search of the knobs that drive the class's (a) divergences (RULE in stage_gap)."""
    knobs, _ = all_knobs()
    train = range(1, CFG["train"] + 1)
    changed = []
    for c in order:
        cl = st["clusters"][c]
        fam_du = {}
        for r in cl["rows"]:
            if r["class"] == "a":
                for f in r["families"]:
                    fam_du[f] = fam_du.get(f, 0.0) + r["dU_per_fight"]
        fams = [f for f, _ in sorted(fam_du.items(), key=lambda kv: -kv[1])[:CFG["gap_families"]]]
        live = SUMMARY["stages"].get("classes", {}).get(c, {}).get("live")
        meta = {n: (t, sh, vals) for n, t, sh, vals in class_knobs(c, knobs)}
        taken = {k.split("=")[0] for k in final_curves()}
        if live is None:   # the class was not searched in this --out: its knobs' order is the liveness screen of the classes stage, which never ran
            st["search"][c] = {"families": fams, "verdict": "NOT SEARCHED: the classes stage has not run for %s in this --out (no liveness list)" % c, "rows": []}
            log("gap search %s: %s" % (c, st["search"][c]["verdict"]))
            continue
        cand = [n for n, _ in live if n in meta and any(n == "family." + f or n.startswith(f + ".") for f in fams) and meta[n][0].format(c=c, v=0).split("=")[0] not in taken][:CFG["gap_knobs"]]
        srch = {"families": fams, "knobs": cand, "rows": []}
        for name in cand:
            tmpl, shared, tried = meta[name]
            mids = [round(shared + (v - shared) / 2.0, 5) for v in tried]
            wide = widen(name, shared, tried, "up") + widen(name, shared, tried, "down")
            vals = [v for v in mids + wide if all(abs(v - t) > 1e-6 for t in tried + [shared])]
            if not vals:
                continue
            base = final_curves()
            res = jmany([req("builtin", c, "g3", base, train)] + [req("builtin", c, "g3", base + [tmpl.format(c=c, v=v)], train) for v in vals])
            cands = sorted(((paired(res[0], x) + (v,)) for x, v in zip(res[1:], vals)), key=lambda x: -x[0])
            sweep = [[x[2], round(x[0], 3), round(x[1], 3)] for x in sorted(cands, key=lambda x: x[2])]
            # G1 (review of 7e883a9a): the best IN-RANGE value and the best WIDENED value are judged separately, so a far value that wins
            # on train never hides an in-range one that would pass (the in-range one may be written; the far one is only ever reported).
            for far in (False, True):
                grp = [x for x in cands if (x[2] in wide) == far]
                if not grp:
                    continue
                d, se, v = grp[0]
                row = {"knob": name, "shared": shared, "best": v, "far": far, "train": [round(d, 3), round(se, 3)], "sweep": sweep, "accepted": False}
                passed = d > 2.5 * se and d >= 0.15
                if passed or (CFG["force"] and not far and not srch["rows"]):
                    spec = tmpl.format(c=c, v=v)
                    j = judge(c, base, spec, forced=not passed)
                    row["steps"], row["why"] = j["steps"], j["why"]
                    ok = passed and j["accepted"]
                    if ok and A.decisions == "auto":   # in range or far: the same two gates (class identity, potions) against the class's start table
                        g = card_potion_gate(c, SUMMARY["stages"].get("classes", {}).get(c, {}).get("base", []), base + [spec])
                        row["card_potion"] = g
                        if not g["ok"]:
                            ok, row["why"] = False, "card / potion gate: " + "; ".join(g["fails"])
                            auto_decision("gap far" if far else "class card", "gap search %s %s %g -> %g" % (c, name, shared, v), "rejected", row["why"], cls=c, gate=g)
                        elif far:
                            auto_decision("gap far", "gap search %s %s %g -> %g (past the range tried)" % (c, name, shared, v), "accepted",
                                          "train %+.2f (se %.2f), holdout %+.2f, cross-world mean %+.2f; card and potion gates hold" % (d, se, j["steps"]["holdout"][0], j["steps"]["cross_world"]["mean/g3"][0]), cls=c, gate=g)
                    if ok and far and A.decisions == "ask":
                        row["why"] = "PASSES THE RULE PAST THE RANGE TRIED: reported, never shipped"
                        open_decision("gap search %s %s %g -> %g (past the range tried)" % (c, name, shared, v), "train %+.2f (se %.2f), holdout %+.2f, cross-world mean %+.2f: far from the shared curve, marth or the coordinator decides (summary.json stages.gap.search.%s)" % (d, se, j["steps"]["holdout"][0], j["steps"]["cross_world"]["mean/g3"][0], c), cls=c)
                    elif ok:
                        row["accepted"] = True
                        if far:
                            row["why"] = "ACCEPTED PAST THE RANGE TRIED: judge + card + potion gates hold"
                        st["kept"].setdefault(c, []).append(spec)
                        changed.append(c)
                else:
                    row["why"] = "train: does not beat noise" if not d > 2.5 * se else "train: beats noise but under 0.15 J"
                srch["rows"].append(row)
                log("gap search %s %-30s %g -> %g%s train %+.2f (se %.2f) %s" % (c, name, shared, v, " (far)" if far else "", d, se, row["why"][:120]))
            st["search"][c] = srch
            SUMMARY["stages"]["gap"] = st
            save_summary()
        st["search"][c] = srch
    if changed:   # the table moved: the joint gate again, and the gap of each class that took a value measured once more on the new table
        SUMMARY["stages"]["gap"] = st
        stage_joint()
        st["joint_after_search"] = SUMMARY["stages"]["joint"]["verdict"]
        final = final_curves()
        for c in sorted(set(changed)):
            if A.decisions == "auto":   # the set the gap search added, with the table as it now is, against his start table
                g, dropped = final_card_check(c, SUMMARY["stages"].get("classes", {}).get(c, {}).get("base", []), [k for k in final if k not in st["kept"][c]], st["kept"][c])
                auto_decision("class card", "class card read for %s (gap search)" % c, "rejected" if dropped else "accepted",
                              "accepted %s in the gap stage's (a) search%s: card and potion gates against his start table" % (" ".join(st["kept"][c]) or "none", ("; DROPPED by the whole-set gate: " + " ".join(dropped)) if dropped else ""), cls=c, gate=g, dropped=dropped)
                final = final_curves()
            else:
                open_decision("class card read for %s (gap search)" % c, "accepted %s in the gap stage's (a) search: confirm none leaves his class card before writing" % " ".join(st["kept"][c]), cls=c)
            for w in A.worlds:
                n = st["cells"]["%s/%s" % (c, w)]["seeds"]
                fin = gap_merge(gap_runs([(w, c, final, s, True) for s in range(GAP_SEEDS0, GAP_SEEDS0 + n)]))
                g, se = mse([uo - ui for *_, ui, uo in fin["fights"]])
                st["cells"]["%s/%s" % (c, w)]["after_search"] = [round(g, 3), round(se, 3)]
                log("gap %s on %s after the (a) search: %+.2f (se %.2f)" % (c, w, g, se))
    return sorted(set(changed))


def gap_ledger(st, order):
    """(b)-(d) as open decisions (the largest per class, with the evidence) and as ledger rows for iaus-classes.md section 3."""
    L = ["# Oracle-gap ledger rows (tune_driver.py stage gap, %s)" % time.strftime("%Y-%m-%d %H:%M"), "",
         "Paste into Docs/research/2026-11/iaus-classes.md section 3. dU/f = the oracle's per-decision advantage summed per fight (an ordering, not an additive share of the gap).", "",
         "| Id | Classes | What is missing | Road |", "|---|---|---|---|"]
    for c in st["clusters"]:   # every class the stage holds (a scoped run kept the others'); decisions only for the classes in scope
        cl = st["clusters"][c]
        gaps = ", ".join("%s %+.2f (se %.2f)" % (w, st["cells"]["%s/%s" % (c, w)]["gap"], st["cells"]["%s/%s" % (c, w)]["se"]) for w in A.worlds)
        rows = [r for r in cl["rows"] if r["class"] in "bcd" and (r["share"] >= 0.05 or r is cl["rows"][0])][:5]
        for i, r in enumerate(rows):
            L.append("| O-%s-%d | %s | (%s) IAUS %s, oracle %s (%s): dU/f %.2f, %.0f%% of his divergence cost; most in %s. Gap %s | %s |" % (
                c, i + 1, c, r["class"], r["iaus"], r["oracle"], r["kind"], r["dU_per_fight"], 100 * r["share"], r["situations"][0][0] if r["situations"] else "-", gaps, r["why"]))
        if rows and c in order:
            open_decision("oracle gap %s: %d divergence(s) of class b/c/d" % (c, len(rows)), "; ".join("(%s) %s -> %s dU/f %.2f: %s" % (r["class"], r["iaus"], r["oracle"], r["dU_per_fight"], r["why"]) for r in rows) + " (summary.json stages.gap.clusters.%s, <out>/gap-ledger.md)" % c, cls=c, kind="oracle")
    open(os.path.join(A.out, "gap-ledger.md"), "w").write("\n".join(L) + "\n")


LOGI_NO_ROW = {"gold", "gem", "jewelry", "ingredient", "junk"}   # Logistics.h: no default logistics row takes these (Followers.cpp:48-51)
LOGI_GEAR = {"weapon_1h", "weapon_2h", "bow", "armor_body", "armor_head"}   # loot_equipment takes strict upgrades only (LootEquipment.cpp)


def logi_class(kind, base, orc):
    """(c) the oracle does what today's rules have no behaviour for: a sell run, a take of a category no row names, a take of gear the
    upgrade-only rule leaves (gathering to sell); (a) the rest: a row's or an INI value (potion floor, ammo keep target, row params)."""
    if kind == "sell_run":
        return "c", "today's rules never make a sell run mid-dungeon"
    if orc == "orc:take" and kind in LOGI_NO_ROW:
        return "c", "no default row takes %s (Followers.cpp:48-51): a category or a scorer that judges it is missing" % kind
    if orc == "orc:take" and kind in LOGI_GEAR:
        return "c", "loot_equipment takes strict upgrades only: gathering %s to sell is not a behaviour today" % kind
    return "a", "a row's or an INI value decides this %s (potion floor, keep target, row param)" % kind


def stage_logistics():
    """THE LOGISTICS WORLD (Logistics.h; marth 11-12 "Loot sim"): today's loot rules (the baseline) against the loot oracle, per class,
    on the built-in fixture (seeds 7001.., 8 episodes each). Runs only where the simulator has the world (a probe); a list world is
    refused by the simulator with the fields its extract lacks, and that refusal is an open decision. The IAUS loot policy is the
    post-T2 batch: until it exists the stage reports baseline vs oracle. Divergences: (c) the oracle takes what no row takes or makes a
    sell run (a behaviour today's rules do not have), (a) the rest (a row's or an INI value: the potion floor, the keep targets)."""
    st = {"policy": "baseline (today's rules); IAUS loot scorer not built", "cells": {}, "clusters": {}, "lists": {}}
    probe = subprocess.run([A.bin, "--logistics", "1", "--seeds", "1", "--classes", "melee"], capture_output=True, text=True)
    if "LOGI-CHECKS" not in probe.stdout:
        st["verdict"] = "SKIPPED: this simulator has no logistics world (no LOGI-CHECKS line from --logistics)"
        log("logistics: " + st["verdict"])
        SUMMARY["stages"]["logistics"] = st
        return
    order = scope() or CELLS
    seeds = ",".join(str(x) for x in range(7001, 7001 + CFG["logi_seeds"]))
    def one(c):
        cmd = [A.bin, "--logistics", str(CFG["logi_eps"]), "--seeds", seeds, "--classes", c]
        def chk(t):
            if "LOGI-CHECKS" not in t or "=> PASS" not in t.split("LOGI-CHECKS:")[-1] or not re.search(r"^LOGIGAP %s " % re.escape(c), t, re.M):
                raise RuntimeError("logistics run of %s: the model's checks did not pass or no LOGIGAP line:\n%s" % (c, t[-600:]))
        return c, text_run("logi:" + json.dumps(cmd[1:]), cmd, "builtin", check=chk)
    for c, txt in POOL.map(one, order):
        m = re.search(r"^LOGIGAP %s ([-0-9.]+) ([-0-9.]+) (\d+) goldJ=([0-9.]+) potionJ_gap=([-0-9.]+) gold_equivalent_gap=([-0-9.]+) baseline_takes=(\d+) oracle_takes=(\d+) sell_runs=(\d+)" % re.escape(c), txt, re.M)
        cell = dict(zip(("gap", "se", "episodes", "goldJ", "potionJ_gap", "gold_equivalent_gap", "baseline_takes", "oracle_takes", "sell_runs"), [float(x) for x in m.groups()]))
        st["cells"][c] = cell
        rows = []
        for k, n, du in re.findall(r"^LOGIDIS %s (\S+) (\d+) ([-0-9.]+)$" % re.escape(c), txt, re.M):
            kind, b, o = k.split("|")
            cl, why = logi_class(kind, b, o)
            rows.append({"cluster": k, "class": cl, "n": int(n), "dU_per_episode": round(float(du) / cell["episodes"], 3), "why": why})
        rows.sort(key=lambda r: -r["dU_per_episode"])
        st["clusters"][c] = rows
        log("logistics %s: gap %+.2f (se %.2f, %d episodes), potions %+.2f J, gold-equivalent %+.0f g" % (c, cell["gap"], cell["se"], cell["episodes"], cell["potionJ_gap"], cell["gold_equivalent_gap"]))
    for w in [w for w in A.worlds if w != "builtin"]:
        r = subprocess.run([A.bin, "--logistics", "1", "--list", CACHE_DIR + w + ".sim.json"], capture_output=True, text=True)
        st["lists"][w] = (r.stderr.strip() or r.stdout[-200:])[:400]
    if st["lists"]:
        open_decision("logistics on the lists", "the simulator refuses a list logistics world: %s. The list extractor (tools/list_balance) would need containers, gold / MISC / gems / ingredients, vendor chests and gold, armor and potion weights" % next(iter(st["lists"].values())))
    open_decision("logistics VALUE function", "J_log = the potion worth of the bag (combat world, PotionWorth) + kGoldJ x gold-equivalent, kGoldJ = J per gold where gold buys the most J (the first potion); gold is valued LINEARLY, so the gap is mostly gold (%s). Whether gold beyond the potion quota is worth J, and what a gear upgrade is worth in J (a combat-world measurement), is a value choice: marth or the coordinator decides (Q-potion-value analogue)"
                  % ", ".join("%s %+.0f g" % (c, v["gold_equivalent_gap"]) for c, v in list(st["cells"].items())[:3]))
    SUMMARY["stages"]["logistics"] = st


# ── tune 2.11.1: the worth-the-gold table and the IAUS loot table ─────────────────────────────────────────────────────────────────
# One knob = one native member (Tune.h kWorthKnobs / kLootKnobs*): worth.<f> = iaus/ConsumableWorth.h WorthTable::<f>, loot.<f> =
# iaus/LootNeed.h LootTuning::<f>. Values: x0.5 / x0.75 / x1.33 / x2 of the placeholder inside the member's valid range (a 0..1 share stays
# inside 0..1, a count stays >= its smallest meaningful value). Both tables are SHARED (one per game), so every value is judged like a
# global on the pooled 15 cells (`beta1`) with the T1 gates.
WORTH_KNOBS = [("worth.ratioCap", 2.0, [1.0, 1.5, 3.0, 4.0]), ("worth.pressureFull", 1.0, [0.5, 0.75, 1.33, 2.0]),
               ("worth.dangerSpan", 0.34, [0.17, 0.255, 0.45, 0.68]), ("worth.hurtWeight", 1.0, [0.5, 0.75, 1.33, 2.0]),
               ("worth.magickaWinLine", 0.15, [0.075, 0.1125, 0.2, 0.3]), ("worth.staminaWinLine", 0.10, [0.05, 0.075, 0.133, 0.2]),
               ("worth.winNeed", 1.0, [0.5, 0.75]), ("worth.ttlFloorSec", 4.0, [2.0, 3.0, 5.33, 8.0]), ("worth.ttlSpanSec", 6.0, [3.0, 4.5, 8.0, 12.0]),
               ("worth.wealthFrac", 0.25, [0.125, 0.1875, 0.333, 0.5]), ("worth.wealthFloor", 100.0, [50.0, 75.0, 133.0, 200.0]),
               ("worth.costWeight", 1.0, [0.5, 0.75, 1.33, 2.0]), ("worth.scarcityRate", 0.5, [0.25, 0.375, 0.667, 1.0]),
               ("worth.restoreCostScale", 0.25, [0.125, 0.1875, 0.333, 0.5]), ("worth.restoreUseWeight", 0.5, [0.25, 0.375, 0.667, 1.0]),
               ("worth.floor", 0.2, [0.1, 0.15, 0.267, 0.4]), ("worth.foodMinGain", 0.05, [0.025, 0.0375, 0.0667, 0.1])]
LOOT_KNOBS = [("loot.takeThreshold", 0.5, [0.25, 0.375, 0.667, 0.8]), ("loot.needBase", 0.55, [0.275, 0.41, 0.73, 0.9]),
              ("loot.needSlope", 0.45, [0.225, 0.34, 0.6, 0.9]), ("loot.roomKnee", 0.25, [0.125, 0.1875, 0.333, 0.5]),
              ("loot.gold", 1.0, [0.5, 0.75]), ("loot.equipment", 0.8, [0.4, 0.6, 1.0]), ("loot.valuables", 0.75, [0.375, 0.56, 1.0]),
              ("loot.jewelry", 0.75, [0.375, 0.56, 1.0]), ("loot.buffScale", 0.95, [0.475, 0.71, 1.0]), ("loot.foodScale", 0.9, [0.45, 0.675, 1.0]),
              ("loot.healthTarget", 6, [3, 4, 8, 12]), ("loot.staminaTarget", 4, [2, 3, 5, 8]), ("loot.staminaTargetNoWeapon", 2, [0, 1, 3, 4]),
              ("loot.magickaTarget", 5, [2, 3, 7, 10]), ("loot.resistTarget", 2, [1, 3, 4]), ("loot.fortifyTarget", 1, [2, 3])]
# NOT searched, each an open decision with its reason: the lab prices their items at 0 because MFO has no road that uses them, so any
# value found would only say "carry less weight" (soul gems: no recharge road, ClickUp 1dfcuabd356; poisons: no apply road; invisibility,
# regen and lockpicks: not in the fixture; the sale scale: inert while consumableSellRoad is false, no ALCH sell road).
LOOT_NOT_SEARCHED = {"loot.gemChargeTrigger": "no weapon-recharge road (ClickUp 1dfcuabd356): a carried gem is worth 0 in the lab",
                     "loot.soulGemTarget": "no weapon-recharge road: a carried gem is worth 0 in the lab",
                     "loot.poisonTarget": "no poison-apply road: a carried poison is worth 0 in the lab",
                     "loot.poisonPowerRef": "no poison-apply road", "loot.poisonWeakScale": "no poison-apply road",
                     "loot.invisTarget": "no invisibility potion in the fixture (a sneak road the logistics world does not model)",
                     "loot.regenTarget": "no regen potion in the fixture", "loot.lockpickTarget": "no lockpick in the fixture",
                     "loot.saleGoldPerWeight": "inert while LootTuning::consumableSellRoad is false (no ALCH sell road)",
                     "loot.saleWeight": "inert while LootTuning::consumableSellRoad is false (no ALCH sell road)"}


def final_sets():
    """The accepted worth-the-gold values (the worth stage), as one --set string, or None."""
    kept = SUMMARY["stages"].get("worth", {}).get("kept", [])
    return ",".join(kept) or None


def shared_search(stage, knobs, pooled, train, judge_one, base_sets=None):
    """Coordinate descent over SHARED knobs (one table per game): liveness on the probe seeds, then most sensitive first, each sweep raced
    (race) on the pooled cells, the best value through judge_one (holdout, gates, confirmatory), an accepted value carried into the next
    sweep; a second pass only after something changed. Rows like the classes stage's."""
    curves = final_curves() if stage == "worth" else []
    cfg = "g3"
    probe = range(train[0], train[0] + CFG["probe_seeds"])
    kn = knobs[:1] if CFG["force"] else knobs
    join = lambda xs: ",".join(([base_sets] if base_sets else []) + xs) or None
    res = jmany([req("builtin", pooled, cfg, curves, probe, join([]))] + [req("builtin", pooled, cfg, curves, probe, join(["%s=%s" % (n, v)])) for n, _, vals in kn for v in (vals[0], vals[-1])])
    live, dead = [], []
    for i, (n, sh, vals) in enumerate(kn):
        best = 0.0
        for r in (res[1 + 2 * i], res[2 + 2 * i]):
            d = [y[0] - x[0] for x, y in zip(res[0], r)]
            if any(abs(x) > 1e-9 for x in d):
                best = max(best, abs(sum(d) / len(d)), 1e-6)
        (live if best > 0.0 else dead).append((best, n, sh, vals))
    live.sort(key=lambda x: -x[0])
    log("%s: %d knobs, %d live, %d dead (%s)" % (stage, len(kn), len(live), len(dead), " ".join(n for _, n, _, _ in dead)))
    kept, rows, done = [], [], set()
    for ps in range(CFG["passes"]):
        moved = False
        for sens, n, sh, vals in live:
            if n in done:
                continue
            b0, rr = race(req("builtin", pooled, cfg, curves, train, join(kept)), [req("builtin", pooled, cfg, curves, train, join(kept + ["%s=%s" % (n, v)])) for v in vals], train)
            cands = sorted([(paired(b0, r) + (v,)) for (r, _), v in zip(rr, vals) if r is not None], key=lambda x: -x[0]) or [(-INF, 0.0, vals[0])]
            d, se, v = cands[0]
            spec = "%s=%s" % (n, v)
            row = {"knob": n, "shipped": sh, "best": v, "probe": round(sens, 3), "pass": ps + 1, "train": [round(d, 3), round(se, 3)] if d > -INF else None,
                   "sweep": [[c[2], round(c[0], 3), round(c[1], 3)] for c in sorted(cands, key=lambda x: x[2]) if c[0] > -INF],
                   "raced_out": [(vv, nn) for (r, nn), vv in zip(rr, vals) if r is None], "accepted": False, "spec": spec}
            passed = d > 2.5 * se and d >= 0.15
            if passed or (CFG["force"] and not rows):
                j = judge_one(join(kept), join(kept + [spec]), not passed)
                row["steps"], row["why"] = j["steps"], j["why"]
                row["accepted"] = passed and j["accepted"]
            else:
                row["why"] = ("train: does not beat noise" if not d > 2.5 * se else "train: beats noise but under 0.15 J") if d > -INF else "train: every value raced out"
            if row["accepted"]:
                kept.append(spec)
                done.add(n)
                moved = True
            rows.append(row)
            log("%s pass %d  %-26s %-8g -> %-8g train %s  %s" % (stage, ps + 1, n, sh, v, row["train"], row["why"][:150]))
            SUMMARY["stages"][stage] = {"kept": kept, "rows": rows, "live": [[n2, round(x, 3)] for x, n2, _, _ in live], "dead": [n2 for _, n2, _, _ in dead], "complete": False}
            save_summary()
        if not moved:
            break
    SUMMARY["stages"][stage] = {"kept": kept, "rows": rows, "live": [[n2, round(x, 3)] for x, n2, _, _ in live], "dead": [n2 for _, n2, _, _ in dead], "complete": True}
    save_summary()
    return kept, rows


def stage_worth():
    """THE WORTH-THE-GOLD TABLE (iaus/ConsumableWorth.h WorthTable, feat/consumables). Needs the consumables world (--sim-flags with
    "--consumables on": gold, item values, buff potions and food in the kits, Setup.h FillConsumables), else the cost side is inert and
    the stage refuses. Every value is shared, so it is judged as a global by judge("beta1"): holdout on the pooled 15 cells, many-seed
    gates on all 15 cells at all three configs, fresh-seed cross-world over every world, the Knight floor."""
    if "--consumables on" not in (A.sim_flags or ""):
        raise SystemExit("the worth stage needs --sim-flags \"--consumables on\" (without the consumables kit the worth rule's cost side is inert)")
    curves = final_curves()
    def judge_one(bs, ns, forced):
        return judge("beta1", curves, None, forced=forced, sets=ns, base_sets=bs)
    kept, rows = shared_search("worth", WORTH_KNOBS, "beta1", range(1, CFG["train"] + 1), judge_one)
    for r in rows:
        if r["accepted"] and (r["best"] in (min(dict((k[0], k[2]) for k in WORTH_KNOBS)[r["knob"]]), max(dict((k[0], k[2]) for k in WORTH_KNOBS)[r["knob"]]))):
            open_decision("worth %s accepted at the edge of the range tried (%s)" % (r["knob"], r["best"]), "the best value may lie past it (runbook 7); written only after a look")


def loot_judge(bs, ns, forced):
    """The loot table's RULE (the combat RULE carried over, 33.3, with what the logistics world can and cannot measure): holdout on seeds
    8001.. (d > 1.5 se, pooled 15 cells); gates on the 128 train + holdout seeds per cell (no cell worse at the 15-at-once bar, none out of
    band); confirmatory on FRESH seeds 9001.. pooled (d > 2 se): the cross-WORLD step cannot run, the simulator refuses a list logistics
    world (34.6: the extracts hold no containers); no Knight floor (a loot value does not change a fight in the lab)."""
    row = {"steps": {}, "accepted": False}
    fails = []
    pooled = "logi:" + ",".join(CELLS)
    hold = range(8001, 8001 + CFG["hold"])
    b, v = jmany([req("builtin", pooled, "g3", [], hold, bs), req("builtin", pooled, "g3", [], hold, ns)])
    hd, hse = paired(b, v)
    row["steps"]["holdout"] = [round(hd, 3), round(hse, 3)]
    if not hd > 1.5 * hse:
        fails.append("holdout: not confirmed")
    if not fails or forced:
        seeds = list(range(7001, 7001 + CFG["train"])) + list(hold)
        res = jmany([req("builtin", "logi:" + c, "g3", [], seeds, x) for c in CELLS for x in (bs, ns)])
        gate = {}
        for i, c in enumerate(CELLS):
            d, se = paired(res[2 * i], res[2 * i + 1])
            gate[c] = [round(d, 3), round(se, 3)]
            if worse(d, se, True) or out_of_band(c, d, se):
                fails.append("gate: %s gets worse (%+.2f, se %.2f)" % (c, d, se))
        row["steps"]["gates"] = gate
    if not fails or forced:
        fresh = range(9001, 9001 + CFG["confirm"])
        b, v = jmany([req("builtin", pooled, "g3", [], fresh, bs), req("builtin", pooled, "g3", [], fresh, ns)])
        fd, fse = paired(b, v)
        row["steps"]["fresh"] = [round(fd, 3), round(fse, 3)]
        if not fd > 2.0 * fse:
            fails.append("fresh seeds: not confirmed (%+.2f, se %.2f)" % (fd, fse))
    row["accepted"] = not fails
    row["why"] = "ACCEPTED: holdout, per-cell gates, fresh seeds (no list world exists for logistics)" if not fails else "; ".join(fails)
    return row


def stage_loot():
    """THE IAUS LOOT TABLE (iaus/LootNeed.h LootTuning) in the logistics world's IAUS arm (Logistics.h, the native scorer compiled in),
    with the stops' buff potions, food, poisons, soul gems and bolts (--logi-consumables). What a carried buff potion or food is worth
    there comes from the COMBAT world: its uses per fight per class (--consumables on --print-consume, with the final table and the
    accepted worth values), written to <out>/uses.txt first."""
    curves = final_curves()
    def one(c):
        cmd = [A.bin, "--no-json", "--threads", "1", "--classes", c, "--fights", "48", "--print-consume", "--seeds", ",".join(str(x) for x in range(1, 1 + max(2, CFG["calib_seeds"])))] + world_args("builtin")
        for cv in curves:
            cmd += ["--curve", cv]
        if final_sets():
            cmd += ["--set", final_sets()]
        def chk(t):
            if "CONSUSE" not in t:
                raise RuntimeError("no CONSUSE line for %s (is --consumables on in --sim-flags?):\n%s" % (c, t[-400:]))
        t = text_run("uses:" + json.dumps(cmd[1:]), cmd, "builtin", check=chk)
        f, u = 0, None
        for m in re.finditer(r"^CONSUSE \d+ (\d+)((?: \d+)+)$", t, re.M):
            f += int(m.group(1))
            xs = [int(x) for x in m.group(2).split()]
            u = xs if u is None else [a + b for a, b in zip(u, xs)]
        return c, [x / max(1, f) for x in u]
    lines = ["%s %s" % (c, " ".join("%.6f" % x for x in u)) for c, u in POOL.map(one, CELLS)]
    open(uses_path(), "w").write("\n".join(lines) + "\n")
    st = {"uses": lines}
    SUMMARY["stages"]["loot_uses"] = st
    kept, rows = shared_search("loot", LOOT_KNOBS, "logi:" + ",".join(CELLS), range(7001, 7001 + CFG["train"]), loot_judge)
    for n, why in LOOT_NOT_SEARCHED.items():
        open_decision("loot %s not searched" % n, why + "; the placeholder stays")


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
            # tune 2.11.1: a RUNTIME knob of the run.sh build (--set const.<name>, run.sh RUNTIME CONSTANTS), no rebuild; proven equal to the
            # const_probe.sh variant binaries (iaus-sim-v2.md 36). const_probe.sh stays for a constant run.sh does not expose.
            res = jmany([req("builtin", c, g, curves, seeds, st) for c in CELLS for g in CONFIGS for st in (None, "const.%s=%s" % (name, v))])
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
            out["%s/%s" % (cls, w)] = fl = knight_floor(w, cls, curves, final_sets())
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
        if final_sets():
            cmd += ["--set", final_sets()]
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
                SUMMARY.setdefault("withheld", []).append(spec)
                open_decision("accepted value %s loses with the re-measured potion worth" % spec, "%+.2f (se %.2f) on fresh seeds: do not write it; report" % (d, se), kind="withheld", verdict="rejected")
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
    for stg, hdr, native in (("worth", "The worth-the-gold table (iaus/ConsumableWorth.h WorthTable)", "ConsumableWorth.h"), ("loot", "The IAUS loot table (iaus/LootNeed.h LootTuning)", "LootNeed.h")):
        if stg not in s:
            continue
        r = s[stg]
        L += ["## " + hdr + ("" if r.get("complete") else " (running)"), "", "Live %d, dead %d (%s). Accepted: %s" % (len(r["live"]), len(r["dead"]), ", ".join(r["dead"]) or "none", " ".join("`%s`" % k for k in r["kept"]) or "none"), "",
              "| Knob | Placeholder | Best | Train d (se) | Raced out | Verdict |", "|---|---|---|---|---|---|"]
        for row in r["rows"]:
            tr = row["train"]
            L.append("| %s | %g | %g | %s | %s | %s |" % (row["knob"], row["shipped"], row["best"], "%+.3f (%.3f)" % tuple(tr) if tr else "n/a", " ".join("%g@%d" % (v, n) for v, n in row.get("raced_out", [])) or "", row["why"][:160]))
        L += ["", "Lines for native/iaus/%s (one per accepted value, the evidence beside it):" % native, "", "```cpp"]
        for row in r["rows"]:
            if row["accepted"]:
                st = row["steps"]
                ev = "holdout %+.3f (se %.3f)" % tuple(st["holdout"])
                if "cross_world" in st:
                    ev += ", fresh-seed mean over %s %+.3f (se %.3f), as shipped %+.3f" % ("/".join(A.worlds), st["cross_world"]["mean/g3"][0], st["cross_world"]["mean/g3"][1], st["cross_world"]["mean/shipped"][0])
                if "fresh" in st:
                    ev += ", fresh seeds %+.3f (se %.3f)" % tuple(st["fresh"])
                L.append("%s = %s;   // SEARCHED (tune 2.11.1): placeholder %g; train d %+.3f (se %.3f), %s" % (row["knob"].split(".", 1)[1], row["best"], row["shipped"], row["train"][0], row["train"][1], ev))
        L += ["```", ""]
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
    if "gap" in s and s["gap"].get("cells"):
        g = s["gap"]
        L += ["## Oracle gap per class (subject-only class oracle, seeds %d.., gap = oracle U - IAUS U)" % GAP_SEEDS0, "",
              "| Class | World | Gap final (se) | fights | Shipped | Tuning change (se) | All followers (built-in) | After (a) search | a / b / c / d dU/f |", "|---|---|---|---|---|---|---|---|---|"]
        for k, v in g["cells"].items():
            c = k.split("/")[0]
            bc = g["clusters"].get(c, {}).get("by_class", {})
            L.append("| %s | %s | %+.2f (%.2f) | %d | %+.2f | %+.2f (%.2f) | %s | %s | %s |" % (c, k.split("/")[1], v["gap"], v["se"], v["fights"], v["shipped_gap"], v["tuning_change"][0], v["tuning_change"][1],
                     "%+.2f (%.2f)" % tuple(v["all_followers_gap"]) if "all_followers_gap" in v else "", "%+.2f (%.2f)" % tuple(v["after_search"]) if "after_search" in v else "",
                     " / ".join("%.2f" % bc.get(x, 0.0) for x in "abcd")))
        L += ["", "### Largest divergences per class (final table, all worlds pooled)", ""]
        for c, cl in g["clusters"].items():
            L.append("- **%s** (%d fights, %d decisions): " % (c, cl["fights"], cl["decisions"]) + "; ".join("(%s) %s -> %s [%s] %.2f/f" % (r["class"], r["iaus"], r["oracle"], r["kind"], r["dU_per_fight"]) for r in cl["rows"][:5]))
        if g.get("kept"):
            L += ["", "Accepted by the (a) search (in final_curves; joint gate re-run: %s): %s" % (g.get("joint_after_search", "?"), " ".join("`%s`" % k for v in g["kept"].values() for k in v))]
            L += ["", "```cpp"] + [cpp_line(k) for v in g["kept"].values() for k in v] + ["```"]
        L += ["", "Ledger rows: `gap-ledger.md` in the run directory.", ""]
    if "logistics" in s:
        g = s["logistics"]
        L += ["## Logistics world (built-in fixture): today's loot rules against the loot oracle", "", g.get("verdict", "Policy: " + g["policy"]), ""]
        if g["cells"]:
            L += ["| Class | Gap J_log (se) | episodes | potions J | gold-equivalent | takes base / oracle | sell runs | largest divergences |", "|---|---|---|---|---|---|---|---|"]
            L += ["| %s | %+.2f (%.2f) | %d | %+.2f | %+.0f g | %d / %d | %d | %s |" % (c, v["gap"], v["se"], v["episodes"], v["potionJ_gap"], v["gold_equivalent_gap"], v["baseline_takes"], v["oracle_takes"], v["sell_runs"],
                  "; ".join("(%s) %s %.2f" % (r["class"], r["cluster"], r["dU_per_episode"]) for r in g["clusters"][c][:3])) for c, v in g["cells"].items()] + [""]
    ad = SUMMARY.get("auto_decisions", [])
    L += ["## AUTOMATIC DECISIONS (--decisions %s; the rule and the numbers are in summary.json auto_decisions)" % A.decisions, "", "| Kind | Verdict | What | Evidence |", "|---|---|---|---|"]
    L += ["| %s | %s | %s | %s |" % (d["kind"], d["verdict"], d["what"], d["evidence"].replace("|", "/")[:400]) for d in ad if d["verdict"] != "note"] or ["| - | - | none | |"]
    L += ["", "Report-only notes (never blockers): %d" % sum(1 for d in ad if d["verdict"] == "note"), ""]
    L += ["## OPEN DECISIONS (the runner reports these and does not decide)", ""] + (["- **%s**: %s" % (d["what"], d["evidence"]) for d in SUMMARY["open_decisions"]] or ["- none"]) + [""]
    return "\n".join(L)


def replay(path):
    """--replay: the open_decisions of an earlier summary.json through the automatic rules, OFFLINE. Only what the summary holds is used:
    a class-card read has the 12 largest changes of his play (stages.classes.<c>.play_changes_natural: metric, start, new), so only band /
    potion metrics among them are judged (the rest moved less than those 12); a decision that needs a measurement the summary lacks is
    `needs-metric`. Prints the table; returns 0."""
    S = json.load(open(path))
    cards, tally, detail = load_cards(), {}, []
    def tl(kind, v):
        tally.setdefault(kind, {"count": 0, "accepted": 0, "rejected": 0, "note": 0, "needs-metric": 0})
        tally[kind]["count"] += 1
        tally[kind][v] += 1
    for d in S.get("open_decisions", []):
        w, cls = d["what"], d.get("cls")
        m = re.match(r"class card read for ([a-z0-9-]+)$", w)
        if m:
            c = m.group(1)
            ch = {k: (o, nw) for k, o, nw in S["stages"].get("classes", {}).get(c, {}).get("play_changes_natural", [])}
            fails, seen = [], 0
            for metric, lo, hi, ln in (cards.get(CARD_ALIAS.get(c, c), {}).get("bands", [])):
                raw = SCHEMA_RAW.get(metric, metric)
                if raw in ch:
                    seen += 1
                    va, vn = band_viol(ch[raw][0], lo, hi), band_viol(ch[raw][1], lo, hi)
                    if vn > va + CARD_SLACK * band_scale(lo, hi):
                        fails.append("%s %.1f -> %.1f (band line %d)" % (metric, ch[raw][0], ch[raw][1], ln))
            for k, cap, strict in (("a.drink_buff", BUFF_SHARE_CAP, True), ("drinkH/f", DRINK_FIGHT_CAP, False)):
                if k in ch:
                    seen += 1
                    o, nw = ch[k]
                    if nw > o + 0.05 * cap and (strict or nw > cap):
                        fails.append("potion %s %.2f -> %.2f" % (k, o, nw))
            v = "rejected" if fails else "accepted" if seen else "needs-metric"
            tl("class card read", v)
            detail.append("class card read %-10s %-12s visible band/potion metrics %d  %s" % (c, v, seen, "; ".join(fails)))
        elif "(gap search)" in w:
            tl("class card read", "needs-metric")
            detail.append("%s: needs the gap search's own play metrics (not in summary.json)" % w)
        elif "consideration weight accepted at the edge" in w:
            tl("weight at edge", "note")
        elif "more than 10" in w:
            tl("many values", "note")
        elif "a value past the edge passes the rule" in w:
            tl("edge past the edge", "needs-metric")
            detail.append("%s: needs card + potion metrics of the widened value%s" % (w, " (a lower family.drink_buff weight can only lower non-restore drinks)" if "drink_buff" in w else ""))
        elif w.startswith("gap search"):
            tl("gap far", "needs-metric")
        elif w.startswith("oracle gap"):
            tl("oracle", "note")
        else:
            tl("other (worth / floor / rating ...)", "note")
    print("%-38s %5s %9s %9s %5s %13s" % ("kind", "count", "accepted", "rejected", "note", "needs-metric"))
    for k, t in tally.items():
        print("%-38s %5d %9d %9d %5d %13d" % (k, t["count"], t["accepted"], t["rejected"], t["note"], t["needs-metric"]))
    print("%-38s %5d" % ("TOTAL", sum(t["count"] for t in tally.values())))
    print("\n".join(detail))
    return 0


def main():
    global A, CFG, POOL
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True, help="the built simulator (SIM_OUT of tools/iaus_sim/run.sh)")
    ap.add_argument("--out", required=True, help="the run's directory: cache.jsonl, progress.log, summary.json, summary.md")
    ap.add_argument("--smoke", action="store_true", help="a few seeds and knobs, every stage exercised; its numbers are NOT results")
    ap.add_argument("--for-class", help="THE NEW-CLASS WORKFLOW (iaus-sim-v2.md 34.4): one class (a new or changed row), on an --out that holds the earlier run: sets --classes to it and --stages to prep,classes,joint,edge,gap,floor,summary")
    ap.add_argument("--classes", help="comma list; default the 15 cells, the furthest from the oracle first")
    ap.add_argument("--worlds", default="builtin,LoreRim,Tuxborn,Vanilla")
    ap.add_argument("--stages", default=DEFAULT_STAGES)
    ap.add_argument("--rejudge", default="warrior:6.32.b=0.15", help="space-separated --curve specs to re-judge under RULE")
    ap.add_argument("--builtin-worth", help="a potion-worth table for the built-in world instead of PotionWorth.h")
    ap.add_argument("--no-worker", action="store_true", help="one simulator process per chunk instead of the persistent `sim --serve` workers")
    ap.add_argument("--no-race", action="store_true", help="measure every candidate of a sweep on every train seed (the pre-2.11.1 driver; the racing proof's reference)")
    ap.add_argument("--sim-flags", default="", help="simulator switches for EVERY run of this driver (batch 3 models, e.g. '--ranged-perks --point-blank'); they go into a wrapper in --out whose text names the binary's sha256, so they are part of every cache key")
    ap.add_argument("--knobs", help="a regular expression: the classes / edge / gap stages search only the knobs whose name matches (e.g. '^equip\\.' for the posture curves); the gates are unchanged")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 16)
    ap.add_argument("--decisions", choices=("auto", "ask"), default="auto", help="auto (default): every former open decision is decided by a rule (class card + potion gates, tuner-auto-decisions.md) or is a report-only note, all in summary.json auto_decisions; ask: the old open decisions, no card / potion gate")
    ap.add_argument("--replay", help="replay the open_decisions of an earlier summary.json through the automatic rules, offline (no simulator), and print the table")
    ap.add_argument("--no-emit-runs", dest="emit_runs", action="store_false", help="do not write one dashboard results file per finished sweep")
    A = ap.parse_args()
    if A.replay:
        return replay(A.replay)
    if A.for_class:
        if A.classes or A.stages != ap.get_default("stages"):
            raise SystemExit("--for-class sets --classes and --stages itself: give neither")
        A.classes, A.stages = A.for_class, FOR_CLASS_STAGES
    A.worlds = A.worlds.split(",")
    A.bin = os.path.abspath(A.bin)
    CFG = SMOKE if A.smoke else FULL
    os.makedirs(A.out, exist_ok=True)
    if A.sim_flags:   # batch 3: one wrapper per (binary, flags); its text carries the binary's sha256, so ident() (sha of A.bin) keys on both
        flags = A.sim_flags.split()
        if any(f in ("--list", "--potion-worth", "--classes", "--seed", "--seeds", "--curve") for f in flags):
            raise SystemExit("--sim-flags takes model switches only, never a world, a class, a seed or a curve")
        body = "#!/bin/sh\n# tune_driver --sim-flags wrapper: binary sha256 %s\nexec %s %s \"$@\"\n" % (sha(A.bin), A.bin, " ".join(flags))
        wrap = os.path.join(A.out, "sim-%s.sh" % hashlib.sha256(body.encode()).hexdigest()[:12])
        if not os.path.exists(wrap) or open(wrap).read() != body:
            with open(wrap, "w") as f:
                f.write(body)
            os.chmod(wrap, 0o755)
        A.bin = wrap
    POOL = cf.ThreadPoolExecutor(max_workers=A.jobs)
    load_cache()
    prev = os.path.join(A.out, "summary.json")
    if os.path.exists(prev):   # a resumed or one-stage run sees what the earlier stages decided (the accepted values feed the later stages)
        try:
            old = json.load(open(prev))
            if old.get("config", {}).get("smoke") == A.smoke:
                SUMMARY["stages"], SUMMARY["open_decisions"] = old.get("stages", {}), [d for d in old.get("open_decisions", []) if d.get("stage")]
                SUMMARY["auto_decisions"], SUMMARY["withheld"] = [d for d in old.get("auto_decisions", []) if d.get("stage")], old.get("withheld", [])
        except ValueError:
            pass   # cut off by a kill: every stage it held replays from the cache
    SUMMARY["config"] = dict(CFG, smoke=A.smoke, worlds=A.worlds, bin=A.bin, sim_flags=A.sim_flags, knobs=A.knobs)
    log("START%s stages=%s worlds=%s cache=%d results" % (" (SMOKE)" if A.smoke else "", A.stages, ",".join(A.worlds), len(CACHE)))
    stages = {"prep": stage_prep, "rejudge": stage_rejudge, "classes": stage_classes, "joint": stage_joint, "edge": stage_edge, "globals": stage_globals, "consts": stage_consts, "floor": stage_floor, "rating": stage_rating, "recalib": stage_recalib, "gap": stage_gap, "logistics": stage_logistics, "worth": stage_worth, "loot": stage_loot}
    order = A.stages.split(",")
    done = []
    i = 0
    while i < len(order):
        name = order[i]
        i += 1
        if name == "summary":
            continue
        SUMMARY["open_decisions"] = [d for d in SUMMARY["open_decisions"] if d.get("stage") != name or (scope() and d.get("cls") and d["cls"] not in scope())]   # a scoped re-run keeps the other classes' decisions
        SUMMARY["auto_decisions"] = [d for d in SUMMARY["auto_decisions"] if d.get("stage") != name or (scope() and d.get("cls") and d["cls"] not in scope())]
        n0, a0 = len(SUMMARY["open_decisions"]), len(SUMMARY["auto_decisions"])
        STAGE[0] = name
        SUMMARY.setdefault("cache_stats", {})[name] = {"seed_runs": 0, "from_cache": 0}   # this invocation's numbers (a resumed run replaces them)
        t0 = time.time()
        stages[name]()
        for d in SUMMARY["open_decisions"][n0:] + SUMMARY["auto_decisions"][a0:]:
            d["stage"] = name
        cs = SUMMARY["cache_stats"][name]
        cs["wall_s"] = round(time.time() - t0, 1)
        save_summary()
        log("STAGE %s done (%.0f s; seed runs %d, %d of them served from the cache = %.1f%%)" % (name, cs["wall_s"], cs["seed_runs"], cs["from_cache"], 100.0 * cs["from_cache"] / max(1, cs["seed_runs"])))
        done.append(name)
        if name == "gap" and SUMMARY["stages"].get("gap", {}).get("search_changed"):
            # G2 (review of 7e883a9a): the (a) search moved the table, so a stage that reads the final table and already ran in this
            # order describes the table from before it: run it again (the default order puts gap first, so this is a custom order only).
            again = [x for x in ("floor", "rating", "recalib") if x in done and x not in order[i:]]
            if again:
                log("gap search moved the table: %s run again after it" % ",".join(again))
                order[i:i] = again
    j = SUMMARY["stages"].get("joint", {}).get("verdict", "")
    SUMMARY["ship"] = {"table": [c for c in final_curves() if c not in SUMMARY.get("withheld", [])] if not j.startswith("FAIL") else [], "withheld": SUMMARY.get("withheld", []),
                       "joint": j or "not run", "note": "nothing a gate rejected is in `table`; a failed joint gate withholds the whole table"}
    save_summary()
    EMIT.shutdown(wait=True)
    log("DONE: %s/summary.md, %d open decision(s), %d automatic decision(s)" % (A.out, len(SUMMARY["open_decisions"]), len(SUMMARY["auto_decisions"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
