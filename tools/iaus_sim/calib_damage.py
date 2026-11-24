#!/usr/bin/env python3
"""iaus_sim/calib_damage.py -- incoming damage on followers out of saved field logs, for the lab's foe damage model (iaus-sim-v3.md 15).

READ-ONLY. usage: calib_damage.py <run-dir> [...]     (a run dir holds the MFO.log of ONE session)

WHAT THE LOGS HOLD (no per-hit damage line exists in MFO.log or APMF.log; these are the only health reads):
  [sense] <id>: own=<0|1> foes=<n> load=<x> hpLoss=<r>%/s ...     scheduler/CombatScan.cpp, one line per follower about every 3.3 s.
            hpLoss is Confidence::HpLossRate: the NET health fraction lost over the last 5 s window, the loss CAPPED at 25 % of his
            maximum, over max(span, 5 s). So it tops out at 5.0 %/s and a one-hit kill reads 5.0, not 100. The lab computes the SAME
            number for its followers (sim --damage-report), so the two columns compare like for like.
  [bleed] <id> <name>: DOWN (essential down; was alive for <s>s)   one line per down of a follower. "DOWN (?(9); ...) hp 100%" is NOT
            a down (life state 9; the log says so itself) and is counted apart. A down within 60 s of his last one is a RE-DOWN: he got
            up at 1-3 % health, so anything downs him again; the burst figures are for FIRST downs
  [bleed] <id> <name>: UP (alive after <s>s essential down)
  [heal-obs] <caster> -> <target> ... hp <a>% -> <b>% after <t>s    a health point of the heal's target, twice
  [iaus-why] <name> (<id>) cast_heal ... -> <name>(<id>) ... recipient_hp x=<f>   a health point of a heal's recipient (0..1)

WHAT IS REPORTED:
  SENSE    in-combat samples (own=1, foes>0), per foes bucket (1-2, 3-4, 5+): n, share losing (hpLoss > 0), mean hpLoss %/s, the share
           at the cap (>= 4.9 %/s), and the mean over losing samples
  DOWNS    downs per follower-minute of combat (a combat minute = in-combat sense samples x their spacing), and per bucket of the
           foes count on the last sense sample before the down
  BURST    per down: the seconds since his last sense sample that read NO loss (hpLoss 0, so his health had not fallen over the 5 s
           before it), i.e. an upper bound on "from holding to down"; and the last health point above 75 % of his, if one is known
  POINTS   health-loss rates between two health points of one follower at most 6 s apart (uncapped, but only heal recipients have
           points, so it is the loss rate of followers somebody is trying to heal)
  HITS     (sim v4 slice 1, review F-A of feat/oracle-unbounded) from the fill probe's hit lines, where a session has them
           ([iaus-fill] hit <attacker> -> <target> source <form> ... | health before <a> at-event <e> after <b>: took <x> in <s> s ... |
           estimate <e> (hit <h> for <kind> <form>, armor <share>, ...); [Debug] bIausFillProbe = 1). What a line is, and is not:
             - "took" is the drop between the WORKER's two health samples around the event (0.12 to 0.55 s apart). It is a WINDOW, not a hit.
             - the engine raises the event twice for most blows (identical time, pair, source and window: one line is dropped), and once
               more per rider of the blow (an enchantment, a poison, a perk spell: another source form at the same instant). All events of
               one attacker on one target inside one window are ONE BLOW here; a blow with a rider is an ENCHANTED blow.
             - lines are rate limited ("lines skipped"), and an event can land one sample late, so a window can read 0 or a sliver.
           WINDOWS  every distinct window with a loss that a foe's hit on a follower sits in: its share of the target's health (the
                    largest health the session shows for him stands in for his maximum, which no line prints). This is what a BURST is,
                    and what the one-hit line (World.h kOneHitLine) is read from: p90.
           EVENTS   the per-blow sizes the lab's hit-size spread (World.h kFieldHit) is drawn from. A window counts only when it holds
                    ONE blow of ONE foe, the attacker's weapon was sampled (kind "weapon"), every source of the blow is that weapon (no
                    rider), it is not a power attack, a sneak attack, a bash or blocked, and the loss is at least MIN_SHARE of the
                    attacker's own after-armor estimate (ASSUMPTION: below that the blow landed in the neighbouring sample; the estimate
                    reads 2 to 6 times UNDER the real hit, so a real blow cannot be this far under it). Size = loss / (1 - armor share),
                    over the median of the same attacker NAME and weapon (a kind needs MIN_KIND blows). The printed kFieldHit row is those
                    ratios divided by their mean (mean 1), sorted.
Every number names its n. A session is not a fight: the gaps between fights are dropped by the own=1 / foes>0 filter. Two run dirs that
hold the same session (a log pulled twice) count once: sessions are keyed by the MFO.log's first timestamp and its hit lines."""
import os
import re
import sys
from collections import defaultdict

TS = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]")
SENSE = re.compile(r"\[sense\] ([0-9A-F]{8}): own=(\d) foes=(\d+) load=([0-9.]+) hpLoss=([0-9.]+)%/s")
BLEED = re.compile(r"\[bleed\] ([0-9A-F]{8}) (.*?): (DOWN|UP) \((essential down; was alive for ([0-9.]+)s|alive after|\?)")
HOBS = re.compile(r"\[heal-obs\] ([0-9A-F]{8}) -> ([0-9A-F]{8}) .*? hp (\d+)% -> (\d+)% after ([0-9.]+)s")
HOBS_SELF = re.compile(r"\[heal-obs\] ([0-9A-F]{8}) .*?road=direct self.*? hp (\d+)% -> (\d+)% after ([0-9.]+)s")
HIT = re.compile(r"\[iaus-fill\] hit (.*?) \(([0-9A-F]{8})\) -> (.*?) \(([0-9A-F]{8})\) source ([0-9A-F]{8}) projectile [0-9A-F]{8} power=(\w+) bash=(\w+) blocked=(\w+) "
                 r"sneak=(\w+) \| health before (\S+) at-event \S+ after (\S+): took (\S+) in (\S+) s, \d+ other hit\(s\) on him in it \| estimate \S+ "
                 r"\(hit (\S+) for (\w+) ([0-9A-F]{8}), armor (\S+), magic resist")
MIN_SHARE = 0.25   # ASSUMPTION (see HITS above): a window under this share of the attacker's own after-armor estimate does not hold the blow
MIN_KIND = 3       # blows an attacker kind needs before its median is a reference
PLAYER = "00000014"
RECIP = re.compile(r"\[iaus-why\] (.*?) \(([0-9A-F]{8})\) cast_heal [^|]*?(?:-> [^|(]*\(([0-9A-F]{8})\) )?= [0-9.]+ \|.*?recipient_hp x=([0-9.]+)")


def bucket(n):
    return "1-2" if n <= 2 else "3-4" if n <= 4 else "5+"


def parse(path):
    """One session: (sense samples, bleed events, health points) with times in seconds of the day."""
    sense, bleed, pts, names = defaultdict(list), [], defaultdict(list), {}
    for line in open(path, errors="replace"):
        m = TS.match(line)
        if not m:
            continue
        t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000.0
        if "[sense]" in line:
            s = SENSE.search(line)
            if s:
                sense[s.group(1)].append((t, int(s.group(2)), int(s.group(3)), float(s.group(4)), float(s.group(5))))
        elif "[bleed]" in line:
            b = BLEED.search(line)
            if b:
                real = b.group(3) == "DOWN" and b.group(5) is not None   # "DOWN (?(9); ...) hp 100%" is life state 9, which the log itself says is not a down
                bleed.append((t, b.group(1), "DOWN" if real else "LS9" if b.group(3) == "DOWN" else "UP", float(b.group(5)) if real else 0.0))
                names[b.group(1)] = b.group(2)
        elif "[heal-obs]" in line:
            h = HOBS.search(line)
            if h:
                dt = float(h.group(5))
                pts[h.group(2)].append((t - dt, int(h.group(3)) / 100.0))
                pts[h.group(2)].append((t, int(h.group(4)) / 100.0))
            else:
                h = HOBS_SELF.search(line)
                if h:
                    dt = float(h.group(4))
                    pts[h.group(1)].append((t - dt, int(h.group(2)) / 100.0))
                    pts[h.group(1)].append((t, int(h.group(3)) / 100.0))
        elif "recipient_hp x=" in line:
            r = RECIP.search(line)
            if r:
                names[r.group(2)] = r.group(1)
                pts[r.group(3) or r.group(2)].append((t, float(r.group(4))))
    return sense, bleed, pts, names


def parse_hits(path):
    """The foe-on-follower hit events of one session, de-duplicated: [(t, attacker name, attacker id, target id, target name, source, flags,
    before, after, took, kind, weapon, weapon hit, armor)], plus the session's follower ids (those with [sense] lines) and its key."""
    fol, ev, first = set(), {}, None
    for line in open(path, errors="replace"):
        m = TS.match(line)
        if not m:
            continue
        if first is None:
            first = line[:14]
        if "[sense]" in line:
            s = SENSE.search(line)
            if s:
                fol.add(s.group(1))
            continue
        if "[iaus-fill] hit " not in line:
            continue
        h = HIT.search(line)
        if not h:
            continue
        t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000.0
        g = h.groups()
        key = (round(t, 3), g[1], g[3], g[4], g[9], g[10])   # the engine's second event of one blow: same instant, pair, source and window
        e = ev.get(key)
        if e is None:
            e = ev[key] = {"t": t, "an": g[0], "a": g[1], "tn": g[2], "tg": g[3], "src": g[4], "power": g[5] == "true", "bash": g[6] == "true", "blocked": g[7] == "true",
                           "sneak": g[8] == "true", "before": float(g[9]), "after": float(g[10]), "took": float(g[11]), "kind": g[14], "wpn": g[15], "hit": float(g[13]), "armor": float(g[16])}
        elif g[0] != "?" and e["an"] == "?":
            e.update({"an": g[0], "kind": g[14], "wpn": g[15], "hit": float(g[13])})
    evs = [e for e in ev.values() if e["tg"] in fol and e["a"] not in fol and e["a"] != PLAYER]
    return evs, fol, (first, len(ev))


def hits_report(dirs):
    """HITS: the windows and the per-blow sizes over every session that has hit lines (docstring HITS)."""
    seen, wins, blows, used = set(), [], [], []
    for d in dirs:
        path = d if d.endswith(".log") else os.path.join(d, "MFO.log")
        evs, fol, key = parse_hits(path)
        if not evs:
            continue
        if key in seen:
            print("HITS  %s: the same session as an earlier run dir (first line %s, %d events), counted once" % (d, key[0], key[1]))
            continue
        seen.add(key)
        used.append(d)
        hpmax = defaultdict(float)
        for e in evs:
            for v in (e["before"], e["after"]):
                if v == v:
                    hpmax[e["tg"]] = max(hpmax[e["tg"]], v)
        w = defaultdict(list)
        for e in evs:
            if e["took"] == e["took"]:
                w[(e["tg"], e["before"], e["after"])].append(e)
        nw = nb = 0
        for (tg, before, after), es in sorted(w.items(), key=lambda kv: min(x["t"] for x in kv[1])):
            took = es[0]["took"]
            if not took > 0.0:
                continue
            nw += 1
            wins.append((took / hpmax[tg], took, es[0]["tn"], d, min(x["t"] for x in es), len({(x["a"], round(x["t"], 1)) for x in es})))
            attackers = {x["a"] for x in es}
            instants = {round(x["t"], 1) for x in es}
            if len(attackers) != 1 or len(instants) != 1:
                continue   # more than one blow in the window: its loss cannot be split
            ref = next((x for x in es if x["kind"] == "weapon"), None)
            if ref is None or any(x["power"] or x["sneak"] or x["bash"] or x["blocked"] for x in es):
                continue   # unsampled attacker, a spell, unarmed; power / sneak / bash / blocked
            if any(x["src"] != ref["wpn"] for x in es):
                continue   # a rider (enchantment, poison, perk spell), or another source than the sampled weapon
            if not ref["armor"] < 1.0 or took < MIN_SHARE * ref["hit"] * (1.0 - ref["armor"]):
                continue   # ASSUMPTION MIN_SHARE: the blow landed in the neighbouring sample
            nb += 1
            blows.append(((ref["an"], ref["wpn"]), took / (1.0 - ref["armor"]), took / hpmax[tg], ref["tn"], d))
        print("HITS  %s: %d foe-on-follower event(s), %d window(s) with a loss, %d plain single blow(s)" % (d, len(evs), nw, nb))
    if not wins:
        print("HITS  no session given has [iaus-fill] hit lines of a foe on a follower (run with [Debug] bIausFillProbe = 1)")
        return
    sh = sorted(x[0] for x in wins)
    q = lambda xs, p: xs[min(len(xs) - 1, int(p * len(xs)))]
    print("FIELD HIT WINDOWS n=%d over %d session(s): loss as a share of the target's health  median %.1f%%  p75 %.1f%%  p90 %.1f%%  max %.1f%%  (30%% or more: %d)" % (
        len(sh), len(used), 100 * q(sh, 0.5), 100 * q(sh, 0.75), 100 * q(sh, 0.9), 100 * sh[-1], sum(x >= 0.30 for x in sh)))
    kinds = defaultdict(list)
    for b in blows:
        kinds[b[0]].append(b[1])
    ratios, share = [], []
    for k, xs in sorted(kinds.items()):
        xs = sorted(xs)
        med = xs[len(xs) // 2] if len(xs) % 2 else 0.5 * (xs[len(xs) // 2 - 1] + xs[len(xs) // 2])
        ok = len(xs) >= MIN_KIND
        print("  kind %-22s weapon %s  n=%-2d median %.1f before armor%s: %s" % (k[0], k[1], len(xs), med, "" if ok else "  (under %d blows: not used)" % MIN_KIND, " ".join("%.2f" % (x / med) for x in xs)))
        if ok:
            ratios += [x / med for x in xs]
    share = sorted(b[2] for b in blows)
    print("FIELD HIT EVENTS n=%d plain single blows (%d in kinds of %d or more): share of the target's health  median %.1f%%  p90 %.1f%%  max %.1f%%" % (
        len(blows), len(ratios), MIN_KIND, 100 * q(share, 0.5) if share else 0.0, 100 * q(share, 0.9) if share else 0.0, 100 * share[-1] if share else 0.0))
    if ratios:
        mean = sum(ratios) / len(ratios)
        rs = sorted(r / mean for r in ratios)
        print("FIELD HIT SPREAD n=%d  mean of the ratios %.3f  after the division by it: median %.2f  p90 %.2f  max %.2f  (twice the median or more: %d)" % (
            len(rs), mean, q(rs, 0.5), q(rs, 0.9), rs[-1], sum(r >= 2.0 * q(rs, 0.5) for r in rs)))
        print("kFieldHit[%d] = { %s };" % (len(rs), ", ".join("%.2ff" % r for r in rs)))


def main():
    dirs = [a for a in sys.argv[1:] if not a.startswith("-")]
    if not dirs:
        print(__doc__)
        print("READ-ONLY: no run dir given")
        return 2
    S = defaultdict(lambda: [0, 0, 0.0, 0])          # bucket -> n, losing, sum, at cap
    minutes, downs, down_b, alld, alld_sessions = 0.0, 0, defaultdict(int), [], []
    burst, above75, rates, wipes, ls9, redowns = [], [], [], [], [0], [0]
    for d in dirs:
        path = d if d.endswith(".log") else os.path.join(d, "MFO.log")
        sense, bleed, pts, names = parse(path)
        nd = 0
        alld += [t + 100000.0 * len(alld_sessions) for (t, fid, what, alive) in bleed if what == "DOWN"]
        alld_sessions.append(d)
        for fid, rows in sense.items():
            gaps = [b[0] - a[0] for a, b in zip(rows, rows[1:]) if 0 < b[0] - a[0] < 8]
            step = sorted(gaps)[len(gaps) // 2] if gaps else 3.3
            for (t, own, foes, load, loss) in rows:
                if own != 1 or foes <= 0:
                    continue
                r = S[bucket(foes)]
                r[0] += 1
                r[1] += loss > 0
                r[2] += loss
                r[3] += loss >= 4.9
                minutes += step / 60.0
        dts = sorted(t for (t, fid, what, alive) in bleed if what == "DOWN")
        for a, b in zip(dts, dts[1:]):
            wipes.append(b - a)
        for (t, fid, what, alive) in bleed:
            if what == "LS9":
                ls9[0] += 1
            if what != "DOWN":
                continue
            downs += 1
            nd += 1
            if alive < 60.0:
                redowns[0] += 1
                continue
            before = [r for r in sense.get(fid, []) if r[0] <= t]
            if before:
                down_b[bucket(before[-1][2]) if before[-1][2] > 0 else "0"] += 1
                calm = [r for r in before if r[4] == 0.0]
                if calm and t - calm[-1][0] < 120:
                    burst.append((t - calm[-1][0], t - before[-1][0], before[-1][4], names.get(fid, fid), os.path.basename(os.path.normpath(d))))
            hi = [p for p in pts.get(fid, []) if p[0] <= t and p[1] > 0.75 and t - p[0] < 60]
            if hi:
                above75.append((t - hi[-1][0], hi[-1][1], names.get(fid, fid)))
        for fid, ps in pts.items():
            ps.sort()
            for a, b in zip(ps, ps[1:]):
                dt = b[0] - a[0]
                if 0.3 <= dt <= 6.0 and a[1] > b[1]:
                    rates.append(100.0 * (a[1] - b[1]) / dt)
        print("SESSION %s: %d follower(s) with sense lines, %d down(s), %d health point(s)" % (d, len(sense), nd, sum(len(v) for v in pts.values())))
    print("\nSENSE  in-combat samples (own=1, foes>0); hpLoss is capped at 5.0 %/s by the game's own reader")
    tot = [0, 0, 0.0, 0]
    for b in ("1-2", "3-4", "5+"):
        n, lo, sm, cp = S[b]
        tot = [tot[0] + n, tot[1] + lo, tot[2] + sm, tot[3] + cp]
        if n:
            print("FIELD SENSE foes %-3s n=%-4d losing %5.1f%%  mean %.2f %%/s  at-cap %5.1f%%  mean-when-losing %.2f %%/s" % (b, n, 100.0 * lo / n, sm / n, 100.0 * cp / n, sm / lo if lo else 0.0))
    n, lo, sm, cp = tot
    if n:
        print("FIELD SENSE foes all n=%-4d losing %5.1f%%  mean %.2f %%/s  at-cap %5.1f%%  mean-when-losing %.2f %%/s" % (n, 100.0 * lo / n, sm / n, 100.0 * cp / n, sm / lo if lo else 0.0))
    print("\nDOWNS")
    print("FIELD DOWNS %d (%d first, %d re-downs within 60 s of getting up) in %.1f follower-minutes of combat = %.3f per follower-minute (first downs %.3f); first downs by foes on the last sample: %s; not counted: %d life-state-9 lines" % (
        downs, downs - redowns[0], redowns[0], minutes, downs / minutes if minutes else 0.0, (downs - redowns[0]) / minutes if minutes else 0.0, " ".join("%s:%d" % kv for kv in sorted(down_b.items())), ls9[0]))
    print("FIELD PILE  downs that came within 12 s of another follower's down in the same session: %d of %d (the gaps between successive downs, s: %s)" % (
        sum(1 for i, t in enumerate(sorted(alld)) if any(abs(t - u) <= 12 and j != i for j, u in enumerate(sorted(alld)))), len(alld), " ".join("%.0f" % w for w in wipes)))
    print("\nBURST  seconds from his last sense sample that read no loss to the down (the sample spacing, about 3.3 s, is the resolution)")
    for x in sorted(burst):
        print("  %5.1f s after a no-loss sample (last sample %.1f s before the down read %.1f %%/s)  %s  [%s]" % x)
    if burst:
        xs = sorted(b[0] for b in burst)
        print("FIELD BURST n=%d  median %.1f s  under 3.5 s: %d  under 7 s: %d  (a down whose LAST sample read no loss: %d)" % (len(xs), xs[len(xs) // 2], sum(x < 3.5 for x in xs), sum(x < 7 for x in xs), sum(1 for b in burst if b[2] == 0.0)))
    for x in sorted(above75):
        print("  above 75%% (%.0f%%) %.1f s before the down  %s" % (100 * x[1], x[0], x[2]))
    print("\nPOINTS  loss between two health points of a heal recipient at most 6 s apart (uncapped)")
    if rates:
        rs = sorted(rates)
        print("FIELD POINTS n=%d  median %.1f %%/s  p75 %.1f  p90 %.1f  max %.1f" % (len(rs), rs[len(rs) // 2], rs[(3 * len(rs)) // 4], rs[(9 * len(rs)) // 10], rs[-1]))
    print("\nHITS  the fill probe's hit lines (windows = bursts; events = plain single blows; see the header)")
    hits_report(dirs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
