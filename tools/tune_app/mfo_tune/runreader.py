"""Readers for a tune_driver --out directory: progress.log, phases.txt, summary.json, gap-run.json. Read-only, tolerant of a run that is still
writing (partial / missing / truncated files return empty values, never raise)."""
import json
import os
import re
import time

LINE_RE = re.compile(r"^(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)\s\s(.*)$")
ROW_RE = re.compile(r"^(classes|edge|gap)\s+(\S+)\s+pass\s+(\d+)\s+(\S+)\s+(\S+)\s+->\s+(\S+)\s+train\s+([+-][\d.]+)\s+\(se\s+([\d.]+)\)\s+(.*)$")
STAGE_DONE_RE = re.compile(r"^STAGE (\S+) done(?: \((\d+) s)?")


_JSON_CACHE = {}


def read_json(path):
    """Memoised on (mtime, size): the tabs re-read the same big summary.json on every timer tick."""
    try:
        st = os.stat(path)
        key = (st.st_mtime_ns, st.st_size)
        c = _JSON_CACHE.get(path)
        if c and c[0] == key:
            return c[1]
        with open(path) as f:
            v = json.load(f)
        _JSON_CACHE[path] = (key, v)
        return v
    except (OSError, ValueError):
        return None


def read_text(path, limit=None):
    try:
        with open(path, errors="replace") as f:
            t = f.read()
        return t if limit is None else t[-limit:]
    except OSError:
        return ""


def parse_log(path, year=None):
    """-> list of dicts {t (epoch), raw, msg, kind, cls, knob, old, new, d, se, verdict, accepted}"""
    year = year or time.localtime().tm_year
    out = []
    prev = 0
    for raw in read_text(path).splitlines():
        m = LINE_RE.match(raw)
        if not m:
            if out:                     # a continuation line (rare): attach to the previous message
                out[-1]["msg"] += " " + raw.strip()
            continue
        mo, dd, hh, mi, ss, msg = m.groups()
        try:
            t = time.mktime((year, int(mo), int(dd), int(hh), int(mi), int(ss), 0, 0, -1))
        except (ValueError, OverflowError):
            t = prev
        if t < prev - 86400 * 200:      # year rollover
            t = time.mktime((year + 1, int(mo), int(dd), int(hh), int(mi), int(ss), 0, 0, -1))
        prev = t
        e = {"t": t, "raw": raw, "msg": msg, "kind": "info", "cls": "", "stage": ""}
        r = ROW_RE.match(msg)
        if r:
            st, cls, ps, knob, old, new, d, se, verdict = r.groups()
            e.update(kind="row", stage=st, cls=cls, knob=knob, old=old, new=new, d=float(d), se=float(se), verdict=verdict.strip(),
                     accepted=verdict.strip().startswith("ACCEPTED"), **{"pass": int(ps)})
        elif msg.startswith("STAGE"):
            e["kind"] = "stage"
            sm = STAGE_DONE_RE.match(msg)
            if sm:
                e["stage"], e["secs"] = sm.group(1), int(sm.group(2) or 0)
        elif msg.startswith("START"):
            e["kind"] = "start"
        elif msg.startswith("OPEN DECISION"):
            e["kind"] = "decision"
        else:
            m2 = re.match(r"^(classes|edge|gap|rejudge)\s+(\S+)", msg)
            if m2:
                e["stage"] = m2.group(1)
                e["cls"] = m2.group(2).rstrip(":")
        out.append(e)
    return out


def parse_phases(path):
    """phases.txt lines: 'A rc=0 1234 s' (round.sh convention) plus free text. -> [(name, rc, secs)]"""
    res = []
    for ln in read_text(path).splitlines():
        m = re.match(r"^(\S+) rc=(\d+) (\d+) s", ln)
        if m:
            res.append((m.group(1), int(m.group(2)), int(m.group(3))))
    return res


def stage_timings(entries):
    """STAGE ... done lines -> {stage: secs}: the LONGEST occurrence (a resumed round replays finished work in seconds)."""
    d = {}
    for e in entries:
        if e["kind"] == "stage" and "secs" in e:
            d[e["stage"]] = max(d.get(e["stage"], 0), e["secs"])
    return d


def last_start(entries):
    s = [e for e in entries if e["kind"] == "start"]
    return s[-1] if s else None


def start_stages(entry):
    if not entry:
        return []
    m = re.search(r"stages=(\S+)", entry["msg"])
    return m.group(1).split(",") if m else []


def current_activity(entries):
    """What the run is doing now: (stage, class, pass, last-line age implied by caller)."""
    for e in reversed(entries):
        if e["kind"] in ("row", "info") and e["stage"]:
            return e["stage"], e["cls"], e.get("pass")
        if e["kind"] == "stage":
            continue
    return "", "", None


def class_spans(entries, start_t=0):
    """Per class of the 'classes' stage: [first t, last t] of its rows, in first-seen order (since start_t)."""
    spans = {}
    for e in entries:
        if e["t"] >= start_t and e["stage"] == "classes" and e["cls"]:
            sp = spans.setdefault(e["cls"], [e["t"], e["t"]])
            sp[1] = e["t"]
    return spans


def eta(entries, planned_classes=0, now=None):
    """Rough seconds left. Stages of the START line not yet done cost their longest measured time in this log; the 'classes' stage is
    estimated from the classes already finished in this round: (classes still to do) x (mean span of finished classes longer than a minute),
    minus what the class in progress has already used. -> (seconds or None, explanation)"""
    now = now or time.time()
    start = last_start(entries)
    planned = start_stages(start)
    if not planned:
        return None, "no START line"
    st = start["t"]
    done_now = {e["stage"] for e in entries if e["t"] >= st and e["kind"] == "stage" and "secs" in e}
    hist = stage_timings(entries)
    left = [s for s in planned if s not in done_now and s != "summary"]
    if not left:
        return 0, "all planned stages done"
    total, notes = 0, []
    for s in left:
        if s == "classes":
            sp = class_spans(entries, st)
            order = list(sp)
            fin = [sp[c][1] - sp[c][0] for c in order[:-1] if sp[c][1] - sp[c][0] > 60]
            if fin and planned_classes:
                mean = sum(fin) / len(fin)
                cur = (sp[order[-1]][1] - sp[order[-1]][0]) if order else 0
                todo = max(0, planned_classes - len(order))
                total += todo * mean + max(0.0, mean - cur)
                notes.append("classes: %d left x %.0f min (mean of %d finished)" % (todo, mean / 60, len(fin)))
            else:
                notes.append("classes: no finished class to time yet")
        elif s in hist and hist[s] > 5:
            total += hist[s]
            notes.append("%s %.0f min (earlier timing)" % (s, hist[s] / 60))
        else:
            notes.append("%s: no timing" % s)
    return (total or None), "; ".join(notes)


def summary(run_dir):
    return read_json(os.path.join(run_dir, "summary.json")) or {}


def gap_run(run_dir):
    return read_json(os.path.join(run_dir, "gap-run.json")) or {}


def verdict_class(row):
    """Collapse a classes row into: accepted / raced out / noise / holdout / gates / cross-world / knight / other."""
    if row.get("accepted"):
        return "accepted"
    if row.get("raced_out"):
        return "raced out"
    why = (row.get("why") or "").lower()
    for key, name in (("noise", "noise"), ("holdout", "holdout"), ("gate", "gates"), ("cross-world", "cross-world"), ("knight", "knight"),
                      ("raced", "raced out")):
        if key in why:
            return name
    return "other"


def class_rows(summ, cls):
    c = (summ.get("stages", {}).get("classes", {}) or {}).get(cls) or {}
    return c.get("rows", [])


def classes_of(summ):
    return list((summ.get("stages", {}).get("classes", {}) or {}).keys())


def kept_specs(summ):
    """The final accepted table: joint.table when the joint stage ran, else every class's kept list."""
    st = summ.get("stages", {})
    j = st.get("joint")
    if isinstance(j, dict) and j.get("table"):
        return list(j["table"])
    out = []
    for cls, c in (st.get("classes", {}) or {}).items():
        out += c.get("kept", [])
    return out


def accepted_sets(summ):
    """--set specs of the shared stages (globals, worth, loot) whose verdict starts with ACCEPT."""
    out = []
    for stage in ("globals", "worth", "loot", "logistics"):
        v = summ.get("stages", {}).get(stage)
        if isinstance(v, list):
            for it in v:
                if isinstance(it, dict) and it.get("set") and str(it.get("verdict", "")).upper().startswith("ACCEPT"):
                    out.append(it["set"])
    return out


def run_state(run_dir, procs=()):
    """-> dict for a run list row: name, dir, mtime, running (pid list), stages, last line."""
    entries = parse_log(os.path.join(run_dir, "progress.log"))
    pid = read_text(os.path.join(run_dir, "pid")).strip()
    running = [p for p in procs if p.get("out") and os.path.abspath(p["out"]) == os.path.abspath(run_dir) and p.get("is_driver")]
    mt = 0
    for fn in ("progress.log", "summary.json"):
        try:
            mt = max(mt, os.path.getmtime(os.path.join(run_dir, fn)))
        except OSError:
            pass
    return {"dir": run_dir, "name": os.path.basename(run_dir.rstrip("/")), "mtime": mt, "running": running, "entries": entries, "pidfile": pid}


def discover_runs(roots):
    """Directories directly under each root (and the roots themselves) holding progress.log or summary.json."""
    found = []
    seen = set()
    for r in roots:
        if not r or not os.path.isdir(r):
            continue
        cands = [r] + [os.path.join(r, n) for n in sorted(os.listdir(r))]
        for c in cands:
            if os.path.isdir(c) and c not in seen and (os.path.exists(os.path.join(c, "progress.log")) or os.path.exists(os.path.join(c, "summary.json"))):
                seen.add(c)
                found.append(c)
    return found
