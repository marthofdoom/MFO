#!/usr/bin/env python3
"""IAUS sim dashboard: a terminal viewer for the offline simulator's per-run JSON results.

Standard library only (curses, json, argparse, os). See README.md for keys and the schema.
Host-only tool: nothing here is read by, or linked into, the DLL.
"""
import argparse
import curses
import json
import locale
import os
import sys
import time

SCHEMA = 1
DEFAULT_RUNS = os.path.expanduser("~/.cache/mfo-iaus-sim/runs")

KNOWN_METRICS = [
    ("idle_pct", "idle%"), ("idle_cand_pct", "idleC%"), ("hold_pct", "hold%"), ("hold_low_pct", "holdL%"),
    ("switches_per_min", "sw/min"), ("target_switch_per_min", "tsw/min"),
    ("heal_latency_s", "healLat"), ("healed_pct", "healed%"), ("overheal_pct", "ovHeal%"),
    ("double_heal_pct", "dblHeal%"), ("overkill_pct", "ovKill%"), ("mp_at_boss_pct", "mpBoss%"),
    ("melee_share_pct", "melee%"), ("taunt_per_min", "taunt/m"), ("power_per_min", "power/m"),
    ("follower_death_pct", "fDeath%"), ("player_death_pct", "pDeath%"), ("ttk_s", "ttk_s"),
    ("damage_taken_per_foe", "dmgTaken"), ("bad_facts", "badFact"), ("invalid", "invalid"),
    ("decide_us_mean", "dec_us"), ("decide_us_max", "decMax"),
]
LABELS = dict(KNOWN_METRICS)
KNOWN_ORDER = [k for k, _ in KNOWN_METRICS]

# "Better" direction: -1 lower is better, +1 higher is better, 0 neutral. First substring hit wins.
BETTER_RULES = [
    ("healed_pct", +1), ("mp_at_boss", +1),
    ("idle", -1), ("overkill", -1), ("double_heal", -1), ("overheal", -1), ("death", -1),
    ("switch", -1), ("decide_us", -1), ("gap", -1), ("latency", -1), ("ttk", -1),
    ("damage_taken", -1), ("bad_facts", -1), ("invalid", -1),
]


def direction(name):
    for pat, d in BETTER_RULES:
        if pat in name:
            return d
    return 0


def is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def fmt(v):
    if v is None:
        return "-"
    if not is_num(v):
        return str(v)
    if v == int(v) and abs(v) < 1e6:
        return str(int(v))
    a = abs(v)
    return "%.0f" % v if a >= 100 else "%.1f" % v if a >= 10 else "%.2f" % v if a >= 0.1 else "%.3f" % v


def fmt_delta(d):
    if d is None:
        return ""
    if abs(d) < 5e-4:
        return "="
    return ("+" if d > 0 else "-") + fmt(abs(d))


def delta_style(name, d):
    if d is None or abs(d) < 5e-4:
        return None
    dr = direction(name)
    if dr == 0:
        return None
    return "ok" if (d * dr) > 0 else "bad"


# ---------------------------------------------------------------- loading / validation

def validate(doc):
    """Return a list of problem strings (empty = valid). Unknown fields are never a problem."""
    p = []
    if not isinstance(doc, dict):
        return ["top level is not an object"]
    if doc.get("schema") != SCHEMA:
        p.append("schema is %r, expected %d" % (doc.get("schema"), SCHEMA))
    for k in ("run_id", "time", "list"):
        if not isinstance(doc.get(k), str) or not doc.get(k):
            p.append("missing or non-string %r" % k)
    for k in ("classes", "scenarios", "rulings", "gaps"):
        v = doc.get(k)
        if v is None:
            if k == "classes":
                p.append("missing 'classes'")
        elif not isinstance(v, list):
            p.append("%r is not a list" % k)
    for i, c in enumerate(doc.get("classes") or []):
        if not isinstance(c, dict):
            p.append("classes[%d] is not an object" % i)
            continue
        if not c.get("class"):
            p.append("classes[%d] has no 'class'" % i)
        for fld in ("metrics", "action_mix"):
            m = c.get(fld)
            if m is None:
                continue
            if not isinstance(m, dict):
                p.append("classes[%d].%s is not an object" % (i, fld))
            else:
                for mk, mv in m.items():
                    if not is_num(mv):
                        p.append("classes[%d].%s.%s is not a number" % (i, fld, mk))
    for i, s in enumerate(doc.get("scenarios") or []):
        if not isinstance(s, dict) or not s.get("name"):
            p.append("scenarios[%d] missing 'name'" % i)
        elif "gap" in s and not is_num(s["gap"]):
            p.append("scenarios[%d].gap is not a number" % i)
    for i, r in enumerate(doc.get("rulings") or []):
        if not isinstance(r, dict) or not r.get("id"):
            p.append("rulings[%d] missing 'id'" % i)
            continue
        if r.get("op") not in ("<=", ">=", "<", ">", None):
            p.append("rulings[%d].op %r is not one of <= >= < >" % (i, r.get("op")))
        if "pass" in r and not isinstance(r["pass"], bool):
            p.append("rulings[%d].pass is not a bool" % i)
    for i, g in enumerate(doc.get("gaps") or []):
        if not isinstance(g, dict) or not g.get("id"):
            p.append("gaps[%d] missing 'id'" % i)
        elif g.get("status") not in ("open", "closing", "closed", "limit", None):
            p.append("gaps[%d].status %r is not open|closing|closed|limit" % (i, g.get("status")))
    return p


def load_dir(path):
    """Returns (runs sorted oldest->newest, notes). Bad files are skipped with a note."""
    runs, notes = [], []
    try:
        names = sorted(n for n in os.listdir(path) if n.endswith(".json"))
    except OSError as e:
        return [], ["runs dir unreadable: %s" % e]
    for n in names:
        fp = os.path.join(path, n)
        try:
            with open(fp, "r", encoding="utf-8") as f:
                doc = json.load(f)
        except (OSError, ValueError) as e:
            notes.append("skipped %s (%s)" % (n, str(e)[:40]))
            continue
        probs = validate(doc)
        hard = [x for x in probs if x in ("missing or non-string 'run_id'", "missing 'classes'", "top level is not an object")]
        if hard:
            notes.append("skipped %s (%s)" % (n, hard[0]))
            continue
        doc["_file"] = n
        runs.append(doc)
    runs.sort(key=lambda r: (str(r.get("time", "")), str(r.get("run_id", ""))))
    return runs, notes


def dir_signature(path):
    try:
        return tuple(sorted((n, os.stat(os.path.join(path, n)).st_mtime_ns) for n in os.listdir(path) if n.endswith(".json")))
    except OSError:
        return ()


# ---------------------------------------------------------------- data model helpers

def class_rows(run):
    return [c for c in (run.get("classes") or []) if isinstance(c, dict) and c.get("class")]


def wmean(pairs):
    tot = sum(w for _, w in pairs)
    return sum(v * w for v, w in pairs) / tot if tot else None


def agg_classes(run):
    """class -> {tier,line,planned,fights,metrics,action_mix,rows}"""
    out = {}
    for c in class_rows(run):
        e = out.setdefault(c["class"], {"tier": c.get("tier"), "line": c.get("line"), "planned": bool(c.get("planned")),
                                        "fights": 0, "rows": [], "_m": {}, "_a": {}})
        w = c.get("fights") if is_num(c.get("fights")) and c.get("fights") > 0 else 1
        e["fights"] += w if is_num(c.get("fights")) else 0
        e["rows"].append(c)
        for k, v in (c.get("metrics") or {}).items():
            if is_num(v):
                e["_m"].setdefault(k, []).append((v, w))
        for k, v in (c.get("action_mix") or {}).items():
            if is_num(v):
                e["_a"].setdefault(k, []).append((v, w))
    for e in out.values():
        e["metrics"] = {k: wmean(v) for k, v in e.pop("_m").items()}
        e["action_mix"] = {k: wmean(v) for k, v in e.pop("_a").items()}
    return out


def overall_metrics(run):
    acc = {}
    for c in class_rows(run):
        w = c.get("fights") if is_num(c.get("fights")) and c.get("fights") > 0 else 1
        for k, v in (c.get("metrics") or {}).items():
            if is_num(v):
                acc.setdefault(k, []).append((v, w))
    return {k: wmean(v) for k, v in acc.items()}


def ordered_metrics(names):
    names = set(names)
    return [k for k in KNOWN_ORDER if k in names] + sorted(names - set(KNOWN_ORDER))


def rulings_for(run, cls):
    return [r for r in (run.get("rulings") or []) if isinstance(r, dict) and r.get("class") in (cls, "*", None)]


def class_gap(run, cls):
    """Main gap of a class: mean scenario gap, else the summed ledger delta_u. None if neither."""
    g = [s["gap"] for s in (run.get("scenarios") or []) if isinstance(s, dict) and s.get("class") == cls and is_num(s.get("gap"))]
    if g:
        return sum(g) / len(g)
    g = [x["delta_u"] for x in (run.get("gaps") or []) if isinstance(x, dict) and x.get("class") == cls and is_num(x.get("delta_u"))]
    return sum(g) if g else None


def prev_same_list(runs, run):
    idx = runs.index(run) if run in runs else -1
    for r in reversed(runs[:idx]):
        if r.get("list") == run.get("list"):
            return r
    return None


def metric_value(run, metric, cls=None):
    """Value for the trends view. Pseudo metrics: gap, rulings_pass_pct."""
    if metric == "gap":
        if cls:
            return class_gap(run, cls)
        g = [s["gap"] for s in (run.get("scenarios") or []) if isinstance(s, dict) and is_num(s.get("gap"))]
        return sum(g) / len(g) if g else None
    if metric == "rulings_pass_pct":
        rs = rulings_for(run, cls) if cls else [r for r in (run.get("rulings") or []) if isinstance(r, dict)]
        return 100.0 * sum(1 for r in rs if r.get("pass")) / len(rs) if rs else None
    if cls:
        e = agg_classes(run).get(cls)
        return e["metrics"].get(metric) if e else None
    return overall_metrics(run).get(metric)


def sparkline(vals, uni):
    chars = "▁▂▃▄▅▆▇█" if uni else "._-~=+*#"
    nums = [v for v in vals if v is not None]
    if not nums:
        return ""
    lo, hi = min(nums), max(nums)
    out = ""
    for v in vals:
        if v is None:
            out += " "
        else:
            out += chars[0] if hi == lo else chars[int((v - lo) / (hi - lo) * (len(chars) - 1) + 0.5)]
    return out


# ---------------------------------------------------------------- views: build tables
# A table is (headers, rows). A row is dict(cells=[str | (str, style)], sort=[...], style=None, key=...).

def cell_text(c):
    return c[0] if isinstance(c, tuple) else c


def mcell(name, val, base_val, cmp_on):
    s = fmt(val)
    if cmp_on and is_num(val) and is_num(base_val):
        d = val - base_val
        return (s + " " + fmt_delta(d), delta_style(name, d))
    return s


def table_classes(st):
    run, base = st.run, st.base
    if run is None:
        return [], []
    agg = agg_classes(run)
    bagg = agg_classes(base) if base else {}
    metrics = ordered_metrics({k for e in agg.values() for k in e["metrics"]})
    headers = ["Class", "T", "Line", "Rul", "Gap", "Trend"] + [LABELS.get(m, m) for m in metrics]
    rows = []
    for cls, e in agg.items():
        if e["planned"] and not st.show_planned:
            continue
        if st.f_tier is not None and e["tier"] != st.f_tier:
            continue
        if st.f_line is not None and e["line"] != st.f_line:
            continue
        rs = rulings_for(run, cls)
        npass = sum(1 for r in rs if r.get("pass"))
        rul = ("%d/%d" % (npass, len(rs)), "bad" if npass < len(rs) else ("ok" if rs else "dim"))
        g = class_gap(run, cls)
        pg = class_gap(base, cls) if base else None
        if g is not None and pg is not None:
            d = g - pg
            arrow = ("v" if not st.uni else "▼") if d < -5e-4 else ("^" if not st.uni else "▲") if d > 5e-4 else "="
            trend = ("%s %s" % (arrow, fmt_delta(d)), "ok" if d < -5e-4 else "bad" if d > 5e-4 else "dim")
        else:
            trend = ("-", "dim")
        cells = [cls + (" (planned)" if e["planned"] else ""), str(e["tier"] if e["tier"] is not None else "-"), str(e["line"] or "-"),
                 rul, fmt(g), trend]
        sort = [cls.lower(), e["tier"] or 0, str(e["line"]), npass - len(rs), g, g - pg if (g is not None and pg is not None) else None]
        bm = bagg.get(cls, {}).get("metrics", {}) if st.cmp_on else {}
        for m in metrics:
            v = e["metrics"].get(m)
            cells.append(mcell(m, v, bm.get(m), st.cmp_on))
            sort.append(v)
        rows.append({"cells": cells, "sort": sort, "key": cls, "style": None})
    return headers, rows


def table_scenarios(st):
    run, base = st.run, st.base
    if run is None:
        return [], []
    bmap = {(s.get("name"), s.get("class")): s for s in (base.get("scenarios") or []) if isinstance(s, dict)} if base else {}
    headers = ["Scenario", "Class", "IAUS U", "Oracle U", "Gap", "dGap" + (" vs cmp" if st.cmp_on else " vs prev")]
    rows = []
    for s in (run.get("scenarios") or []):
        if not isinstance(s, dict):
            continue
        b = bmap.get((s.get("name"), s.get("class")))
        d = s["gap"] - b["gap"] if b and is_num(s.get("gap")) and is_num(b.get("gap")) else None
        rows.append({"cells": [str(s.get("name")), str(s.get("class", "-")), fmt(s.get("iaus_u")), fmt(s.get("oracle_u")),
                               fmt(s.get("gap")), (fmt_delta(d) if d is not None else "-", delta_style("gap", d))],
                     "sort": [str(s.get("name")).lower(), str(s.get("class")), s.get("iaus_u"), s.get("oracle_u"), s.get("gap"), d],
                     "key": s.get("class"), "style": None})
    return headers, rows


GAP_STYLE = {"open": "bad", "closing": "warn", "closed": "ok", "limit": "info"}


def table_gaps(st):
    run, base = st.run, st.base
    if run is None:
        return [], []
    bmap = {g.get("id"): g for g in (base.get("gaps") or []) if isinstance(g, dict)} if base else {}
    headers = ["Id", "Family", "Class", "dU", "dU chg", "Status", "Description"]
    rows = []
    for g in (run.get("gaps") or []):
        if not isinstance(g, dict):
            continue
        b = bmap.get(g.get("id"))
        d = g["delta_u"] - b["delta_u"] if b and is_num(g.get("delta_u")) and is_num(b.get("delta_u")) else None
        sty = GAP_STYLE.get(g.get("status"))
        rows.append({"cells": [(str(g.get("id")), sty), str(g.get("family", "-")), str(g.get("class", "-")), fmt(g.get("delta_u")),
                               (fmt_delta(d) if d is not None else "-", delta_style("gap", d)), (str(g.get("status", "-")), sty),
                               str(g.get("desc", ""))],
                     "sort": [natkey(g.get("id")), str(g.get("family")), str(g.get("class")), g.get("delta_u"), d, str(g.get("status")), str(g.get("desc"))],
                     "key": g.get("class"), "style": None})
    return headers, rows


def natkey(s):
    s = str(s)
    digits = "".join(ch for ch in s if ch.isdigit())
    return (s.rstrip("0123456789"), int(digits) if digits else 0)


def table_runs(st):
    headers = ["", "Time", "List", "Branch", "Sha", "Label"]
    rows = []
    for r in reversed(st.runs):
        mark = ("*" if r is st.run else "") + ("c" if r is st.base and st.cmp_on else "") + ("^" if r.get("run_id") == st.pinned else "")
        rows.append({"cells": [mark, str(r.get("time", "")), str(r.get("list", "")), str(r.get("branch", "")),
                               str(r.get("sha", ""))[:10], str(r.get("label", ""))],
                     "sort": [mark, r.get("time"), r.get("list"), r.get("branch"), r.get("sha"), r.get("label")], "key": r.get("run_id"),
                     "style": "head" if r is st.run else None})
    return headers, rows


def trend_metric_list(st):
    names = set()
    for r in st.list_runs():
        names |= set(overall_metrics(r))
    return ordered_metrics(names) + ["gap", "rulings_pass_pct"]


def table_trends(st):
    runs = st.list_runs()
    mets = trend_metric_list(st)
    if not mets:
        return [], [], []
    st.t_metric = min(st.t_metric, len(mets) - 1)
    m = mets[st.t_metric]
    vals = [metric_value(r, m, st.t_class) for r in runs]
    headers = ["Time", "Sha", "Label", "Value", "Chg"]
    rows, prev = [], None
    for r, v in zip(runs, vals):
        d = v - prev if is_num(v) and is_num(prev) else None
        rows.append({"cells": [str(r.get("time", "")), str(r.get("sha", ""))[:10], str(r.get("label", "")), fmt(v),
                               (fmt_delta(d), delta_style(m, d))], "sort": [r.get("time"), r.get("sha"), r.get("label"), v, d],
                     "key": r.get("run_id"), "style": "head" if r is st.run else None})
        if v is not None:
            prev = v
    title = ["Metric: %s (m next/M prev)  Class: %s (c cycles)  List: %s  %s" % (
        m, st.t_class or "all classes", st.run.get("list") if st.run else "-",
        {-1: "lower is better", 1: "higher is better", 0: "neutral"}[direction(m)]),
        "Trend: [%s]" % sparkline(vals, st.uni)]
    return title, headers, rows


# ---------------------------------------------------------------- class detail

def detail_lines(st):
    """List of (text, style) lines for the class detail view."""
    run, base = st.run, st.base
    cls = st.detail_class
    L = []
    if run is None or cls is None:
        return [("no class selected", None)]
    agg = agg_classes(run).get(cls)
    if not agg:
        return [("class %s not in this run" % cls, "bad")]
    L.append(("%s  tier %s  %s%s   run %s (%s)" % (cls, agg["tier"], agg["line"], " [planned]" if agg["planned"] else "",
                                                  run.get("run_id"), run.get("list")), "head"))
    L.append(("", None))
    metrics = ordered_metrics({k for c in agg["rows"] for k in (c.get("metrics") or {})})
    bmap = {}
    if base and st.cmp_on:
        for c in class_rows(base):
            if c["class"] == cls:
                bmap[(c.get("bracket"), c.get("rank"))] = c
    L.append(("METRICS per bracket" + ("  (delta vs compare run)" if bmap else ""), "info"))
    labels = [LABELS.get(m, m) for m in metrics]
    cw = []
    grid = []
    for c in agg["rows"]:
        row = [("%s %s" % (c.get("bracket", "-"), c.get("rank", "")), None)]
        b = bmap.get((c.get("bracket"), c.get("rank")))
        for m in metrics:
            row.append(mcell(m, (c.get("metrics") or {}).get(m), (b.get("metrics") or {}).get(m) if b else None, bool(b)))
        grid.append(row)
    hdr = ["Bracket"] + labels
    for j in range(len(hdr)):
        cw.append(max(len(hdr[j]), max([len(cell_text(r[j])) for r in grid] or [0])))
    L.append(("  ".join(h.ljust(cw[j]) for j, h in enumerate(hdr)), "dim"))
    for r in grid:
        # a row line carries one style: use the worst delta colour only when every coloured cell agrees; keep plain otherwise
        L.append(("  ".join(cell_text(c).ljust(cw[j]) for j, c in enumerate(r)), None))
    L.append(("  ".join(("MEAN" if j == 0 else mcell(metrics[j - 1], agg["metrics"].get(metrics[j - 1]), None, False)).ljust(cw[j])
                        for j in range(len(hdr))), "info"))
    L.append(("", None))
    L.append(("ACTION MIX per bracket", "info"))
    fams = sorted({k for c in agg["rows"] for k in (c.get("action_mix") or {})})
    fills = "█▓▒░▚▞▤▥" if st.uni else "#=+-:.~*"
    L.append(("legend: " + "  ".join("%s=%s" % (fills[i % len(fills)], f) for i, f in enumerate(fams)), "dim"))
    width = 30
    for c in agg["rows"]:
        mix = c.get("action_mix") or {}
        tot = sum(v for v in mix.values() if is_num(v) and v > 0) or 1
        bar, acc = "", 0.0
        for i, f in enumerate(fams):
            v = mix.get(f, 0)
            acc += (v if is_num(v) and v > 0 else 0) / tot * width
            bar += fills[i % len(fills)] * max(0, int(round(acc)) - len(bar))
        bar = bar[:width].ljust(width)
        L.append(("%-16s |%s| %s" % ("%s %s" % (c.get("bracket", "-"), c.get("rank", "")), bar,
                                      " ".join("%s %s" % (f, fmt(mix[f])) for f in fams if f in mix)), None))
    L.append(("", None))
    rs = rulings_for(run, cls)
    L.append(("RULINGS %d/%d pass" % (sum(1 for r in rs if r.get("pass")), len(rs)), "info"))
    for r in rs:
        ok = bool(r.get("pass"))
        L.append(("  %-4s %s  %s %s %s %s (value %s)  %s" % (r.get("id"), "PASS" if ok else "FAIL", r.get("metric", ""), r.get("op", ""),
                                                          fmt(r.get("target")), "", fmt(r.get("value")), r.get("text", "")), "ok" if ok else "bad"))
    L.append(("", None))
    scs = [s for s in (run.get("scenarios") or []) if isinstance(s, dict) and s.get("class") == cls]
    L.append(("SCENARIOS (%d)" % len(scs), "info"))
    for s in sorted(scs, key=lambda s: -(s.get("gap") or 0)):
        L.append(("  %-28s IAUS %s  oracle %s  gap %s" % (s.get("name"), fmt(s.get("iaus_u")), fmt(s.get("oracle_u")), fmt(s.get("gap"))), None))
    L.append(("", None))
    gs = [g for g in (run.get("gaps") or []) if isinstance(g, dict) and g.get("class") in (cls, "*")]
    L.append(("GAPS (%d)" % len(gs), "info"))
    for g in gs:
        L.append(("  %-4s %-8s dU %-6s %-8s %s" % (g.get("id"), g.get("family", ""), fmt(g.get("delta_u")), g.get("status", ""), g.get("desc", "")),
                  GAP_STYLE.get(g.get("status"))))
    return L


# ---------------------------------------------------------------- state

VIEWS = ["Classes", "Detail", "Scenarios", "Gaps", "Trends", "Runs"]
KEY_VIEW = {"1": 0, "2": 1, "3": 2, "4": 3, "5": 4, "6": 5}


class State:
    def __init__(self, path, uni):
        self.path, self.uni = path, uni
        self.runs, self.notes = [], []
        self.sig = None
        self.pinned = None
        self.compare = None  # run_id
        self.cmp_on = False
        self.view = 0
        self.prev_view = 0
        self.sel = {}
        self.scroll = {}
        self.hoff = 0
        self.filter = {}
        self.sortcol = {}
        self.sortrev = {}
        self.show_planned = True
        self.f_tier = None
        self.f_line = None
        self.f_list = None
        self.detail_class = None
        self.t_metric = 0
        self.t_class = None
        self.status = ""
        self.refresh(force=True)

    def refresh(self, force=False):
        sig = dir_signature(self.path)
        if sig == self.sig and not force:
            return
        self.sig = sig
        self.runs, self.notes = load_dir(self.path)
        self.status = "; ".join(self.notes[:2]) if self.notes else ""

    def pool(self):
        return [r for r in self.runs if self.f_list is None or r.get("list") == self.f_list]

    @property
    def run(self):
        pool = self.pool() or self.runs
        if self.pinned:
            for r in self.runs:
                if r.get("run_id") == self.pinned:
                    return r
        return pool[-1] if pool else None

    @property
    def base(self):
        run = self.run
        if run is None:
            return None
        if self.cmp_on and self.compare:
            for r in self.runs:
                if r.get("run_id") == self.compare:
                    return r
        return prev_same_list(self.runs, run)

    def list_runs(self):
        run = self.run
        return [r for r in self.runs if run and r.get("list") == run.get("list")]

    def lists(self):
        return sorted({str(r.get("list")) for r in self.runs})


def build_table(st):
    """Returns (title_lines, headers, rows) for the current table view."""
    v = VIEWS[st.view]
    title = []
    if v == "Classes":
        h, r = table_classes(st)
    elif v == "Scenarios":
        h, r = table_scenarios(st)
        r = None if not h else r
    elif v == "Gaps":
        h, r = table_gaps(st)
    elif v == "Runs":
        h, r = table_runs(st)
    else:
        title, h, r = table_trends(st) if trend_metric_list(st) else ([], [], [])
    r = r or []
    flt = st.filter.get(v, "").lower()
    if flt:
        r = [x for x in r if flt in " ".join(cell_text(c) for c in x["cells"]).lower()]
    sc = st.sortcol.get(v)
    if sc is None and v == "Scenarios":
        sc, st.sortrev[v] = 4, True
    if sc is not None and h:
        sc = min(sc, len(h) - 1)
        have = [x for x in r if x["sort"][sc] is not None]
        miss = [x for x in r if x["sort"][sc] is None]
        have.sort(key=lambda x: x["sort"][sc], reverse=bool(st.sortrev.get(v)))
        r = have + miss
    return title, h, r


# ---------------------------------------------------------------- curses

COLORS = {}


def init_colors():
    COLORS.clear()
    try:
        curses.start_color()
        curses.use_default_colors()
        pairs = {"ok": curses.COLOR_GREEN, "bad": curses.COLOR_RED, "warn": curses.COLOR_YELLOW, "info": curses.COLOR_CYAN, "dim": curses.COLOR_BLUE}
        for i, (k, c) in enumerate(pairs.items(), 1):
            curses.init_pair(i, c, -1)
            COLORS[k] = curses.color_pair(i)
    except curses.error:
        pass
    COLORS["head"] = curses.A_BOLD
    COLORS["rev"] = curses.A_REVERSE
    COLORS.setdefault("dim", curses.A_DIM)


def put(win, y, x, s, attr=0):
    h, w = win.getmaxyx()
    if y < 0 or y >= h or x >= w:
        return
    s = s[: max(0, w - x - (1 if y == h - 1 else 0))]
    try:
        win.addstr(y, x, s, attr)
    except curses.error:
        pass


def draw_table(win, st, y0, title, headers, rows, key):
    h, w = win.getmaxyx()
    y = y0
    for t in title:
        put(win, y, 0, t, COLORS.get("info", 0))
        y += 1
    if not headers:
        put(win, y, 0, "(no data)", COLORS.get("dim", 0))
        return
    widths = []
    for j, hd in enumerate(headers):
        cap = 64 if hd == "Description" else 30
        widths.append(min(cap, max([len(hd)] + [len(cell_text(r["cells"][j])) for r in rows])))
    sc = st.sortcol.get(key)
    x = -st.hoff
    for j, hd in enumerate(headers):
        mark = (("v" if st.sortrev.get(key) else "^") if sc == j else "")
        put_clip(win, y, x, (hd + mark).ljust(widths[j]) + " ", curses.A_UNDERLINE | curses.A_BOLD)
        x += widths[j] + 1
    y += 1
    avail = h - y - 1
    sel = max(0, min(st.sel.get(key, 0), len(rows) - 1))
    st.sel[key] = sel
    top = st.scroll.get(key, 0)
    if sel < top:
        top = sel
    if sel >= top + avail:
        top = sel - avail + 1
    st.scroll[key] = top
    for i in range(top, min(len(rows), top + avail)):
        r = rows[i]
        x = -st.hoff
        for j, c in enumerate(r["cells"]):
            txt, sty = (c if isinstance(c, tuple) else (c, r.get("style")))
            attr = COLORS.get(sty, 0) if sty else 0
            if i == sel:
                attr |= curses.A_REVERSE
            put_clip(win, y, x, txt[: widths[j]].ljust(widths[j]) + " ", attr)
            x += widths[j] + 1
        y += 1
    if len(rows) > avail:
        put(win, h - 2, max(0, w - 14), " %d/%d " % (sel + 1, len(rows)), COLORS.get("dim", 0))


def put_clip(win, y, x, s, attr=0):
    """put() that tolerates a negative x (horizontally scrolled off the left edge)."""
    if x < 0:
        s = s[-x:]
        x = 0
    if s:
        put(win, y, x, s, attr)


def draw_detail(win, st):
    h, w = win.getmaxyx()
    lines = detail_lines(st)
    top = st.scroll.get("Detail", 0)
    top = max(0, min(top, max(0, len(lines) - (h - 3))))
    st.scroll["Detail"] = top
    for i, (t, sty) in enumerate(lines[top: top + h - 3]):
        put_clip(win, 2 + i, -st.hoff, t, COLORS.get(sty, 0) if sty else 0)


def draw(win, st):
    win.erase()
    h, w = win.getmaxyx()
    if h < 6 or w < 30:
        put(win, 0, 0, "terminal too small (need 30x6)")
        win.refresh()
        return
    run = st.run
    tabs = ""
    x = 0
    for i, n in enumerate(VIEWS):
        s = " %d:%s " % (i + 1, n)
        put(win, 0, x, s, curses.A_REVERSE if i == st.view else 0)
        x += len(s)
    hdr = ""
    if run:
        hdr = "%s [%s] %s%s" % (run.get("run_id", ""), run.get("list", ""), (run.get("label") or "")[:30],
                                (" vs " + str(st.base.get("run_id"))) if st.base and st.cmp_on else "")
        if st.pinned:
            hdr += " (pinned)"
    put(win, 1, 0, hdr + ("   filter(list)=%s tier=%s line=%s planned=%s" % (st.f_list or "all", st.f_tier or "all", st.f_line or "all", "on" if st.show_planned else "off")
                          if VIEWS[st.view] == "Classes" else ""), COLORS.get("info", 0))
    v = VIEWS[st.view]
    if v == "Detail":
        draw_detail(win, st)
    elif not st.runs and v != "Runs":
        put(win, 3, 0, "No runs found in %s" % st.path, COLORS.get("warn", 0))
    else:
        title, hd, rows = build_table(st)
        draw_table(win, st, 2, title, hd, rows, v)
    flt = st.filter.get(v)
    foot = (st.status or "q quit 1-6/Tab views jk move / filter s sort Enter open Esc back ? help")
    if flt:
        foot = "/%s  %s" % (flt, foot)
    put(win, h - 1, 0, foot, COLORS.get("rev", 0))
    win.refresh()


HELP = ["Keys", "1-6 / Tab    switch view (Classes Detail Scenarios Gaps Trends Runs)", "j k / arrows move; left/right scroll columns",
        "/ filter text   (empty clears)   s cycle sort column   r reverse sort", "Enter open detail / select run   Esc back",
        "Classes: t cycle tier, L cycle line, l cycle list, P toggle planned",
        "Trends: m / M metric next/prev, c cycle class", "Runs: Enter pin as current, u unpin (follow newest),",
        "      c set compare to selected, C clear compare", "x toggle compare deltas (uses compare run, else previous run of the list)", "any key closes help"]


def prompt(win, text):
    h, w = win.getmaxyx()
    buf = ""
    win.timeout(-1)
    while True:
        put(win, h - 1, 0, (text + buf).ljust(w - 1), curses.A_REVERSE)
        win.refresh()
        k = win.getch()
        if k in (10, 13, curses.KEY_ENTER):
            break
        if k == 27:
            buf = None
            break
        if k in (curses.KEY_BACKSPACE, 127, 8):
            buf = buf[:-1]
        elif 32 <= k < 127:
            buf += chr(k)
    win.timeout(500)
    return buf


def cycle(cur, options):
    if cur is None:
        return options[0] if options else None
    i = options.index(cur) + 1 if cur in options else 0
    return options[i] if i < len(options) else None


def handle_key(win, st, k):
    """Returns False to quit."""
    v = VIEWS[st.view]
    h, w = win.getmaxyx()
    ch = chr(k) if 0 <= k < 256 else ""
    if ch == "q":
        return False
    if ch in KEY_VIEW:
        st.view = KEY_VIEW[ch]
        st.hoff = 0
        return True
    if ch == "\t":
        st.view = (st.view + 1) % len(VIEWS)
        st.hoff = 0
        return True
    if ch == "?":
        win.erase()
        for i, t in enumerate(HELP):
            put(win, i, 0, t)
        win.refresh()
        win.timeout(-1)
        win.getch()
        win.timeout(500)
        return True
    if k == 27:
        if v == "Detail":
            st.view = st.prev_view
        elif st.filter.get(v):
            st.filter[v] = ""
        return True
    if ch == "x":
        st.cmp_on = not st.cmp_on
        st.status = "compare deltas %s" % ("on" if st.cmp_on else "off")
        return True
    if k == curses.KEY_RIGHT:
        st.hoff += 8
        return True
    if k == curses.KEY_LEFT:
        st.hoff = max(0, st.hoff - 8)
        return True
    if v == "Detail":
        n = h - 4
        if ch == "j" or k == curses.KEY_DOWN:
            st.scroll["Detail"] = st.scroll.get("Detail", 0) + 1
        elif ch == "k" or k == curses.KEY_UP:
            st.scroll["Detail"] = max(0, st.scroll.get("Detail", 0) - 1)
        elif k == curses.KEY_NPAGE:
            st.scroll["Detail"] = st.scroll.get("Detail", 0) + n
        elif k == curses.KEY_PPAGE:
            st.scroll["Detail"] = max(0, st.scroll.get("Detail", 0) - n)
        return True
    title, hd, rows = build_table(st)
    sel = st.sel.get(v, 0)
    if ch == "j" or k == curses.KEY_DOWN:
        st.sel[v] = min(len(rows) - 1, sel + 1)
    elif ch == "k" or k == curses.KEY_UP:
        st.sel[v] = max(0, sel - 1)
    elif k == curses.KEY_NPAGE:
        st.sel[v] = min(len(rows) - 1, sel + h - 5)
    elif k == curses.KEY_PPAGE:
        st.sel[v] = max(0, sel - (h - 5))
    elif ch == "g":
        st.sel[v] = 0
    elif ch == "G":
        st.sel[v] = max(0, len(rows) - 1)
    elif ch == "/":
        s = prompt(win, "filter: ")
        if s is not None:
            st.filter[v] = s
            st.sel[v] = 0
    elif ch == "s":
        st.sortcol[v] = (st.sortcol.get(v, -1) + 1) % max(1, len(hd))
        st.sortrev[v] = False
    elif ch == "r":
        st.sortrev[v] = not st.sortrev.get(v)
    elif k in (10, 13, curses.KEY_ENTER) and rows:
        key = rows[max(0, min(sel, len(rows) - 1))]["key"]
        if v == "Runs":
            st.pinned = key
            st.status = "pinned %s as current" % key
        elif key is not None and v in ("Classes", "Scenarios", "Gaps"):
            if key != "*":
                st.detail_class = key
                st.prev_view = st.view
                st.view = VIEWS.index("Detail")
                st.scroll["Detail"] = 0
                st.hoff = 0
    elif v == "Classes":
        if ch == "t":
            st.f_tier = cycle(st.f_tier, sorted({c.get("tier") for r in [st.run] if r for c in class_rows(r) if c.get("tier") is not None}))
        elif ch == "L":
            st.f_line = cycle(st.f_line, sorted({str(c.get("line")) for r in [st.run] if r for c in class_rows(r)}))
        elif ch == "l":
            st.f_list = cycle(st.f_list, st.lists())
            st.pinned = None
        elif ch == "P":
            st.show_planned = not st.show_planned
    elif v == "Trends":
        mets = trend_metric_list(st)
        if ch == "m" and mets:
            st.t_metric = (st.t_metric + 1) % len(mets)
        elif ch == "M" and mets:
            st.t_metric = (st.t_metric - 1) % len(mets)
        elif ch == "c":
            st.t_class = cycle(st.t_class, sorted(agg_classes(st.run))) if st.run else None
    elif v == "Runs":
        if ch == "u":
            st.pinned = None
            st.status = "following newest run"
        elif ch in ("c", "C") and rows:
            if ch == "C":
                st.cmp_on, st.compare = False, None
            else:
                st.compare = rows[max(0, min(sel, len(rows) - 1))]["key"]
                st.cmp_on = True
                st.status = "comparing against %s" % st.compare
    return True


def ui(stdscr, st):
    try:
        curses.curs_set(0)
    except curses.error:
        pass
    init_colors()
    stdscr.keypad(True)
    stdscr.timeout(500)
    last = time.time()
    while True:
        if time.time() - last >= 2:
            last = time.time()
            old = len(st.runs)
            st.refresh()
            if len(st.runs) > old and not st.pinned:
                st.status = "new run: %s" % st.runs[-1].get("run_id")
        try:
            draw(stdscr, st)
        except curses.error:
            pass
        k = stdscr.getch()
        if k == -1:
            continue
        if k == curses.KEY_RESIZE:
            continue
        if not handle_key(stdscr, st, k):
            return


# ---------------------------------------------------------------- report / json-check

def find_run(runs, rid):
    for r in runs:
        if r.get("run_id") == rid:
            return r
    return None


def text_table(headers, rows):
    cells = [[cell_text(c) for c in r] for r in rows]
    wd = [max(len(h), *(len(r[j]) for r in cells)) if cells else len(h) for j, h in enumerate(headers)]
    out = ["  ".join(h.ljust(wd[j]) for j, h in enumerate(headers)).rstrip()]
    out += ["  ".join(c.ljust(wd[j]) for j, c in enumerate(r)).rstrip() for r in cells]
    return out


def report(args):
    runs, notes = load_dir(args.runs)
    for n in notes:
        print("note: " + n, file=sys.stderr)
    if not runs:
        print("no runs in %s" % args.runs, file=sys.stderr)
        return 2
    run = find_run(runs, args.run) if args.run else runs[-1]
    if run is None:
        print("run %s not found" % args.run, file=sys.stderr)
        return 2
    cmpr = None
    if args.compare:
        cmpr = find_run(runs, args.compare)
        if cmpr is None:
            print("run %s not found" % args.compare, file=sys.stderr)
            return 2
    st = State.__new__(State)
    st.runs, st.pinned, st.compare, st.cmp_on = runs, run["run_id"], args.compare, bool(cmpr)
    st.f_list = st.f_tier = st.f_line = None
    st.show_planned, st.uni, st.filter, st.sortcol, st.sortrev, st.sel, st.scroll = True, False, {}, {}, {}, {}, {}
    print("RUN %s  list=%s  branch=%s  sha=%s  seed=%s" % (run["run_id"], run.get("list"), run.get("branch"), run.get("sha"), run.get("seed")))
    print("label: %s" % run.get("label", ""))
    if cmpr:
        print("compare: %s (%s)" % (cmpr["run_id"], cmpr.get("label", "")))
    print("\nCLASSES")
    h, rows = table_classes(st)
    print("\n".join(text_table(h, [r["cells"] for r in rows])))
    fails = [r for r in (run.get("rulings") or []) if isinstance(r, dict) and not r.get("pass")]
    print("\nFAILING RULINGS (%d of %d)" % (len(fails), len(run.get("rulings") or [])))
    for r in fails:
        print("  %-4s [%s] %s: %s %s %s, value %s" % (r.get("id"), r.get("class"), r.get("text", ""), r.get("metric"), r.get("op"),
                                                      fmt(r.get("target")), fmt(r.get("value"))))
    print("\nSCENARIO GAPS (largest first)")
    h, rows = table_scenarios(st)
    rows.sort(key=lambda r: -(r["sort"][4] or 0))
    print("\n".join(text_table(h, [r["cells"] for r in rows])))
    print("\nGAP LEDGER")
    h, rows = table_gaps(st)
    print("\n".join(text_table(h, [r["cells"] for r in rows])))
    return 0


def json_check(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError) as e:
        print("%s: cannot parse: %s" % (path, e))
        return 1
    probs = validate(doc)
    if probs:
        for p in probs:
            print("%s: %s" % (path, p))
        return 1
    print("%s: ok (%d class rows, %d scenarios, %d rulings, %d gaps)" % (
        path, len(doc.get("classes") or []), len(doc.get("scenarios") or []), len(doc.get("rulings") or []), len(doc.get("gaps") or [])))
    return 0


def main():
    ap = argparse.ArgumentParser(description="IAUS sim dashboard (terminal)")
    ap.add_argument("--runs", default=DEFAULT_RUNS, help="directory of run JSON files (default %s)" % DEFAULT_RUNS)
    ap.add_argument("--report", action="store_true", help="print a plain-text report and exit")
    ap.add_argument("--run", help="run id for --report (default newest)")
    ap.add_argument("--compare", help="run id to compare against in --report")
    ap.add_argument("--json-check", metavar="FILE", help="validate one run file against schema v1")
    ap.add_argument("--ascii", action="store_true", help="ASCII bars and arrows instead of unicode blocks")
    args = ap.parse_args()
    if args.json_check:
        return json_check(args.json_check)
    if args.report:
        return report(args)
    locale.setlocale(locale.LC_ALL, "")
    uni = not args.ascii and "utf" in (locale.getpreferredencoding(False) or "").lower()
    if not sys.stdout.isatty():
        print("not a terminal; use --report", file=sys.stderr)
        return 2
    st = State(args.runs, uni)
    curses.wrapper(ui, st)
    return 0


if __name__ == "__main__":
    sys.exit(main())
