#!/usr/bin/env python3
"""Regenerates the three SYNTHETIC fixture runs. Every number is invented; no list data."""
import json, os, random

HERE = os.path.dirname(os.path.abspath(__file__))
BRACKETS = ["foe_0.5x", "foe_0.75x", "foe_1x", "foe_1.5x", "foe_2x", "boss"]
CLASSES = [  # name, tier, line, planned
    ("Fighter", 1, "Melee", False), ("Archer", 1, "Ranged", False), ("Mage", 1, "Mage", False),
    ("Knight", 2, "Melee", False), ("Evoker", 2, "Mage", False), ("Paladin", 2, "Hybrid", True),
]
MIX = {
    "Melee": ["melee", "block", "shout", "potion"], "Ranged": ["ranged", "melee", "retreat", "potion"],
    "Mage": ["spell_attack", "heal", "ward", "melee"], "Hybrid": ["melee", "heal", "spell_attack", "block"],
}
SCEN = [("lone_archer_vs_pack", "Archer"), ("archer_kite_melee", "Archer"), ("archer_low_ammo", "Archer"),
        ("fighter_vs_boss", "Fighter"), ("fighter_two_foes", "Fighter"), ("fighter_low_hp", "Fighter"),
        ("mage_oom_vs_boss", "Mage"), ("mage_heal_player", "Mage"), ("mage_ward_vs_caster", "Mage"),
        ("knight_taunt_pack", "Knight"), ("knight_shield_rank", "Knight"), ("knight_vs_ranged", "Knight"),
        ("evoker_aoe_cluster", "Evoker"), ("evoker_heal_double", "Evoker"), ("evoker_boss_burst", "Evoker"),
        ("paladin_heal_or_hit", "Paladin"), ("paladin_ward_window", "Paladin")]
GAPS = [("G1", "idle", "*", "idle with a valid candidate", 0.08), ("G2", "heal", "Mage", "late heal on a falling player", 0.11),
        ("G3", "heal", "Evoker", "double heal on one target", 0.07), ("G4", "target", "*", "target switch churn", 0.05),
        ("G5", "melee", "Mage", "mage steps into melee at low mana", 0.14), ("G6", "ranged", "Archer", "overkill on nearly dead foe", 0.04),
        ("G7", "taunt", "Knight", "taunt cadence too slow", 0.06), ("G8", "ward", "Evoker", "ward cast after the hit lands", 0.09),
        ("G9", "hold", "Fighter", "hold pick keeps a worse weapon", 0.03), ("G10", "boss", "*", "mana spent before the boss", 0.10)]
CL = ["open", "open", "closing", "closed"]


def metrics(line, prog, rnd, b):
    def j(x, s=0.1):
        return round(x * (1 + rnd.uniform(-s, s)), 2)
    k = 1.0 - 0.45 * prog  # fixtures: improvement over runs
    return {"idle_pct": j(9 * k), "idle_cand_pct": j(4 * k), "hold_pct": j(6), "hold_low_pct": j(2 * k),
            "switches_per_min": j(7 * (0.5 + 0.5 * k)), "target_switch_per_min": j(4 * k),
            "healed_pct": j(70 + 20 * prog), "overheal_pct": j(18 * k), "double_heal_pct": j(8 * k),
            "overkill_pct": j(12 * k), "mp_at_boss_pct": j(40 + 30 * prog),
            "melee_share_pct": j(30) if line in ("Melee", "Hybrid") else j(14 - 11 * prog),
            "taunt_per_min": j(1.2), "power_per_min": j(2.0), "follower_death_pct": j(6 * k * (1 + 0.3 * b)),
            "player_death_pct": j(2 * k), "ttk_s": j(14 * (1 + 0.25 * b)), "damage_taken_per_foe": j(55 * (1 + 0.2 * b)),
            "bad_facts": 0, "invalid": 0, "decide_us_mean": j(41), "decide_us_max": j(310, 0.3), "fixture_extra_metric": j(1)}


def build(rid, when, lst, branch, sha, label, prog, seed):
    rnd = random.Random(seed)
    classes = []
    for name, tier, line, planned in CLASSES:
        for b, br in enumerate(BRACKETS):
            fam = MIX[line]
            raw = [rnd.uniform(1, 5) for _ in fam]
            mix = {f: round(100 * r / sum(raw), 1) for f, r in zip(fam, raw)}
            classes.append({"class": name, "tier": tier, "line": line, "planned": planned, "bracket": br,
                            "rank": "all", "fights": 40 + 10 * (b % 3), "metrics": metrics(line, prog, rnd, b), "action_mix": mix})
    scen = []
    for n, c in SCEN:
        o = round(rnd.uniform(0.6, 0.9), 3)
        g = round(max(0.0, rnd.uniform(0.02, 0.18) * (1 - 0.5 * prog)), 3)
        scen.append({"name": n, "class": c, "iaus_u": round(o - g, 3), "oracle_u": o, "gap": g,
                     "metrics": {"idle_pct": round(rnd.uniform(0, 12) * (1 - 0.4 * prog), 2)}})
    mm = round(14 - 11 * prog, 2)
    dh = round(6.5 - 3 * prog, 2)
    rul = [{"id": "R1", "text": "T1 mage melee_share_pct <= 5", "class": "Mage", "metric": "melee_share_pct", "op": "<=", "target": 5, "value": mm, "pass": mm <= 5},
           {"id": "R2", "text": "no bad facts anywhere", "class": "*", "metric": "bad_facts", "op": "<=", "target": 0, "value": 0, "pass": True},
           {"id": "R3", "text": "evoker double heal <= 4%", "class": "Evoker", "metric": "double_heal_pct", "op": "<=", "target": 4, "value": dh, "pass": dh <= 4},
           {"id": "R4", "text": "knight taunts >= 1 per min", "class": "Knight", "metric": "taunt_per_min", "op": ">=", "target": 1, "value": 1.2, "pass": True}]
    gaps = [{"id": i, "family": f, "class": c, "desc": d, "delta_u": round(u * (1 - 0.4 * prog), 3),
             "status": CL[min(3, int(prog * 2) + (n % 2))] if n < 6 else CL[(n + int(prog * 2)) % 3]}
            for n, (i, f, c, d, u) in enumerate(GAPS)]
    gaps[4]["status"] = "limit"
    return {"schema": 1, "run_id": rid, "time": when, "branch": branch, "sha": sha, "list": lst, "seed": seed,
            "fights_per_cell": 50, "label": label, "fixture": True, "oracle": {"k": 3, "horizon_s": 8, "kind": "fixture-expert"},
            "classes": classes, "scenarios": scen, "rulings": rul, "gaps": gaps, "unknown_future_field": {"x": 1}}


ARCH = ["swarm", "archers", "mages", "healer", "boss", "undead", "beast", "ambush", "kite", "mixed"]
GROUPS = {"knight line": ["Knight"], "casters": ["Mage", "Evoker", "Paladin"], "*": [c[0] for c in CLASSES]}


def build2(rid, when, lst, branch, sha, label, prog, seed, with_slugs):
    """A SCHEMA 2 run shaped like a normal class report run of the simulator: NO named scenario rows, a gap ledger that names
    class GROUPS, the per-class oracle gaps (class_gaps) and the rating battery. with_slugs=False leaves gaps[].classes out,
    so the dashboard's own group -> class fallback is exercised."""
    d = build(rid, when, lst, branch, sha, label, prog, seed)
    rnd = random.Random(seed * 7 + 1)
    d["schema"] = 2
    d["mode"], d["stealth"] = "iaus", "g3"
    d["scenarios"] = []
    d["gaps"] = [{"id": "G1", "family": "taunt", "class": "knight line", "desc": "no survivability term in the taunt", "delta_u": round(4.0 * (1 - 0.5 * prog), 2), "status": "open"},
                 {"id": "G5", "family": "cast offense", "class": "casters", "desc": "no overkill term", "delta_u": round(2.0 * (1 - 0.5 * prog), 2), "status": "open"},
                 {"id": "G9", "family": "attack target", "class": "*", "desc": "target switches above the field rate", "delta_u": None, "status": "closing"}]
    if with_slugs:
        for g in d["gaps"]:
            g["classes"] = GROUPS[g["class"]]
    d["class_gaps"] = [{"class": n, "gap": round(rnd.uniform(1, 9) * (1 - 0.4 * prog), 2), "se": round(rnd.uniform(0.5, 2), 2), "fights": 6,
                        "natural_gap": 1.0, "natural_fights": 4, "scenario_gap": None, "scenario_fights": 0} for n, _, _, _ in CLASSES if n != "Paladin"]
    rows = []
    for n, tier, _, _ in CLASSES:
        by = {a: round(rnd.uniform(55, 90), 1) for a in ARCH}
        rows.append({"class": n, "tier": tier, "fights": 120, "score": round(sum(by.values()) / len(by), 2), "se": 1.1, "by_archetype": by})
    for tier in (1, 2):
        grp = sorted([r for r in rows if r["tier"] == tier], key=lambda r: -r["score"])
        mean = sum(r["score"] for r in grp) / len(grp)
        for i, r in enumerate(grp):
            r.update({"rank": i + 1, "of": len(grp), "tier_mean": round(mean, 2), "delta": round(r["score"] - mean, 2),
                      "below": r["score"] - mean < -2.2, "weakest": min(r["by_archetype"], key=r["by_archetype"].get), "weakest_delta": -3.0})
    d["rating"] = {"fights_per_cell": 3, "cells": 40, "classes": rows}
    return d


RUNS = [("20261101-100000-fx1", "2026-11-01T10:00:00", "Vanilla", "fixture/a", "aaaa111", "FIXTURE baseline", 0.0, 11),
        ("20261105-143000-fx2", "2026-11-05T14:30:00", "Tuxborn", "fixture/b", "bbbb222", "FIXTURE other list, mid tuning", 0.5, 22),
        ("20261109-091500-fx3", "2026-11-09T09:15:00", "Vanilla", "fixture/c", "cccc333", "FIXTURE melee share fix", 1.0, 33)]
for r in RUNS:
    with open(os.path.join(HERE, "run-%s.json" % r[0]), "w") as f:
        json.dump(build(*r), f, indent=1)

RUNS2 = [("20261110-080000-fx4", "2026-11-10T08:00:00", "Vanilla", "fixture/d", "dddd444", "FIXTURE schema 2, ledger groups without slugs", 0.2, 44, False),
         ("20261111-080000-fx5", "2026-11-11T08:00:00", "Tuxborn", "fixture/e", "eeee555", "FIXTURE schema 2, other list (not a trend base)", 0.5, 55, True),
         ("20261112-080000-fx6", "2026-11-12T08:00:00", "Vanilla", "fixture/f", "ffff666", "FIXTURE schema 2, class report run", 0.9, 66, True)]
for r in RUNS2:
    with open(os.path.join(HERE, "run-%s.json" % r[0]), "w") as f:
        json.dump(build2(*r), f, indent=1)
