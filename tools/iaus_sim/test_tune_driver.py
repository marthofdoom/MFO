#!/usr/bin/env python3
"""Unit tests of tune_driver.py's pure helpers (no simulator needed). Run: python3 -I tools/iaus_sim/test_tune_driver.py"""
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
        self.assertEqual(T.classify("attack", "ext:bash", "ext", {"bash": 5})[0], "d")          # FIDELITY wins
        self.assertEqual(T.classify("attack", "ext:to_player", "ext", {"regroup": 0})[0], "c")  # a family never offered
        self.assertEqual(T.classify("attack", "ext:hide", "ext", {"disengage": 3})[0], "b")     # offered elsewhere, gated here
        self.assertEqual(T.classify("attack", "ext:get_sight", "ext", {})[0], "c")              # no family at all
        self.assertEqual(T.classify("attack!", "taunt", "ranked", {})[0], "b")                  # IAUS broke a class ruling
        self.assertEqual(T.classify("attack", "drink_health", "unranked", {})[0], "b")
        c, _, fams = T.classify("attack", "taunt", "ranked", {})
        self.assertEqual((c, fams), ("a", ["attack", "taunt"]))
        self.assertEqual(T.classify("power_attack", "idle", "idle", {})[2], ["power_attack"])

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
        names = {n.split(".", 1)[1] for n, _, _ in T.LOOT_KNOBS} | {n.split(".", 1)[1] for n in T.LOOT_NOT_SEARCHED}
        self.assertEqual(names, self.fields("LootNeed.h", "LootTuning"))
        self.assertFalse({n for n, _, _ in T.LOOT_KNOBS} & set(T.LOOT_NOT_SEARCHED))

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


if __name__ == "__main__":
    unittest.main()
