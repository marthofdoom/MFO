#!/usr/bin/env python3
"""calib_traversal.py -- turn the N0 passive field probes in MFO.log into the lab's ability file and a summary.

Reads the [probe-trav], [probe-drop], [probe-point], [probe-ray] and [probe-trust] lines native/probes/ writes
(Docs/research/2026-11/iaus-sim-v4.md 8.8 item 6, 10.2 row N0). Pure text in, JSON and a summary out. Touches no game file.

Usage:
  calib_traversal.py MFO.log [more.log ...] [--out abilities.json] [--min-obs N]

Output JSON (the shape the lab's --traversal-file loads; UNVERIFIED until a session has produced numbers):
  { "source": [...logs], "actors": { "<fid>": { "name", "moves": n, "up": {n, max, median}, "down": {...}, "moveSec": {n, median, p90, max},
      "failed": n, "noStop": n, "livedDuringMove": n } },
    "abilities": { "maxUp": ..., "maxDown": ..., "moveSec": ..., "n": ... },     # party-wide, from the clean stops only
    "dropLinks": { "noEventDrops": n, "maxDown": ..., "actors": [...] },        # a follower losing height with NO graph event
    "pointRoad": [ one row per closed claim: kind, R, inside seconds, walked inside, u/s, closest, verdict hint ],
    "rayCost": { site: { "meanUs": ..., "p95UpperUs": ..., "maxUs": ..., "n": ... } },
    "trust": { sign: count } }
A drop with graph event NONE is the answer to "do followers use the navmesh's own drop links": it is a candidate, not a proof (the
link kind under him needs the N1 snapshot). It becomes a proof once a dump of the same cell shows a drop link at those two positions.
"""
import argparse, json, re, statistics, sys
from collections import Counter, defaultdict

TRAV = re.compile(r"\[probe-trav\] (?P<fid>[0-9A-Fa-f]{8}) '(?P<name>[^']*)': (?P<how>[A-Z-]+(?: \([^)]*\))?) \((?P<ev>[^)]*)\) dur=(?P<dur>-?[0-9.]+)s "
                  r"start=(?P<start>\([^)]*\)|unknown[^|]*?) stop=(?P<stop>\([^)]*\)|\?) dz=(?P<dz>[-+0-9.]+) \| player (?P<player>\([^)]*\)|\?) \| in flight: (?P<fl>[^|]*)"
                  r"\| others mid-move: (?P<oth>\d+) \| lived signs during the move: (?P<lived>\d+)")
DROP = re.compile(r"\[probe-drop\] (?P<fid>[0-9A-Fa-f]{8}) '(?P<name>[^']*)': lost (?P<dz>[0-9.]+) height in (?P<sec>[0-9.]+)s, ran (?P<run>[0-9.]+) across, "
                  r"from (?P<a>\([^)]*\)) to (?P<b>\([^)]*\)) \| graph traversal event=(?P<ev>YES|NONE)")
POINT_CLOSE = re.compile(r"\[probe-point\] (?P<fid>[0-9A-Fa-f]{8}): (?P<kind>\S+) claim CLOSED \((?P<why>[^)]*)\) after (?P<total>[0-9.]+)s, R (?P<r>[0-9.]+): start dist (?P<sd>-?[0-9.]+), "
                         r"closest (?P<min>-?[0-9.]+), last (?P<last>-?[0-9.]+), first inside after (?P<arr>-?[0-9.]+)s, inside (?P<ins>[0-9.]+)s walking (?P<path>[0-9.]+) units "
                         r"\((?P<spd>[0-9.]+) u/s, farthest (?P<far>[0-9.]+) from the point\)")
RAY = re.compile(r"\[probe-ray\] (?P<site>\S+): (?P<n>\d+) rays in (?P<s>[0-9]+)s .*? \| mean (?P<mean>[0-9.]+) us, p50 <= (?P<p50>[0-9.]+) us, p95 <= (?P<p95>[0-9.]+) us, max (?P<max>[0-9.]+) us")
TRUST = re.compile(r"\[probe-trust\] (?P<fid>[0-9A-Fa-f]{8}) '(?P<name>[^']*)': sign=(?P<sign>\S+)")


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))] if xs else None


def stat(xs):
    return {"n": len(xs), "max": max(xs) if xs else None, "median": statistics.median(xs) if xs else None, "p90": pct(xs, 0.9)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--out", help="write the JSON here (default: stdout summary only)")
    ap.add_argument("--min-obs", type=int, default=3, help="a party-wide bound needs this many clean stops (default 3)")
    a = ap.parse_args()

    actors = defaultdict(lambda: {"name": "", "up": [], "down": [], "sec": [], "failed": 0, "noStop": 0, "lived": 0, "moves": 0, "inflight": Counter()})
    drops, points, trust, rays = [], [], Counter(), defaultdict(list)
    for path in a.logs:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if "[probe-" not in line:
                    continue
                if m := TRAV.search(line):
                    r = actors[m["fid"]]
                    r["name"] = m["name"]
                    r["moves"] += 1
                    r["lived"] += int(m["lived"])
                    for w in filter(None, (x.strip() for x in m["fl"].split(","))):
                        r["inflight"][w] += 1
                    how, dur, dz = m["how"], float(m["dur"]), float(m["dz"])
                    if how.startswith("STOP") and dur >= 0 and not m["start"].startswith("unknown"):
                        r["sec"].append(dur)
                        (r["up"] if dz >= 0 else r["down"]).append(abs(dz))
                    elif how.startswith("INTERRUPT"):
                        r["failed"] += 1
                    elif how.startswith("NO-STOP"):
                        r["noStop"] += 1
                elif m := DROP.search(line):
                    drops.append({"fid": m["fid"], "name": m["name"], "dz": float(m["dz"]), "sec": float(m["sec"]), "run": float(m["run"]),
                                  "from": m["a"], "to": m["b"], "graphEvent": m["ev"] == "YES"})
                elif m := POINT_CLOSE.search(line):
                    ins, path, spd, r_ = float(m["ins"]), float(m["path"]), float(m["spd"]), float(m["r"])
                    hint = ("never inside" if ins == 0 else "pinned (barely moved inside)" if path < 0.25 * r_ else "moves tactically inside (walked >= R)" if path >= r_ else "in between")
                    points.append({"fid": m["fid"], "kind": m["kind"], "why": m["why"], "R": r_, "total": float(m["total"]), "insideSec": ins, "walkedInside": path,
                                   "uPerSec": spd, "closest": float(m["min"]), "farthestInside": float(m["far"]), "hint": hint})
                elif m := RAY.search(line):
                    rays[m["site"]].append((int(m["n"]), float(m["mean"]), float(m["p95"]), float(m["max"])))
                elif m := TRUST.search(line):
                    trust[m["sign"]] += 1

    out = {"source": a.logs, "actors": {}, "abilities": {}, "dropLinks": {}, "pointRoad": points, "rayCost": {}, "trust": dict(trust)}
    ups, downs, secs = [], [], []
    for fid, r in actors.items():
        out["actors"][fid] = {"name": r["name"], "moves": r["moves"], "up": stat(r["up"]), "down": stat(r["down"]), "moveSec": stat(r["sec"]),
                              "failed": r["failed"], "noStop": r["noStop"], "livedDuringMove": r["lived"], "inFlight": dict(r["inflight"])}
        ups += r["up"]; downs += r["down"]; secs += r["sec"]
    n = len(secs)
    out["abilities"] = {"n": n, "enough": n >= a.min_obs,
                        "maxUp": max(ups) if ups else None, "maxDown": max(downs) if downs else None,
                        "moveSec": statistics.median(secs) if secs else None,
                        "note": "from clean STOPs only; UNVERIFIED below --min-obs"}
    no_ev = [d for d in drops if not d["graphEvent"]]
    out["dropLinks"] = {"noEventDrops": len(no_ev), "maxDown": max((d["dz"] for d in no_ev), default=None),
                        "actors": sorted({d["fid"] for d in no_ev}), "samples": no_ev[:20],
                        "withEventDrops": len(drops) - len(no_ev)}
    for site, rows in rays.items():
        tot = sum(r[0] for r in rows)
        out["rayCost"][site] = {"n": tot, "meanUs": sum(r[0] * r[1] for r in rows) / tot if tot else None,
                                "p95UpperUs": max(r[2] for r in rows), "maxUs": max(r[3] for r in rows)}

    print(f"traversal moves: {sum(r['moves'] for r in actors.values())} by {len(actors)} actor(s); clean stops {n}; "
          f"maxUp {out['abilities']['maxUp']}, maxDown {out['abilities']['maxDown']}, median {out['abilities']['moveSec']} s")
    print(f"drops with NO graph event (drop-link candidates): {len(no_ev)}; with an event: {len(drops) - len(no_ev)}"
          + ("  -> followers DO lose height with no parkour event: check them against a navmesh dump" if no_ev else "  -> none seen: nothing may rely on drop links yet (9a)"))
    for p in points:
        print(f"point claim {p['kind']} R {p['R']:.0f}: inside {p['insideSec']:.1f}s walked {p['walkedInside']:.0f} ({p['uPerSec']:.0f} u/s) -> {p['hint']}")
    for site, d in out["rayCost"].items():
        print(f"ray {site}: n {d['n']}, mean {d['meanUs']:.1f} us, p95 <= {d['p95UpperUs']:.0f} us, max {d['maxUs']:.1f} us")
    if trust:
        print("lived trust signs: " + ", ".join(f"{k} x{v}" for k, v in trust.items()))
    if a.out:
        with open(a.out, "w", encoding="utf-8") as fh:
            json.dump(out, fh, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
