#!/usr/bin/env python3
"""Unit tests of tune_driver.py's pure helpers (no simulator needed). Run: python3 -I tools/iaus_sim/test_tune_driver.py"""
import re
import os
import sys
import tempfile
import types
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tune_driver as T  # noqa: E402


class LogiMid(unittest.TestCase):
    def test_negative_midpoint_keeps_its_sign(self):   # review F7
        self.assertTrue(all(v < 0 for v in T.logi_mid(-0.1, (0.6, 0.8, 1.25, 1.6), (-0.1, -0.05, 0.05, 0.1, 0.2))))
        self.assertTrue(all(v < 0 for v in T.logi_mid(-0.5, (0.6, 0.8, 1.25, 1.6), (-0.1, 0.1))))

    def test_positive_and_zero_as_before(self):
        self.assertEqual(T.logi_mid(0.1, (0.6,), (-0.1, -0.05, 0.05)), [0.02, 0.05, 0.15000000000000002])
        self.assertTrue(all(v > 0 for v in T.logi_mid(0.0, (0.6,), (-0.1, 0.1))))


class SightGate(unittest.TestCase):   # review F11: the knobs that read sight are a skip list the driver enforces
    def test_the_sight_consideration_and_scalars_are_skipped(self):
        self.assertTrue(T.sight_gated("cast_offense.los")); self.assertTrue(T.sight_gated("attack.los.b")); self.assertTrue(T.sight_gated("t.occClaimRelease")); self.assertTrue(T.sight_gated("t.occHold=4"))
        self.assertFalse(T.sight_gated("attack.threat")); self.assertFalse(T.sight_gated("t.momentum")); self.assertFalse(T.sight_gated("loot.takeThreshold"))

    def test_no_scalar_stage_offers_one_and_the_names_are_real(self):
        # the names on the list are the game's own (iaus/Catalogue.cpp SetTuningScalar, the consideration table): a typo would gate nothing
        cat = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "native", "iaus", "Catalogue.cpp")).read()
        for n in T.SIGHT_GATED_SCALARS:
            self.assertIn('"%s"' % n, cat)
        for n in T.SIGHT_GATED_CONS:
            self.assertIn('"%s"' % n, cat)
        self.assertEqual([n for n, _, _ in T.GLOBALS if not T.sight_gated(n)], [n for n, _, _ in T.GLOBALS if n not in T.SIGHT_GATED_SCALARS])

    def test_the_flag_lifts_it(self):
        T.ALLOW_SIGHT_KNOBS = True
        try:
            self.assertFalse(T.sight_gated("cast_offense.los"))
        finally:
            T.ALLOW_SIGHT_KNOBS = False


class World(unittest.TestCase):   # sim v3: the driver always names the world (v3 by default since the sim mirror batch of 11-17; --world v2 still works)
    def test_world_is_always_first(self):
        self.assertEqual(T.world_flags("v2", ""), ["--world", "v2"])
        self.assertEqual(T.world_flags("v3", "--ranged-perks --point-blank"), ["--world", "v3", "--ranged-perks", "--point-blank"])

    def test_sim_flags_may_not_name_the_world(self):
        with self.assertRaises(SystemExit):
            T.world_flags("v2", "--world v3")


class Cards(unittest.TestCase):   # review F2 of 5749026a: every band of Playstyles.h is parsed, a two-sided or raw one is never silently dropped
    def test_survival_bands_are_parsed(self):
        T.CARDS.clear()
        cards = T.load_cards()
        for cls in ("mage", "evoker", "conjurer", "warlock", "healer"):
            self.assertIn("downs/f", [b[0] for b in cards[cls]["bands"]], cls)
            self.assertIn("selfHealLow%", [b[0] for b in cards[cls]["bands"]], cls)

    def test_band_regexes(self):
        import re
        line = 'inRange("x%", 25, 75, "why"), atMost("y", 3, "why"), Band{ "z", 1, 2, "raw" }'
        self.assertEqual(len(re.findall(r'\b(?:atLeast|atMost|inRange)\(|\bBand\{', line)), 3)
        self.assertEqual(re.findall(r'inRange\("([^"]+)", ([-0-9.]+), ([-0-9.]+)', line), [("x%", "25", "75")])


class Edge(unittest.TestCase):
    def setUp(self):
        T.KNOB_META.clear()
        T.KNOB_META.update({"a.w": ("mul", -T.INF, T.INF, False), "p.y1": ("add", 0.02, 1.0, False), "p.x1": ("add", 0.0, 1.0, True),
                            "n.a": ("mul", -T.INF, T.INF, False), "lv.lo": ("mul", 0.02, 1.0, False)})

    def test_edge_detection(self):
        self.assertEqual(T.edge_of("a.w", 1.0, [0.5, 0.75, 1.5, 2.0], 0.5), "down")
        self.assertEqual(T.edge_of("a.w", 1.0, [0.5, 0.75, 1.5, 2.0], 2.0), "up")
        self.assertIsNone(T.edge_of("a.w", 1.0, [0.5, 0.75, 1.5, 2.0], 1.5))
        self.assertEqual(T.edge_of("n.a", -8.0, [-4.0, -6.0, -10.64, -16.0], -4.0), "down")   # a negative shared value: by ratio

    def test_widen_mul_is_x4_x025(self):
        self.assertEqual(T.widen("a.w", 1.0, [0.5, 0.75, 1.5, 2.0], "down"), [0.35, 0.25])
        self.assertEqual(T.widen("a.w", 1.0, [0.5, 0.75, 1.5, 2.0], "up"), [3.0, 4.0])
        self.assertEqual(T.widen("n.a", -8.0, [-4.0], "down"), [-2.8, -2.0])

    def test_widen_past_a_tried_x025(self):   # batch 3: the crossover tried down to x0.25 still widens (limit 0.02)
        T.KNOB_META["c.x"] = ("mul", 0.02, 1.0, False)
        self.assertEqual(T.widen("c.x", 0.45, [0.1125, 0.1575, 0.225, 0.3375, 0.5985], "down"), [0.07875, 0.05625])
        self.assertEqual(T.widen("a.w", 1.0, [0.5, 0.75, 1.5, 2.0], "down"), [0.35, 0.25])   # unchanged when x0.35 / x0.25 lie past the edge

    def test_widen_stays_in_range(self):
        self.assertEqual(T.widen("p.y1", 0.9, [0.6, 0.75, 1.0], "up"), [])            # the edge is the valid limit
        self.assertEqual(T.widen("lv.lo", 0.5, [0.25, 0.375, 0.665, 0.835], "up"), [1.0])
        self.assertEqual(T.widen("p.x1", 0.5, [0.375, 0.625], "up"), [0.75])           # strict: 4x the step reaches the neighbour (1.0) and is dropped


class CheckWorld(unittest.TestCase):   # review F8
    def test_missing_companion_files_stop(self):
        d = tempfile.mkdtemp()
        T.CACHE_DIR = d + "/"
        T.A = types.SimpleNamespace(out=d)
        open(os.path.join(d, "L.sim.json"), "w").write("{}")
        with self.assertRaises(SystemExit):
            T.check_world("L")                       # no .combatmods.json
        open(os.path.join(d, "L.combatmods.json"), "w").write("{}")
        with self.assertRaises(SystemExit):
            T.check_world("L")                       # no potion-worth table in --out
        T.check_world("L", need_worth=False)
        open(os.path.join(d, "potworth-L.txt"), "w").write("x")
        T.check_world("L")
        self.assertIn("--list", T.world_args("L"))
        T.check_world("builtin")


class Gap(unittest.TestCase):
    def test_classify(self):
        self.assertEqual(T.FIDELITY, {})                                                          # 11-17: the oracle's bash and guard run under the game's cooldowns
        self.assertEqual(T.classify("attack", "ext:bash", "ext", {"bash": 5})[0], "b")          # the family exists, its gate did not offer it
        self.assertEqual(T.classify("attack", "ext:bash", "ext", {})[0], "c")
        T.FIDELITY["ext:bash"] = "x"
        try:
            self.assertEqual(T.classify("attack", "ext:bash", "ext", {"bash": 5})[0], "d")      # a FIDELITY row wins
        finally:
            T.FIDELITY.clear()
        self.assertEqual(T.classify("attack", "ext:to_player", "ext", {"regroup": 0})[0], "c")  # a family never offered
        self.assertEqual(T.classify("attack", "ext:hide", "ext", {"disengage": 3})[0], "b")     # offered elsewhere, gated here
        self.assertEqual(T.classify("attack", "ext:get_sight", "ext", {})[0], "c")              # no family at all
        self.assertEqual(T.classify("attack!", "taunt", "ranked", {})[0], "b")                  # IAUS broke a class ruling
        self.assertEqual(T.classify("attack", "drink_health", "unranked", {})[0], "b")
        c, _, fams = T.classify("attack", "taunt", "ranked", {})
        self.assertEqual((c, fams), ("a", ["attack", "taunt"]))
        self.assertEqual(T.classify("power_attack", "idle", "idle", {})[2], ["power_attack"])

    def test_every_extended_action_of_the_oracle_has_a_family_and_a_road(self):
        # the oracle's extended set (Oracle.h Name(Ext)) and the driver's two tables must name the same actions, so no divergence is classified by a missing row
        src = open(os.path.join(os.path.dirname(os.path.abspath(T.__file__)), "Oracle.h")).read()
        names = set(re.findall(r'"(ext:[a-z_]+)"', src[src.index("inline const char* Name(Ext e)"):src.index("struct Opt {")]))
        self.assertGreaterEqual(len(names), 11)
        self.assertEqual(names, set(T.EXT_FAMILY))
        self.assertEqual(names, set(T.EXT_ROAD))
        c, why, _ = T.classify("cast_offense", "ext:clear_line", "ext", {})
        self.assertEqual(c, "c")
        self.assertNotIn("not in the road table", why)

    def test_parse_refuses_a_missing_dump(self):
        T.CFG = dict(T.SMOKE)
        with self.assertRaises(RuntimeError):
            T.gap_parse("CLASS ORACLE GAP\n", "knight", 1)
        r = T.gap_parse("GAPFIGHT knight nat 0 80.0 85.5\nGAPSIT knight attack|idle|idle|hp:hi,near:melee,foes:1 3 9.5\nGAPGEN knight block 4\nGAPDEC knight 50\n", "knight", 7)
        self.assertEqual((r["fights"], r["sit"]["attack|idle|idle|hp:hi,near:melee,foes:1"], r["gen"]["block"], r["dec"]), ([(7, 0, "nat", 80.0, 85.5)], (3, 9.5), 4, 50))


class Logistics(unittest.TestCase):
    def test_logi_class(self):
        self.assertEqual(T.logi_class("sell_run", "base:go_on", "orc:sell_run")[0], "c")
        self.assertEqual(T.logi_class("gold", "base:leave", "orc:take")[0], "c")
        self.assertEqual(T.logi_class("bow", "base:leave", "orc:take")[0], "c")       # gathering gear to sell
        self.assertEqual(T.logi_class("bow", "base:take", "orc:leave")[0], "a")
        self.assertEqual(T.logi_class("potion_h", "base:leave", "orc:take")[0], "a")  # the potion floor / the row param
        self.assertEqual(T.logi_class("ammo", "base:take", "orc:leave")[0], "a")


class CardGateF4F5(unittest.TestCase):   # review F4 / F5 of b59b0a1a
    def test_every_cell_has_a_card_and_parses(self):
        T.CARDS.clear()
        cards = T.load_cards()
        for c in T.CELLS:
            self.assertIn(T.CARD_ALIAS.get(c, c), cards, c)
        self.assertTrue(all(cards[T.CARD_ALIAS.get(c, c)]["bands"] for c in ("knight", "mage")))

    def test_missing_metric_is_graded_not_skipped(self):
        saved = T.play_metrics
        try:
            # start has no taunt sample, the new table has one far outside the knight's bound: graded against the bare bound
            T.play_metrics = lambda cls, curves: ({"contact%": (80.0, 0.1)} if curves == ["a"] else {"contact%": (80.0, 0.1), "taunt/min": (0.0, 0.1)})
            g = T.card_potion_gate("knight", ["a"], ["b"])
            self.assertTrue(any("taunt_per_min" in f for f in g["fails"]), g["fails"])
            # start present, new missing: FAIL
            T.play_metrics = lambda cls, curves: ({"contact%": (80.0, 0.1), "taunt/min": (3.0, 0.1)} if curves == ["a"] else {"contact%": (80.0, 0.1)})
            g = T.card_potion_gate("knight", ["a"], ["b"])
            self.assertTrue(any("taunt_per_min" in f and "no sample" in f for f in g["fails"]), g["fails"])
        finally:
            T.play_metrics = saved


class GapSearchG1(unittest.TestCase):   # review G1 / G2 of 7e883a9a
    def test_auto_gate_rejection_keeps_nothing(self):   # review F3 of b59b0a1a: decisions=auto, the card + potion gate rejects every value
        T.CFG = dict(T.SMOKE, force=False)
        T.A = types.SimpleNamespace(worlds=["builtin"], out=tempfile.mkdtemp(), decisions="auto")
        T.SUMMARY["stages"] = {"classes": {"knight": {"live": [["attack.x.w", 1.0]], "base": []}}}
        T.SUMMARY["open_decisions"] = []
        T.SUMMARY["auto_decisions"] = []
        names = ("all_knobs", "class_knobs", "final_curves", "jmany", "req", "paired", "judge", "widen", "log", "save_summary", "stage_joint", "gap_runs", "gap_merge", "card_potion_gate")
        saved = {k: getattr(T, k) for k in names}
        try:
            T.all_knobs = lambda: ([], [])
            T.class_knobs = lambda c, k: [("attack.x.w", "{c}:attack.x.w={v}", 1.0, [0.5, 2.0])]
            T.final_curves = lambda: []
            T.widen = lambda n, sh, tried, side: [4.0] if side == "up" else []
            T.req = lambda w, c, cfg, curves, seeds: tuple(curves)
            T.jmany = lambda reqs: list(reqs)
            gain = {(): 0.0, ("knight:attack.x.w=0.75",): 0.5, ("knight:attack.x.w=1.5",): 0.1, ("knight:attack.x.w=4.0",): 0.9}
            T.paired = lambda a, b: (gain[b] - gain[a], 0.05)
            T.judge = lambda c, base, spec, forced=False: {"accepted": True, "why": "ACCEPTED", "steps": {"holdout": [1, 0.1], "cross_world": {"mean/g3": [1, 0.1]}}}
            T.card_potion_gate = lambda cls, anchor, new: {"ok": False, "fails": ["card test: rejected"], "card": [], "potion": []}
            T.log = lambda m: None
            T.save_summary = lambda: None
            T.stage_joint = lambda: T.SUMMARY["stages"].__setitem__("joint", {"verdict": "PASS"})
            T.gap_runs = lambda tasks: []
            T.gap_merge = lambda parts: {"fights": [(1, 0, "nat", 1.0, 1.0)]}
            st = {"clusters": {"knight": {"rows": [{"class": "a", "families": ["attack"], "dU_per_fight": 1.0}]}}, "search": {}, "kept": {}, "cells": {"knight/builtin": {"seeds": 1}}}
            changed = T.gap_search(st, ["knight"])
        finally:
            for k, v in saved.items():
                setattr(T, k, v)
        self.assertEqual(st["kept"].get("knight", []), [])   # nothing the gate rejected is added to the kept set
        self.assertEqual(changed, [])
        self.assertTrue(all(not r["accepted"] for r in st["search"]["knight"]["rows"]))
        self.assertTrue(any(d["verdict"] == "rejected" for d in T.SUMMARY["auto_decisions"]))

    def test_in_range_judged_even_when_a_far_value_wins(self):
        T.CFG = dict(T.SMOKE, force=False)
        T.A = types.SimpleNamespace(worlds=["builtin"], out=tempfile.mkdtemp(), decisions="ask")
        T.SUMMARY["stages"] = {"classes": {"knight": {"live": [["attack.x.w", 1.0]]}}}
        T.SUMMARY["open_decisions"] = []
        judged = []
        saved = {k: getattr(T, k) for k in ("all_knobs", "class_knobs", "final_curves", "jmany", "req", "paired", "judge", "widen", "log", "save_summary", "stage_joint", "gap_runs", "gap_merge")}
        try:
            T.all_knobs = lambda: ([], [])
            T.class_knobs = lambda c, k: [("attack.x.w", "{c}:attack.x.w={v}", 1.0, [0.5, 2.0])]
            T.final_curves = lambda: []
            T.widen = lambda n, sh, tried, side: [4.0] if side == "up" else []
            T.req = lambda w, c, cfg, curves, seeds: tuple(curves)
            T.jmany = lambda reqs: list(reqs)
            gain = {(): 0.0, ("knight:attack.x.w=0.75",): 0.5, ("knight:attack.x.w=1.5",): 0.1, ("knight:attack.x.w=4.0",): 0.9}
            T.paired = lambda a, b: (gain[b] - gain[a], 0.05)
            T.judge = lambda c, base, spec, forced=False: (judged.append(spec), {"accepted": True, "why": "ACCEPTED", "steps": {"holdout": [1, 0.1], "cross_world": {"mean/g3": [1, 0.1]}}})[1]
            T.log = lambda m: None
            T.save_summary = lambda: None
            T.stage_joint = lambda: T.SUMMARY["stages"].__setitem__("joint", {"verdict": "PASS"})
            T.gap_runs = lambda tasks: []
            T.gap_merge = lambda parts: {"fights": [(1, 0, "nat", 1.0, 1.0)]}
            st = {"clusters": {"knight": {"rows": [{"class": "a", "families": ["attack"], "dU_per_fight": 1.0}]}}, "search": {}, "kept": {}, "cells": {"knight/builtin": {"seeds": 1}}}
            changed = T.gap_search(st, ["knight"])
        finally:
            for k, v in saved.items():
                setattr(T, k, v)
        self.assertEqual(judged, ["knight:attack.x.w=0.75", "knight:attack.x.w=4.0"])   # both groups judged, in-range first
        self.assertEqual(st["kept"], {"knight": ["knight:attack.x.w=0.75"]})            # only the in-range one is written
        self.assertEqual(changed, ["knight"])
        self.assertTrue(any("past the range tried" in d["what"] for d in T.SUMMARY["open_decisions"]))

    def test_gap_runs_before_the_table_readers(self):
        o = T.DEFAULT_STAGES.split(",")
        self.assertTrue(all(o.index("gap") < o.index(x) for x in ("floor", "rating", "recalib", "globals", "consts")))
        f = T.FOR_CLASS_STAGES.split(",")
        self.assertLess(f.index("gap"), f.index("floor"))


class Race(unittest.TestCase):   # tune 2.11.1: racing decides which candidates get the full seeds, never the bar
    def setUp(self):
        self.oldA, self.oldj = T.A, T.jmany
        T.A = types.SimpleNamespace(no_race=False)
        # candidate id in `sets`: J = base + effect + noise(seed); "b" = the base
        eff = {None: 0.0, "c0": 0.0, "c1": 1.0, "c2": -1.0, "c3": 0.9}
        noise = lambda s: ((s * 7919) % 13 - 6) * 0.05
        self.calls = []
        def jm(reqs):
            self.calls.append([(r[4], len(r[6])) for r in reqs])
            return [[(10.0 + eff[r[4]] + noise(s), 0, 1) for s in r[6]] for r in reqs]
        T.jmany = jm

    def tearDown(self):
        T.A, T.jmany = self.oldA, self.oldj

    def mk(self, sets):
        return T.req("builtin", "x", "g3", [], range(1, 65), sets)

    def test_losers_drop_early_winners_run_full(self):
        b, rr = T.race(self.mk(None), [self.mk("c0"), self.mk("c1"), self.mk("c2"), self.mk("c3")], range(1, 65))
        self.assertIsNone(rr[0][0]); self.assertEqual(rr[0][1], 16)       # no effect: under the 0.15 J bar at 3 sigma after 16 seeds
        self.assertIsNone(rr[2][0])                                        # a loss
        self.assertIsNotNone(rr[1][0]); self.assertEqual(len(rr[1][0]), 64)   # the leader is measured on every seed
        self.assertEqual(len(b), 64)

    def test_no_race_measures_everything(self):
        T.A.no_race = True
        b, rr = T.race(self.mk(None), [self.mk("c0"), self.mk("c1")], range(1, 65))
        self.assertTrue(all(r is not None and len(r) == 64 for r, _ in rr))
        self.assertEqual(len(self.calls), 1)


class NewTables(unittest.TestCase):   # tune 2.11.1: one knob = one native member
    def fields(self, path, struct):
        import re
        txt = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../native/iaus/" + path)).read()
        body = txt[txt.index("struct %s {" % struct):]
        body = body[:body.index("\n    };")]
        out = set()
        for m in re.finditer(r"^\s*(float|int)\s+([^;]+);", body, re.M):
            for part in m.group(2).split(","):
                out.add(part.split("=")[0].strip())
        return out

    def test_worth_knobs_cover_worthtable(self):
        self.assertEqual({n.split(".", 1)[1] for n, _, _ in T.WORTH_KNOBS}, self.fields("ConsumableWorth.h", "WorthTable"))

    def test_loot_knobs_cover_loottuning(self):
        names = {n.split(".", 1)[1] for n, _, _ in T.LOOT_KNOBS + T.LOOT_EPISODE_KNOBS} | {n.split(".", 1)[1] for n in T.LOOT_NOT_SEARCHED}
        self.assertEqual(names, self.fields("LootNeed.h", "LootTuning"))
        self.assertFalse({n for n, _, _ in T.LOOT_KNOBS + T.LOOT_EPISODE_KNOBS} & set(T.LOOT_NOT_SEARCHED))
        self.assertEqual([n for n, _, _ in T.LOOT_EPISODE_KNOBS], ["loot.batchHoldMinSec"])   # review of 9689734b (S2, S6): the guard timers, the bar floor and the gate rows are NOT searched
        for n in ("guardFreshSec", "guardUnreadMaxSec", "guardRepostSec", "guardBarFloorSec", "gateReadmitSec", "gateConeDeg", "gateBeyondU", "gateReadmitMoveU", "gateRepeatU", "gateShortU", "gateNearU"):
            self.assertIn("loot." + n, T.LOOT_NOT_SEARCHED)
        tune_h = open(os.path.join(os.path.dirname(os.path.abspath(T.__file__)), "Tune.h"), encoding="utf-8").read()
        for n, _, _ in T.LOOT_KNOBS + T.LOOT_EPISODE_KNOBS:
            self.assertIn('"%s"' % n, tune_h, n)   # every searched row is one `--set` reaches (Tune.h kLootKnobsF / kLootKnobsI)

    def test_loot_episode_command_and_key(self):
        d = tempfile.mkdtemp()
        T.A = types.SimpleNamespace(bin=os.path.join(d, "sim"), out=d, builtin_worth=None, loot_places="")
        open(T.A.bin, "w").write("x")
        T.CFG = dict(T.FULL)
        self.assertEqual(T.sim_cmd("builtin", "epi:melee,mage", "g3", [], "loot.batchHoldMinSec=6.0", None)[1:],
                         ["--no-json", "--threads", "1", "--loot-report", "6", "--loot-score", "--classes", "melee,mage", "--set", "loot.batchHoldMinSec=6.0"])
        k0 = T.key_of("builtin", "epi:melee", "g3", [], None, None)
        T.A.loot_places = "Tuxborn"
        cmd = T.sim_cmd("builtin", "epi:melee", "g3", [], None, None)
        self.assertEqual(cmd[cmd.index("--env-set") + 1], "Tuxborn")
        self.assertTrue(cmd[cmd.index("--loot-list") + 1].endswith("Tuxborn.loot.json"))
        self.assertEqual(T.loot_world_args(True)[1], "Tuxborn/heldout")
        self.assertNotEqual(k0, T.key_of("builtin", "epi:melee", "g3", [], None, None))   # another set of places is another cache key

    def test_loot_gate_check(self):
        notes = []
        T.open_decision = lambda what, why: notes.append((what, why))
        ship = {"gate": {"crimes": [0.0, False], "leftRR/ep": [1.0, True], "roadLeft%": [40.0, False]}}
        self.assertIsNone(T.loot_gate_check("train", ship, {"gate": {"crimes": [0.0, False], "leftRR/ep": [0.9, True], "roadLeft%": [40.0, False]}}))
        self.assertEqual(notes, [])
        self.assertIsNone(T.loot_gate_check("train", ship, {"gate": {"crimes": [0.0, False], "leftRR/ep": [0.2, False], "roadLeft%": [55.0, False]}}))   # worse, under its bound: flagged all the same
        self.assertEqual(len(notes), 1); self.assertIn("roadLeft%", notes[0][1])
        self.assertIn("crimes", T.loot_gate_check("heldout", ship, {"gate": {"crimes": [1.0, True], "leftRR/ep": [1.0, True], "roadLeft%": [40.0, False]}}))   # a hard row: rejected

    def test_values_in_range(self):
        shares = {"dangerSpan", "magickaWinLine", "staminaWinLine", "winNeed", "wealthFrac", "restoreCostScale", "restoreUseWeight", "floor", "foodMinGain",
                  "takeThreshold", "needBase", "needSlope", "roomKnee", "buffScale", "foodScale"}
        for n, sh, vals in T.WORTH_KNOBS + T.LOOT_KNOBS:
            f = n.split(".", 1)[1]
            self.assertNotIn(sh, vals, n)
            for v in vals:
                self.assertGreaterEqual(v, 0, n)
                if f in shares:
                    self.assertLessEqual(v, 1.0, n)

    def test_loot_command_and_key(self):
        d = tempfile.mkdtemp()
        T.A = types.SimpleNamespace(bin="/x/sim", out=d, builtin_worth=None)
        T.CFG = dict(T.FULL)
        open(T.uses_path(), "w").write("melee 0\n")
        open(T.A.bin if os.path.exists(T.A.bin) else os.path.join(d, "sim"), "w").write("x")
        T.A.bin = os.path.join(d, "sim")
        cmd = T.sim_cmd("builtin", "logi:melee,mage", "g3", [], "loot.healthTarget=8", None)
        self.assertEqual(cmd[1:], ["--no-json", "--logistics", "8", "--logi-policy", "iaus", "--logi-score", "--logi-consumables", "--logi-uses", T.uses_path(), "--classes", "melee,mage", "--set", "loot.healthTarget=8"])
        self.assertIn("uses:", T.key_of("builtin", "logi:melee", "g3", [], None, None))
        self.assertNotIn("uses:", T.key_of("builtin", "melee", "g3", [], None, None))


class ShipState(unittest.TestCase):   # backlog 2026-11-19: needs_joint_rerun stayed set after a passing joint on the reduced table
    def setUp(self):
        self.saved = dict(T.SUMMARY)
        T.SUMMARY.clear()
        T.SUMMARY.update({"stages": {"classes": {"mercenary": {"kept": ["mercenary:0.7.a=0.931", "mercenary:family.0=1.2"]}, "knight": {"kept": ["knight:0.8.a=1.0"]}}}, "open_decisions": [], "auto_decisions": []})
        self.full = ["mercenary:0.7.a=0.931", "mercenary:family.0=1.2", "knight:0.8.a=1.0"]

    def tearDown(self):
        T.SUMMARY.clear()
        T.SUMMARY.update(self.saved)

    def test_joint_passed_on_the_full_table_ships_it(self):
        T.SUMMARY["stages"]["joint"] = {"verdict": "PASS", "table": T.final_curves()}
        self.assertEqual(T.final_curves(), self.full)
        self.assertEqual(T.ship_state()["needs_joint_rerun"], "")
        self.assertEqual(T.ship_state()["table"], self.full)

    def test_a_withheld_value_asks_for_the_joint_once_and_a_passing_rerun_clears_it(self):
        T.SUMMARY["stages"]["joint"] = {"verdict": "PASS", "table": list(self.full)}
        T.SUMMARY["withheld"] = ["mercenary:0.7.a=0.931"]              # recalib ran after the joint and withheld one value
        reduced = [k for k in self.full if k != "mercenary:0.7.a=0.931"]
        self.assertEqual(T.final_curves(), reduced)                     # every reader of the final table sees the reduced one
        self.assertEqual(T.final_curves(with_withheld=True), self.full)
        sh = T.ship_state()
        self.assertIn("recalib withheld 1 value(s)", sh["needs_joint_rerun"])
        self.assertEqual(sh["table"], [])
        T.SUMMARY["stages"]["joint"] = {"verdict": "PASS", "table": T.final_curves()}   # the joint stage run again grades final_curves()
        sh = T.ship_state()
        self.assertEqual(sh["needs_joint_rerun"], "")                   # ... and the flag clears, with the value still withheld
        self.assertEqual(sh["table"], reduced)
        self.assertEqual(sh["withheld"], ["mercenary:0.7.a=0.931"])

    def test_a_failed_or_missing_joint_ships_nothing(self):
        self.assertIn("not run", T.ship_state()["needs_joint_rerun"])
        T.SUMMARY["stages"]["joint"] = {"verdict": "FAIL: x", "table": list(self.full)}
        self.assertEqual(T.ship_state()["table"], [])
        T.SUMMARY["stages"]["joint"] = {"verdict": "PASS", "table": self.full[:1]}   # the gap search moved the table after the joint
        self.assertIn("different table", T.ship_state()["needs_joint_rerun"])


class OneValuePerKnob(unittest.TestCase):   # Opus review of 1874f81e: the default rejudge spec (warrior:6.32.b) is also a classes knob
    def setUp(self):
        self.saved = dict(T.SUMMARY)

    def tearDown(self):
        T.SUMMARY.clear()
        T.SUMMARY.update(self.saved)

    def test_the_classes_value_replaces_the_rejudged_one(self):
        T.SUMMARY.clear()
        T.SUMMARY.update({"stages": {"rejudge": [{"spec": "warrior:6.32.b=0.15", "accepted": True}, {"spec": "knight:0.8.a=1.1", "accepted": True}, {"spec": "mage:1.2.a=3", "accepted": False}],
                                     "classes": {"warrior": {"kept": ["warrior:6.32.b=0.2", "warrior:family.0=1.33"]}},
                                     "gap": {"kept": {"warrior": ["warrior:family.0=1.5", "warrior:0.9.floor=0.6"]}}}})
        self.assertEqual(T.final_curves(), ["warrior:6.32.b=0.2", "warrior:family.0=1.33", "knight:0.8.a=1.1", "warrior:0.9.floor=0.6"])
        self.assertEqual(len({k.split("=")[0] for k in T.final_curves()}), len(T.final_curves()))


class RerunAfter(unittest.TestCase):   # the joint gate runs again, in the same run, when recalib withheld a value after it
    def setUp(self):
        self.saved = dict(T.SUMMARY)
        T.SUMMARY.clear()
        T.SUMMARY.update({"stages": {}, "withheld": []})

    def tearDown(self):
        T.SUMMARY.clear()
        T.SUMMARY.update(self.saved)

    def test_joint_again_after_a_withheld_value(self):
        self.assertEqual(T.rerun_after("recalib", ["classes", "joint", "recalib"], ["logistics", "summary"])[0], [])   # nothing withheld
        T.SUMMARY["withheld"] = ["mercenary:0.7.a=0.931"]
        self.assertEqual(T.rerun_after("recalib", ["classes", "joint", "recalib"], ["logistics", "summary"])[0], ["joint"])
        self.assertEqual(T.rerun_after("recalib", ["classes", "recalib"], ["joint", "summary"])[0], [])                # joint is still to come
        self.assertEqual(T.rerun_after("recalib", ["classes", "recalib"], ["summary"])[0], [])                         # a run without the joint stage
        self.assertEqual(T.rerun_after("joint", ["classes", "joint", "recalib", "joint"], ["summary"])[0], [])          # and it does not loop

    def test_gap_rule_unchanged(self):
        T.SUMMARY["stages"]["gap"] = {"search_changed": True}
        self.assertEqual(T.rerun_after("gap", ["floor", "rating", "gap"], ["summary"])[0], ["floor", "rating"])


class CarryOverBinary(unittest.TestCase):   # a summary made by another simulator binary is not carried over
    def setUp(self):
        self.saved = dict(T.SUMMARY)

    def tearDown(self):
        T.SUMMARY.clear()
        T.SUMMARY.update(self.saved)

    def old(self, sha):
        cfg = {"cells": "15"}
        if sha:
            cfg["bin_sha"] = sha
        return {"config": cfg, "stages": {"classes": {"knight": {"kept": ["knight:0.8.a=1.1"]}}, "joint": {"verdict": "PASS", "table": ["knight:0.8.a=1.1"]}}, "withheld": []}

    def test_another_binary_carries_nothing(self):
        T.SUMMARY.clear()
        T.SUMMARY.update({"stages": {}, "open_decisions": [], "auto_decisions": []})
        lines = T.carry_over(self.old("aaaaaaaaaaaaaaaa"), "15", "bbbbbbbbbbbbbbbb")
        self.assertEqual(len(lines), 1)
        self.assertIn("another simulator binary", lines[0])
        self.assertEqual(T.SUMMARY["stages"], {})

    def test_the_same_binary_carries_everything_and_an_unrecorded_one_warns(self):
        T.SUMMARY.clear()
        T.SUMMARY.update({"stages": {}, "open_decisions": [], "auto_decisions": []})
        self.assertEqual(T.carry_over(self.old("aaaaaaaaaaaaaaaa"), "15", "aaaaaaaaaaaaaaaa"), [])
        self.assertIn("joint", T.SUMMARY["stages"])
        lines = T.carry_over(self.old(None), "15", "aaaaaaaaaaaaaaaa")
        self.assertEqual(len(lines), 1)
        self.assertIn("UNCHECKED", lines[0])
        self.assertIn("classes", T.SUMMARY["stages"])


class FieldHits(unittest.TestCase):   # calib_damage.py HITS: the line's own counts are read, a window is classified by what it holds
    def test_line_counts_and_kinds(self):
        import calib_damage as C
        d = tempfile.mkdtemp()
        L = ("[19:44:%s] [info] [iaus-fill] hit %s (%s) -> Cicero (0009BCB0) source %s projectile 00000000 power=%s bash=false blocked=false sneak=false | health before %s at-event nan "
             "after %s: took %s in 0.13 s, %d other hit(s) on him in it | estimate 10.0 (hit 20.0 for weapon 00013841, armor 0.500, magic resist 0) ratio 1.0 | attacker attack state 0 | sink thread 1 | lines skipped %d hits aged out 0 overflow 0")
        rows = ["[19:44:00.000] [info] [sense] 0009BCB0: own=1 foes=2 load=1.0 hpLoss=0.0%/s",
                L % ("01.000", "Archer", "00091447", "00013841", "false", "400.0", "380.0", "20.0", 0, 0),      # plain: 20 over an estimate of 10
                L % ("01.000", "Archer", "00091447", "00013841", "false", "400.0", "380.0", "20.0", 0, 3),      # the engine's second event of the same blow
                L % ("02.000", "Archer", "00091447", "00013841", "false", "380.0", "340.0", "40.0", 0, 0),      # plain: 40 over 10
                L % ("03.000", "Archer", "00091447", "00013841", "true", "340.0", "300.0", "40.0", 0, 0),       # a power attack
                L % ("04.000", "Archer", "00091447", "00013841", "false", "300.0", "250.0", "50.0", 1, 0),      # a blow ...
                L % ("04.000", "Archer", "00091447", "FE000841", "false", "300.0", "250.0", "50.0", 1, 0),      # ... with a rider
                L % ("05.000", "Archer", "00091447", "00013841", "false", "250.0", "249.0", "1.0", 4, 0)]       # late: 1 is under a quarter of 10
        open(os.path.join(d, "MFO.log"), "w").write("\n".join(rows) + "\n")
        evs, fol, key = C.parse_hits(os.path.join(d, "MFO.log"))
        self.assertEqual(len(evs), 6)                                           # the duplicate is one event
        self.assertEqual(sum(e["skipped"] for e in evs), 3)                     # the duplicate line's "lines skipped" still counts
        self.assertEqual(sorted(e["other"] for e in evs), [0, 0, 0, 1, 1, 4])
        kinds = []
        by = {}
        for e in evs:
            by.setdefault((e["before"], e["after"]), []).append(e)
        for es in by.values():
            kinds.append(C.classify_window(es, es[0]["took"])[0])
        self.assertEqual(sorted(kinds), ["late", "plain", "plain", "power", "rider"])
        import io
        import contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            C.hits_report([d])
        out = buf.getvalue()
        self.assertIn("kFieldHit[2] = { 0.67f, 1.33f };", out)                   # 2.0 and 4.0 times the estimate, over their mean 3.0
        self.assertIn("FIELD HIT KIND multi     n=0", out)
        self.assertIn("FIELD HIT KIND rider     n=1", out)
        self.assertIn("1 window(s) say they hold more events than the log has lines for", out)   # the late one: 4 others, one line (the rider window: 1 other, two lines)


class CellsText(unittest.TestCase):   # review of f26dac4a: "15 cells" text under --cells all
    def test_the_rule_does_not_say_15_cells(self):
        self.assertIsNone(re.search(r"15 cells|15-cell|15-at-once", T.SUMMARY["rule"]))
        self.assertIn("38 under --cells all", T.__doc__)


if __name__ == "__main__":
    unittest.main()
