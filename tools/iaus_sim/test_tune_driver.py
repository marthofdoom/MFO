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


class World(unittest.TestCase):   # the driver always names the world (v4 by default since sim v4 batch C; --world v3 and v2 still work)
    def test_world_is_always_first(self):
        self.assertEqual(T.world_flags("v2", ""), ["--world", "v2"])
        self.assertEqual(T.world_flags("v3", "--ranged-perks --point-blank"), ["--world", "v3", "--ranged-perks", "--point-blank"])

    def test_sim_flags_may_not_name_the_world(self):
        with self.assertRaises(SystemExit):
            T.world_flags("v2", "--world v3")

    def test_v4_is_the_train_places_with_field_books_and_sim_flags_come_last(self):   # sim v4 batch C: the tune's world
        self.assertEqual(T.world_flags("v4", ""), ["--world", "v4", "--env-set", "all", "--env-hold", "train", "--books", "field"])
        self.assertEqual(T.world_flags("v4", "--books lab")[-2:], ["--books", "lab"])   # the simulator takes the last of a repeated switch
        self.assertEqual(T.HOLD_FLAGS, ["--env-hold", "heldout", "--books", "heldout"])
        self.assertEqual(T.DEFAULT_STAGES.split(",")[2:8], ["classes", "edge", "follow", "spells", "worth", "joint"])   # the gap is the score BEFORE the joint gate judges the table


class ChoiceBatch(unittest.TestCase):   # sim v4 batch C (iaus-sim-v4.md 15): the follow stage, the spells stage, their readers
    NAMES = ("all_knobs", "class_knobs", "final_curves", "follow_oracle", "follow_iaus", "judge", "card_potion_gate", "log", "save_summary", "regret_runs", "spell_knobs", "scope", "cells", "gapi_runs")

    def setUp(self):
        self.saved = {k: getattr(T, k) for k in self.NAMES}
        self.cfg, self.a, self.stages = T.CFG, T.A, T.SUMMARY.get("stages")
        T.CFG = dict(T.FULL, follow_seeds=2, gap_max_seeds=2, spell_seeds=2, spell_rounds=3)
        T.A = types.SimpleNamespace(worlds=["builtin"], out=tempfile.mkdtemp(), decisions="auto", classes=None)
        T.SUMMARY["stages"] = {}
        T.SUMMARY["open_decisions"], T.SUMMARY["auto_decisions"] = [], []
        T.log = lambda m: None
        T.save_summary = lambda: None
        # the set checks (choice review R3): the IAUS arm on held-out places, seeds 7601... setgain: curve spec or --set string -> U it adds there
        self.setgain, self.settasks = {}, []

        def gapi(tasks):
            self.settasks += tasks
            return [{(s, j): 50.0 + 0.1 * j + sum(self.setgain.get(k, 0.0) for k in table) + self.setgain.get(sets, 0.0) for j in range(4)} for w, c, table, s, sets, hold in tasks]
        T.gapi_runs = gapi

    def test_the_held_out_set_check(self):
        T.cells = lambda: ["evoker", "knight"]
        self.setgain = {"evoker:9.1.a=2.0": 0.5, "t.x=1": -2.0}
        ho = T.held_out_check([], ["evoker:9.1.a=2.0"], ["evoker", "knight"])
        self.assertTrue(ho["ok"])
        self.assertEqual(ho["pooled"][0], 0.5)
        self.assertEqual(ho["seeds"], [T.HOLD_FINAL0, T.HOLD_FINAL0 + 1])                  # seeds nothing else reads
        self.assertTrue(all(t[5] for t in self.settasks))                                  # every run on the held-out places and books
        self.assertGreater(T.HOLD_FINAL0, T.FOLLOW_FRESH0 + T.FULL["gap_max_seeds"])
        ho = T.held_out_check([], [], ["evoker", "knight"], None, "t.x=1")                 # a shared table alone can fail it
        self.assertFalse(ho["ok"])
        self.assertIn("-2.00", ho["why"])
        self.setgain, good = {}, T.gapi_runs                                               # driver E1: an empty or partial pairing is not a pass
        def drop(pick):
            def runs(tasks):
                return [({} if pick(t) == "all" else {k: v for k, v in x.items() if not (pick(t) and k[1] == 0)}) for t, x in zip(tasks, good(tasks))]
            return runs
        for pick in (lambda t: "all", lambda t: "all" if t[1] == "knight" else False, lambda t: "all" if t[1] == "knight" and t[2] else False, lambda t: t[1] == "knight" and bool(t[2]), lambda t: t[1] == "knight" and not t[2]):
            T.gapi_runs = drop(pick)
            ho = T.held_out_check([], ["evoker:9.1.a=2.0"], ["evoker", "knight"])
            self.assertFalse(ho["ok"])
            self.assertIn("no complete pairing", ho["why"])
            self.assertIn("knight/builtin", ho["why"])
        T.gapi_runs = good
        self.assertTrue(T.held_out_check([], ["evoker:9.1.a=2.0"], ["evoker", "knight"])["ok"])
        self.assertFalse(T.held_out_check([], [], [])["ok"])                               # nothing to check is not a pass
        T.A.worlds = ["builtin", "Tuxborn"]
        T.gapi_runs = lambda tasks: [({} if t[0] == "Tuxborn" else x) for t, x in zip(tasks, good(tasks))]
        self.assertIn("knight/Tuxborn", T.held_out_check([], [], ["knight"])["why"])       # one world of a cell empty

    def test_joint_grades_the_whole_table_and_fails_on_the_held_out_check(self):   # choice review R2, R3
        saved = {k: getattr(T, k) for k in ("jmany", "held_out_check", "final_sets")}
        try:
            reqs = []
            T.cells = lambda: ["knight"]
            T.final_curves = lambda: ["knight:1.1.a=2"]
            T.final_sets = lambda: "t.x=1"
            T.jmany = lambda rs: (reqs.extend(rs), [[(70.0, 0, 0.0)] * len(r[6]) for r in rs])[1]
            gates = []
            T.card_potion_gate = lambda cls, anchor, new, new_sets=None: (gates.append(new_sets), {"ok": True, "fails": []})[1]
            calls = []
            T.held_out_check = lambda a, b, who, a_sets=None, b_sets=None: (calls.append((a, b, who, a_sets, b_sets)), {"ok": False, "why": "the 1-cell mean is -1.00 U (se 0.20)", "pooled": [-1.0, 0.2], "cells": {}, "seeds": [7601, 7606]})[1]
            T.stage_joint()
            jo = T.SUMMARY["stages"]["joint"]
            self.assertEqual({(r[3], r[4]) for r in reqs}, {((), None), (("knight:1.1.a=2",), "t.x=1")})   # shipped: no row, no shared value. final: both
            self.assertEqual(calls, [([], ["knight:1.1.a=2"], ["knight"], None, "t.x=1")])
            self.assertEqual(gates, ["t.x=1"])                                               # the class card reads the table that ships
            self.assertTrue(jo["verdict"].startswith("FAIL") and "held-out" in jo["verdict"], jo["verdict"])
            T.final_curves = lambda: []                                                      # a shared value alone is a table to gate
            T.held_out_check = lambda a, b, who, a_sets=None, b_sets=None: {"ok": True, "why": "", "pooled": [0.0, 0.1], "cells": {}, "seeds": [7601, 7606]}
            T.stage_joint()
            self.assertEqual(T.SUMMARY["stages"]["joint"]["verdict"], "PASS")
        finally:
            for k, v in saved.items():
                setattr(T, k, v)

    def test_follow_drops_a_set_that_is_worse_held_out(self):
        self.plant({"evoker:9.1.a=2.0": 1.5})
        self.setgain = {"evoker:9.1.a=2.0": -3.0}
        T.stage_follow()
        fo = T.SUMMARY["stages"]["follow"]
        self.assertEqual(fo["kept"]["evoker"], [])
        self.assertEqual(T.final_curves(), [])
        self.assertIn("held-out", fo["cells"]["evoker"]["verdict"])
        self.assertNotIn("gap_after", fo["cells"]["evoker"])
        self.assertFalse(any(r["accepted"] for r in fo["rows"]["evoker"]))
        self.assertEqual([d["kind"] for d in T.SUMMARY["auto_decisions"]], ["held-out set"])

    def test_spells_drops_a_set_that_is_worse_held_out(self):
        self.spell_world({"t.unmeasuredDamage=0.25": 1.0}, {"t.unmeasuredDamage=0.25": 1.0})
        self.setgain = {"t.unmeasuredDamage=0.25": -3.0}
        T.stage_spells()
        sp = T.SUMMARY["stages"]["spells"]
        self.assertEqual(sp["kept"], [])
        self.assertNotIn("report_final", sp)
        self.assertEqual(T.spell_sets(), [])
        self.assertFalse(sp["held_out_set"]["ok"])

    def tearDown(self):
        for k, v in self.saved.items():
            setattr(T, k, v)
        T.CFG, T.A = self.cfg, self.a
        T.SUMMARY["stages"] = self.stages if self.stages is not None else {}

    def test_table_slice_is_the_class_and_its_t1_companions(self):   # cost cuts 3 and 4
        tab = ["mage:9.1.a=2", "evoker:9.2.w=1.5", "healer:11.3.b=0.2", "melee:family.0=1.15"]
        self.assertEqual(T.table_slice("evoker", tab), ["mage:9.1.a=2", "evoker:9.2.w=1.5", "melee:family.0=1.15"])
        self.assertEqual(T.table_slice("knight", tab), ["mage:9.1.a=2", "melee:family.0=1.15"])
        T.CFG = dict(T.SMOKE)
        T.A.bin, T.A.builtin_worth = "/sim", None
        a, b = T.gap_cmd("builtin", "knight", tab, 6001), T.gap_cmd("builtin", "knight", tab[:1] + tab[3:], 6001)
        self.assertEqual(a, b)                                      # a value of another class is not in the command, so not in the cache key
        self.assertIn("--spell-regret", a)                          # every oracle run prints the regret: one cache entry for gap, follow and spells
        i = T.gap_cmd("builtin", "knight", tab, 6001, arm="iaus")
        self.assertTrue("--gap-arm" in i and "--spell-regret" not in i)
        # the race's cost cuts ride on every oracle run and on no IAUS arm, and one switch takes them off (iaus-sim-v4.md 15.7)
        self.assertEqual(a[a.index("--oracle-pair") + 1], "0.10")
        self.assertTrue("--oracle-nmin" in a and a[a.index("--oracle-prune") + 1] == "target" and not any(x.startswith("--oracle-p") or x == "--oracle-nmin" for x in i))
        T.A.oracle_cuts = ""
        self.assertFalse(any(x in T.gap_cmd("builtin", "knight", tab, 6001) for x in ("--oracle-pair", "--oracle-nmin", "--oracle-prune")))
        del T.A.oracle_cuts

    def plant(self, gain, fresh_gain=None, hold_gain=None, gap_after=None):
        """A class with a gap of 4 U on 8 fights; candidate spec -> what it adds to IAUS's U on the search fights (gain), on fresh fights and
        on the held-out ones; the oracle run again on the new table reads gap_after (default: the gap less the gain)."""
        keys = [(s, j) for s in (6001, 6002) for j in range(4)]
        noise = [0.0, 0.2, -0.2, 0.1, -0.1, 0.3, -0.3, 0.0]
        T.all_knobs = lambda: ([], 0)
        T.class_knobs = lambda c, k: [("cast_offense.damage.a", "{c}:9.1.a={v}", 1.0, [0.5, 2.0])]
        T.final_curves = lambda: list(T.SUMMARY["stages"].get("follow", {}).get("kept", {}).get("evoker", []))
        T.SUMMARY["stages"]["classes"] = {"evoker": {"base": [], "live": [["cast_offense.damage.a", 1.0]], "rows": [{"knob": "cast_offense.damage.a", "sweep": [[0.5, 0.4, 0.1], [2.0, 0.5, 0.1]]}]}}
        fg, hg = fresh_gain if fresh_gain is not None else gain, hold_gain if hold_gain is not None else gain
        add = lambda table, g: sum(g.get(k, 0.0) for k in table)

        def oracle(cls, table, seeds, hold=False):
            off = 0 if seeds[0] == 6001 else 1000
            ga = gap_after if (gap_after is not None and table) else 4.0 - add(table, gain)
            return {"builtin": {"fights": [(s + off, j, "nat", 50.0 + n + add(table, fg if off else gain), 50.0 + n + add(table, fg if off else gain) + ga) for (s, j), n in zip(keys, noise)],
                                "sit": {"cast_offense|cast_offense|ranked|x": (8, 16.0)}, "gen": {}, "dec": 8}}
        T.follow_oracle = oracle
        T.follow_iaus = lambda cls, table, seeds, hold=False: {"builtin": {(s + (0 if seeds[0] == 6001 else 1000), j): 50.0 + n + add(table, hg if hold else (gain if seeds[0] == 6001 else fg)) for (s, j), n in zip(keys, noise)}}
        T.judge = lambda cls, kept, spec, forced=False, fence=False: {"accepted": fence, "why": "FENCE HOLDS" if fence else "strict judge called", "steps": {}}
        T.card_potion_gate = lambda cls, anchor, new, new_sets=None: {"ok": True, "fails": []}
        T.scope = lambda: ["evoker"]

    def test_follow_accepts_a_planted_improvement_and_rejects_a_planted_regression(self):   # design 6.4
        self.plant({"evoker:9.1.a=2.0": 1.5, "evoker:9.1.a=0.5": -1.5})
        T.stage_follow()
        fo = T.SUMMARY["stages"]["follow"]
        self.assertEqual(fo["kept"]["evoker"], ["evoker:9.1.a=2.0"])
        self.assertEqual(fo["cells"]["evoker"]["gap"], 4.0)
        self.assertEqual(fo["cells"]["evoker"]["gap_after"][0], 2.5)
        self.assertEqual(T.final_curves(), ["evoker:9.1.a=2.0"])                       # the one reader of the table sees it
        self.assertEqual(fo["changed"], ["evoker"])

    def test_follow_needs_fresh_fights_held_out_places_and_a_gap_that_really_fell(self):
        for kw, why in (({"fresh_gain": {"evoker:9.1.a=2.0": 0.0}}, "fresh gap fights"), ({"hold_gain": {"evoker:9.1.a=2.0": -3.0}}, "held-out"), ({"gap_after": 6.0}, "the gap ROSE")):
            T.SUMMARY["stages"] = {}
            self.plant({"evoker:9.1.a=2.0": 1.5}, **kw)
            T.stage_follow()
            fo = T.SUMMARY["stages"]["follow"]
            self.assertEqual(fo["kept"]["evoker"], [], why)
            self.assertTrue(any(why in r["why"] for r in fo["rows"]["evoker"]), (why, [r["why"] for r in fo["rows"]["evoker"]]))

    def test_follow_does_not_search_a_class_with_no_gap(self):
        self.plant({"evoker:9.1.a=2.0": 1.5}, gap_after=None)
        orc = T.follow_oracle
        T.follow_oracle = lambda cls, table, seeds, hold=False: {w: dict(m, fights=[(s, j, k, ui, ui + 0.05) for s, j, k, ui, _ in m["fights"]]) for w, m in orc(cls, table, seeds, hold).items()}
        T.stage_follow()
        self.assertIn("not searched", T.SUMMARY["stages"]["follow"]["cells"]["evoker"]["verdict"])

    def test_rerun_and_final_sets(self):
        T.SUMMARY["stages"] = {"follow": {"changed": ["evoker"]}, "spells": {"kept": ["t.unmeasuredDamage=0.35"]}, "worth": {"kept": ["worth.floor=0.1"]}}
        self.assertEqual(T.rerun_after("follow", ["joint", "follow"], ["summary"])[0], ["joint"])   # a custom order: the joint gate graded the older table
        self.assertEqual(T.rerun_after("follow", ["classes", "follow"], ["joint"])[0], [])          # the default order: the joint gate comes after
        self.assertEqual(T.final_sets(), "t.unmeasuredDamage=0.35,worth.floor=0.1")

    def spell_world(self, alt_regret, table_regret, hold_regret=None):
        """Two casters and a blade; base regret 2 U a fight (with a little spread); each alternate spec -> its screened regret; each accepted
        table (--set string) -> its true regret."""
        spread = [0.0, 0.1, -0.1, 0.05]

        def runs(tasks):
            out = []
            for w, c, curves, s, sets, alts, hold in tasks:
                cast = c != "knight"
                base = table_regret.get(sets, 2.0) if sets else 2.0
                out.append({"fights": {(s, j): (5 if cast else 0, (base + spread[j]) if cast else 0.0, [((hold_regret if hold and hold_regret else alt_regret).get(a, 2.0) + spread[j]) if cast else 0.0 for a in alts]) for j in range(4)},
                            "rows": {"cast_offense|cast_offense|fact": [20, 8, 4 * base, 20, 5.0, 6.0, 4.0]} if cast else {}, "dec": [30, 20 if cast else 0, 3]})
            return out
        T.regret_runs = runs
        T.spell_knobs = lambda: [("t.unmeasuredDamage", 0.5, [0.25, 0.8]), ("aw.9", 1.769, [0.9, 3.5])]
        T.final_curves = lambda: []
        T.scope = lambda: ["evoker", "healer", "knight"]
        self.judged = []
        T.judge = lambda cls, kept, spec, forced=False, sets=None, base_sets=None, fence=False: (self.judged.append((cls, sets, base_sets, fence)), {"accepted": True, "why": "FENCE HOLDS", "steps": {}})[1]

    def test_spells_accepts_a_planted_improvement_once_and_reports(self):
        self.spell_world({"t.unmeasuredDamage=0.25": 1.0, "aw.9=3.5": 3.0}, {"t.unmeasuredDamage=0.25": 1.0})
        T.stage_spells()
        sp = T.SUMMARY["stages"]["spells"]
        self.assertEqual(sp["casters"], ["evoker", "healer"])                             # a cell with no cast decision is left out
        self.assertEqual(sp["kept"], ["t.unmeasuredDamage=0.25"])
        self.assertEqual(self.judged[0], ("beta1", "t.unmeasuredDamage=0.25", None, True))   # J is the fence, over every cell
        self.assertEqual(sp["report"]["evoker/builtin"]["cause_pct"]["fact"], 100.0)
        self.assertEqual(sp["report"]["evoker/builtin"]["regret_u"], 2.0)
        self.assertEqual(sp["report_final"]["evoker/builtin"]["regret_u"], 1.0)
        self.assertFalse(sp["rows"][-1]["accepted"])                                      # the next round found nothing more, and says so

    def test_spells_rejects_a_screen_winner_that_fails_as_the_table_or_held_out(self):
        self.spell_world({"t.unmeasuredDamage=0.25": 1.0}, {"t.unmeasuredDamage=0.25": 2.0})          # the screen flattered it
        T.stage_spells()
        self.assertEqual(T.SUMMARY["stages"]["spells"]["kept"], [])
        self.assertIn("fresh fights", T.SUMMARY["stages"]["spells"]["rows"][0]["why"])
        T.SUMMARY["stages"] = {}
        self.spell_world({"t.unmeasuredDamage=0.25": 1.0}, {"t.unmeasuredDamage=0.25": 1.0}, {"t.unmeasuredDamage=0.25": 5.0})
        T.stage_spells()
        self.assertEqual(T.SUMMARY["stages"]["spells"]["kept"], [])
        self.assertIn("held-out", T.SUMMARY["stages"]["spells"]["rows"][0]["why"])

    def test_the_regret_readers(self):
        T.CFG = dict(T.SMOKE)
        txt = "SPELLFIGHT evoker nat 0 3 1.5000 1.0000 2.0000\nSPELLDEC evoker 12 3 40\nSPELLREG evoker cast_offense cast_offense fact 2 1 1.0000 2 0.5000 0.6000 0.2000\nSPELLREG evoker cast_control! cast_offense gate 1 1 0.5000 0 0.0000 0.0000 0.0000\nGAPIAUS evoker nat 0 71.2500\n"
        r = T.regret_parse(txt, "evoker", 6001, 2)
        self.assertEqual(r["fights"][(6001, 0)], (3, 1.5, [1.0, 2.0]))
        t = T.regret_table([r])
        self.assertEqual((t["regret_u"], t["within"], t["cross"], t["any_pct"]), (1.5, 1.0, 0.5, 66.7))
        self.assertEqual((t["cause_pct"]["fact"], t["cause_pct"]["gate"]), (66.7, 33.3))
        self.assertEqual(T.gapi_parse(txt, "evoker", 6001), {(6001, 0): 71.25})
        with self.assertRaises(RuntimeError):
            T.regret_parse(txt, "evoker", 6001, 1)                # another number of alternates than asked: a mixed-up run, never read
        with self.assertRaises(RuntimeError):
            T.regret_parse("", "evoker", 6001)                    # a simulator without --spell-regret is an error, never a regret of 0
        with self.assertRaises(RuntimeError):
            T.gapi_parse("", "evoker", 6001)

    def test_spell_worth_rows_cover_the_native_table(self):   # one knob = one native member (the NewTables rule), searched or listed with its reason
        fields = NewTables.fields(None, "SpellWorthTable.h", "SpellWorth")
        self.assertEqual({n.split(".", 1)[1] for n, _, _ in T.SW_KNOBS} | {n.split(".", 1)[1] for n in T.SW_NOT_SEARCHED}, fields)
        self.assertFalse({n for n, _, _ in T.SW_KNOBS} & set(T.SW_NOT_SEARCHED))
        cpp = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../native/iaus/SpellWorth.cpp")).read()
        self.assertEqual(set(re.findall(r'"sw\.(\w+)"', cpp)), fields)                # Set / Get name every field
        self.assertIn("SetSpellWorthKnob", open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "Tune.h")).read())   # --set sw.* reaches native's names
        self.assertIn("t.unmeasuredDamage", open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "Tune.h")).read())

    def test_the_fence_asks_j_not_to_fall_where_the_rule_asks_it_to_rise(self):
        names = ("jmany", "req", "paired", "cells", "out_of_band")
        saved = {k: getattr(T, k) for k in names}
        try:
            T.req = lambda w, c, g, curves, seeds, sets=None, binary=None: "new" if "evoker:9.1.a=2.0" in curves else "base"
            T.jmany = lambda reqs: list(reqs)
            T.paired = lambda a, b: (0.01, 0.05) if (a, b) == ("base", "new") else (0.0, 0.05)   # J does not move
            T.out_of_band = lambda c, d, se, k=1.0: False
            strict = T.judge("evoker", [], "evoker:9.1.a=2.0")
            fence = T.judge("evoker", [], "evoker:9.1.a=2.0", fence=True)
            T.paired = lambda a, b: (-1.0, 0.05)                                               # J falls
            fallen = T.judge("evoker", [], "evoker:9.1.a=2.0", fence=True)
        finally:
            for k, v in saved.items():
                setattr(T, k, v)
        self.assertEqual((strict["accepted"], strict["why"]), (False, "holdout: not confirmed"))
        self.assertTrue(fence["accepted"])
        self.assertEqual((fallen["accepted"], fallen["why"]), (False, "holdout: worse"))


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
            T.play_metrics = lambda cls, curves, sets=None: ({"contact%": (80.0, 0.1)} if curves == ["a"] else {"contact%": (80.0, 0.1), "taunt/min": (0.0, 0.1)})
            g = T.card_potion_gate("knight", ["a"], ["b"])
            self.assertTrue(any("taunt_per_min" in f for f in g["fails"]), g["fails"])
            # start present, new missing: FAIL
            T.play_metrics = lambda cls, curves, sets=None: ({"contact%": (80.0, 0.1), "taunt/min": (3.0, 0.1)} if curves == ["a"] else {"contact%": (80.0, 0.1)})
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
        for n in ("guardFreshSec", "guardUnreadMaxSec", "guardRepostSec", "guardBarFloorSec", "gateReadmitSec", "gateConeDeg", "gateBeyondU", "gateReadmitMoveU", "gateRepeatU", "gateShortU", "gateNearU", "gatePassU", "gatePassRunU"):
            self.assertIn("loot." + n, T.LOOT_NOT_SEARCHED)
        tune_h = open(os.path.join(os.path.dirname(os.path.abspath(T.__file__)), "Tune.h"), encoding="utf-8").read()
        for n, _, _ in T.LOOT_KNOBS + T.LOOT_EPISODE_KNOBS:
            self.assertIn('"%s"' % n, tune_h, n)   # every searched row is one `--set` reaches (Tune.h kLootKnobsF / kLootKnobsI)

    def test_stale_list_is_refused(self):
        d = tempfile.mkdtemp()
        for name, text, want in (("old", '{"archetype":18,"av":-1}', True), ("new", '{"archetype":18,"summon":[3,4]}', False), ("none", '{"archetype":0}', False)):
            p = os.path.join(d, name)
            open(p, "w").write(text)
            self.assertEqual(T.stale_spawns(p), want, name)

    def test_list_without_placed_rows_is_refused(self):   # placed-spells L1
        d = tempfile.mkdtemp()
        for name, text, want in (("old", '{"archetype":0}', True), ("new", '{"archetype":0,"placedSchema":2}', False), ("schema1", '{"archetype":0,"placedSchema":1}', True), ("none", '{}', False)):
            p = os.path.join(d, name)
            open(p, "w").write(text)
            self.assertEqual(T.stale_placed(p), want, name)

    def test_loot_gate_constants_mirror_native(self):
        # three numbers the lab cannot include (they sit in engine-typed code): Loot.h Rules against native
        here = os.path.dirname(os.path.abspath(T.__file__))
        native = open(os.path.join(here, "..", "..", "native", "logistics", "LootTravel_internal.h"), encoding="utf-8").read()
        lab = open(os.path.join(here, "Loot.h"), encoding="utf-8").read()
        for nat, mine in (("kPassMaxGapSec", "passMaxGapSec"), ("kPassSlackU", "passSlackU"), ("kGateFloorBand", "gateFloorU")):
            a = re.findall(r"constexpr float\s+%s\s*=\s*([0-9.]+)f;" % nat, native)
            b = re.findall(r"\b%s = ([0-9.]+)f" % mine, lab)
            self.assertEqual((len(a), len(b)), (1, 1), nat)
            self.assertEqual(float(a[0]), float(b[0]), nat)

    def test_loot_episode_command_and_key(self):
        saved = (T.A, T.CFG, T.CACHE_DIR, os.environ.get("MFO_ENV_CACHE"), dict(T.TEXT), dict(T._ENV_IDENT))
        try:
            self.loot_episode_command_and_key()
        finally:
            T.A, T.CFG, T.CACHE_DIR = saved[:3]
            os.environ.pop("MFO_ENV_CACHE")
            if saved[3] is not None:
                os.environ["MFO_ENV_CACHE"] = saved[3]
            T.TEXT.clear(); T.TEXT.update(saved[4]); T._ENV_IDENT.clear(); T._ENV_IDENT.update(saved[5])

    def loot_episode_command_and_key(self):
        d = tempfile.mkdtemp()
        T.CACHE_DIR = d + "/"
        os.environ["MFO_ENV_CACHE"] = d
        for f, t in (("index.json", "{}"), ("Tuxborn.loot.json", "one"), ("Vanilla.loot.json", "one")):
            open(os.path.join(d, f), "w").write(t)
        T.A = types.SimpleNamespace(bin=os.path.join(d, "sim"), out=d, builtin_worth=None, loot_places="", world="v3")
        open(T.A.bin, "w").write("#!/bin/sh\necho ran >> %s/runs\necho out\n" % d)
        os.chmod(T.A.bin, 0o755)
        T.CFG = dict(T.FULL)
        self.assertEqual(T.sim_cmd("builtin", "epi:melee,mage", "g3", [], "loot.batchHoldMinSec=6.0", None)[1:],
                         ["--no-json", "--threads", "1", "--loot-report", "6", "--loot-score", "--classes", "melee,mage", "--set", "loot.batchHoldMinSec=6.0"])
        k0, c0 = T.key_of("builtin", "epi:melee", "g3", [], None, None), T.key_of("builtin", "melee", "g3", [], None, None)
        T.A.loot_places = "Tuxborn"
        cmd = T.sim_cmd("builtin", "epi:melee", "g3", [], None, None)
        self.assertEqual(cmd[cmd.index("--env-set") + 1], "Tuxborn")
        self.assertTrue(cmd[cmd.index("--loot-list") + 1].endswith("Tuxborn.loot.json"))
        self.assertEqual(T.loot_world_args(True)[1], "Tuxborn/heldout")
        k1 = T.key_of("builtin", "epi:melee", "g3", [], None, None)
        self.assertNotEqual(k0, k1)   # another set of places is another cache key
        # driver E1: the loot table is part of the key of every loot-world run, and of no other
        runs = lambda: len(open(os.path.join(d, "runs")).read().split()) if os.path.exists(os.path.join(d, "runs")) else 0
        T.text_run("lootrep:x", [T.A.bin], "builtin"); T.text_run("lootrep:x", [T.A.bin], "builtin"); T.text_run("gap:x", [T.A.bin], "builtin")
        self.assertEqual(runs(), 2)
        open(os.path.join(d, "Tuxborn.loot.json"), "w").write("re-extracted")
        self.assertNotEqual(k1, T.key_of("builtin", "epi:melee", "g3", [], None, None))
        self.assertEqual(c0, T.key_of("builtin", "melee", "g3", [], None, None))          # a combat run does not read it
        T.text_run("lootrep:x", [T.A.bin], "builtin"); T.text_run("gap:x", [T.A.bin], "builtin")
        self.assertEqual(runs(), 3)                                                        # the report ran again, the gap run replayed
        k2 = T.key_of("builtin", "epi:melee", "g3", [], None, None)
        open(os.path.join(d, "Vanilla.loot.json"), "w").write("another set's")
        self.assertEqual(k2, T.key_of("builtin", "epi:melee", "g3", [], None, None))
        T._ENV_IDENT.clear()
        open(os.path.join(d, "a.mfoenv"), "w").write("a place")                            # the places are read in every --world
        self.assertNotEqual(k2, T.key_of("builtin", "epi:melee", "g3", [], None, None))
        os.remove(os.path.join(d, "Tuxborn.loot.json"))
        with self.assertRaises(SystemExit):
            T.key_of("builtin", "epi:melee", "g3", [], None, None)                         # a missing table is never an empty hash

    def test_loot_gate_check(self):   # loot review R2: paired per seed, beyond noise, and the "takes less" rows reject
        notes, saved = [], T.open_decision
        T.open_decision = lambda what, why, **kw: notes.append((what, why))
        try:
            def rep(crimes, **rows):   # row -> per-seed values (4 seeds); the gate value is their mean
                rows["crimes"] = crimes
                return {"gate": {k: [sum(v) / len(v), False] for k, v in rows.items()}, "seeds": {k: dict(zip(range(9501, 9505), v)) for k, v in rows.items()}}
            ship = rep([0, 0, 0, 0], **{"leftRR/ep": [1.0, 1.2, 0.8, 1.0], "roadLeft%": [40.0, 41.0, 39.0, 40.0], "restart%": [20.0, 22.0, 18.0, 20.0]})
            why, rows = T.loot_gate_check("train", ship, rep([0, 0, 0, 0], **{"leftRR/ep": [1.3, 0.9, 0.9, 1.0], "roadLeft%": [40.0, 41.0, 39.0, 40.5], "restart%": [20.0, 23.0, 17.5, 20.0]}))
            self.assertIsNone(why)                                           # worse on some seeds, not beyond noise: neither flagged nor rejected
            self.assertEqual(notes, [])
            self.assertEqual(rows["leftRR/ep"][4], "ok")
            why, rows = T.loot_gate_check("train", ship, rep([0, 0, 0, 0], **{"leftRR/ep": [1.0, 1.2, 0.8, 1.0], "roadLeft%": [40.0, 41.0, 39.0, 40.0], "restart%": [25.0, 27.5, 23.0, 25.0]}))
            self.assertIsNone(why)                                           # another row worse beyond noise: a note
            self.assertEqual(len(notes), 1); self.assertIn("restart%", notes[0][1])
            why, rows = T.loot_gate_check("heldout", ship, rep([0, 0, 0, 0], **{"leftRR/ep": [1.0, 1.2, 0.8, 1.0], "roadLeft%": [55.0, 57.0, 54.0, 56.0], "restart%": [20.0, 22.0, 18.0, 20.0]}))
            self.assertIn("roadLeft%", why)                                  # a reject row worse beyond noise, though under its bound: REJECTED
            self.assertEqual(rows["roadLeft%"][4], "REJECT")
            why, _ = T.loot_gate_check("heldout", ship, rep([0, 1, 0, 0], **{"leftRR/ep": [1.0, 1.2, 0.8, 1.0], "roadLeft%": [40.0, 41.0, 39.0, 40.0], "restart%": [20.0, 22.0, 18.0, 20.0]}))
            self.assertIn("crimes", why)                                     # one more crime: rejected, no noise band
            self.assertEqual(set(T.LOOT_REJECT_ROWS), {"leftRR/ep", "goldLeftRR/ep", "roadLeft%", "dibsNear/ep"})
        finally:
            T.open_decision = saved

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

    def test_the_joint_verdict_binds_the_shared_values(self):   # driver P4
        T.SUMMARY["stages"]["spells"] = {"kept": ["t.unmeasuredDamage=0.35"]}
        T.SUMMARY["stages"]["joint"] = {"verdict": "PASS", "table": T.final_curves(), "sets": T.final_sets()}
        sh = T.ship_state()
        self.assertEqual((sh["needs_joint_rerun"], sh["table"], sh["sets"]), ("", self.full, ["t.unmeasuredDamage=0.35"]))
        T.SUMMARY["stages"]["worth"] = {"kept": ["worth.floor=0.1"]}    # accepted after the PASS
        sh = T.ship_state()
        self.assertIn("other shared values", sh["needs_joint_rerun"])
        self.assertEqual((sh["table"], sh["sets"]), ([], []))
        T.SUMMARY["stages"]["joint"]["sets"] = T.final_sets()           # graded again
        self.assertEqual(T.ship_state()["sets"], ["t.unmeasuredDamage=0.35", "worth.floor=0.1"])
        T.SUMMARY["stages"]["spells"]["kept"] = []                      # the reverse: the joint graded a set that is no longer accepted
        self.assertEqual((T.ship_state()["table"], T.ship_state()["sets"]), ([], []))
        T.SUMMARY["stages"]["joint"] = {"verdict": "FAIL: x", "table": T.final_curves(), "sets": T.final_sets()}
        sh = T.ship_state()
        self.assertEqual((sh["table"], sh["sets"]), ([], []))           # a failed joint ships no shared value either
        self.assertEqual(T.final_sets(), "worth.floor=0.1")
        T.SUMMARY["stages"].pop("joint")
        self.assertEqual(T.ship_state()["sets"], [])


class LootGates(unittest.TestCase):   # driver P2, P3: the loot stage judges each candidate on the gate rows and its table on held-out places
    NAMES = ("A", "CFG", "log", "save_summary", "jmany", "race", "final_curves", "loot_report", "loot_values", "LOOT_KNOBS", "LOOT_EPISODE_KNOBS", "LOOT_NOT_SEARCHED", "LOOT_PREFIX", "POOL", "text_run")
    ROWS = ("leftRR/ep", "goldLeftRR/ep", "roadLeft%", "dibsNear/ep", "crimes", "guardSeenTakes", "restart%")

    def setUp(self):
        self.saved, self.sum = {k: getattr(T, k) for k in self.NAMES}, dict(T.SUMMARY)
        T.SUMMARY.clear()
        T.SUMMARY.update({"stages": {}, "open_decisions": [], "auto_decisions": []})
        T.A = types.SimpleNamespace(decisions="auto", loot_places="Tuxborn", loot_world="episodes", bin="/sim")
        T.CFG = dict(T.SMOKE, force=False, passes=2)
        self.logged = []
        T.log, T.save_summary, T.final_curves = self.logged.append, lambda: None, lambda: []
        T.LOOT_KNOBS, T.LOOT_EPISODE_KNOBS, T.LOOT_NOT_SEARCHED = [("loot.buffScale", 0.95, [0.475]), ("loot.healthTarget", 6, [8])], [], {}
        self.value = {"loot.buffScale=0.475": 2.0, "loot.healthTarget=8": 1.0}      # what the search's objective adds: the bad step is the better one
        self.bad = {"loot.buffScale=0.475": {"leftRR/ep": 1.5}}                     # ... and leaves more of what is wanted on every seed (train)
        self.held, self.heldrows, self.reports = {}, {}, []
        noise = [0.0, 0.05, -0.05, 0.02]
        parts = lambda sets: (sets or "").split(",") if sets else []
        T.jmany = lambda reqs: [[(50.0 + noise[s % 4] + sum(self.value.get(k, 0.0) * (1.0 + 0.01 * (s % 3)) for k in parts(st)), 0.0, 1.0) for s in seeds] for _, _, _, _, st, _, seeds in reqs]
        T.race = lambda base, cands, seeds: (T.jmany([base])[0], [(T.jmany([c])[0], len(seeds)) for c in cands])

        def report(sets, heldout):
            self.reports.append((sets, heldout))
            add = {}
            for k in parts(sets):
                for row, v in (self.heldrows if heldout else self.bad).get(k, {}).items():
                    add[row] = add.get(row, 0.0) + v
            seeds = {r: {s: (0.0 if r in T.LOOT_HARD_ROWS else 10.0 + noise[s % 4]) + add.get(r, 0.0) for s in range(9501, 9505)} for r in self.ROWS}
            return {"value": 0.0, "gate": {r: [sum(v.values()) / 4, False] for r, v in seeds.items()}, "gate_failing": [], "seeds": seeds}
        T.loot_report = report
        T.loot_values = lambda sets, heldout: {s: 100.0 + noise[s % 4] + sum(self.held.get(k, 0.5) * (1.0 + 0.1 * (s % 3)) for k in parts(sets)) for s in range(9501, 9505)}

    def tearDown(self):
        for k, v in self.saved.items():
            setattr(T, k, v)
        T.SUMMARY.clear()
        T.SUMMARY.update(self.sum)

    def run_stage(self):
        T.SUMMARY.update({"stages": {}, "open_decisions": [], "auto_decisions": []})
        T.stage_loot()
        return T.SUMMARY["stages"]["loot"], T.SUMMARY["stages"]["loot_world"]

    def rejected(self):
        return [d for d in T.SUMMARY["auto_decisions"] if d["verdict"] == "rejected"]

    def test_a_bad_step_is_refused_as_a_candidate_and_the_good_one_kept(self):   # P2
        L, w = self.run_stage()
        self.assertEqual(L["kept"], ["loot.healthTarget=8"])                       # before: buffScale was carried and the whole table rejected at the end
        row = [r for r in L["rows"] if r["knob"] == "loot.buffScale"][0]
        self.assertFalse(row["accepted"])
        self.assertIn("loot gate: train places: leftRR/ep", row["why"])
        self.assertEqual(row["steps"]["gate_rows"]["leftRR/ep"][4], "REJECT")
        self.assertTrue(any("loot.buffScale" in m and "loot gate" in m for m in self.logged))   # the log says why
        self.assertNotIn("rejected", w)
        self.assertEqual(self.rejected(), [])

    def test_each_reject_and_hard_row_refuses_a_candidate_and_another_row_does_not(self):
        for r in T.LOOT_REJECT_ROWS + T.LOOT_HARD_ROWS:
            self.bad = {"loot.buffScale=0.475": {r: 1.5}}
            self.assertEqual(self.run_stage()[0]["kept"], ["loot.healthTarget=8"], r)
        self.bad = {"loot.buffScale=0.475": {"restart%": 1.5}}
        self.assertEqual(self.run_stage()[0]["kept"], ["loot.buffScale=0.475", "loot.healthTarget=8"])

    def test_the_final_check_still_rejects_the_whole_table(self):
        self.heldrows = {"loot.healthTarget=8": {"roadLeft%": 5.0}}                # held out only
        L, w = self.run_stage()
        self.assertEqual(L["kept"], [])
        self.assertIn("heldout places: roadLeft%", L["rejected"])
        self.assertFalse(any(r["accepted"] for r in L["rows"]))

    def test_a_held_out_value_loss_rejects_under_auto(self):   # P3
        self.held = {"loot.healthTarget=8": -3.0}
        L, w = self.run_stage()
        self.assertEqual((L["kept"], L["kept_before_reject"]), ([], ["loot.healthTarget=8"]))
        self.assertIn("heldout places: value per episode -3", L["rejected"])
        self.assertEqual([d["kind"] for d in self.rejected()], ["loot gate"])
        self.assertIn("DROPPED", [r for r in L["rows"] if r["knob"] == "loot.healthTarget"][0]["why"])
        T.A.decisions = "ask"                                                      # ask: still marth's to decide, and filed as one
        L, w = self.run_stage()
        self.assertEqual(L["kept"], ["loot.healthTarget=8"])
        self.assertTrue(any("loses value" in d["what"] for d in T.SUMMARY["open_decisions"]))

    def test_the_held_out_value_check_is_paired_and_fails_closed(self):
        ship = {1: 10.0, 2: 90.0, 3: 50.0}
        self.assertIsNone(T.loot_value_check(ship, {1: 10.2, 2: 89.8, 3: 50.0})[0])          # within noise
        self.assertIsNone(T.loot_value_check(ship, dict(ship))[0])                             # the same table
        self.assertIn("-1.0", T.loot_value_check(ship, {1: 9.0, 2: 89.1, 3: 48.9})[0])        # a small loss on every seed, far under the spread of the seeds
        self.assertIsNone(T.loot_value_check({1: 0.0, 2: 0.0}, {1: -2.0, 2: -0.2})[0])        # -1.1 at se 0.9: 1.22 se
        self.assertIsNotNone(T.loot_value_check({1: 0.0, 2: 0.0}, {1: -2.0, 2: -0.5})[0])     # -1.25 at se 0.75: 1.67 se
        for a, b in (({}, {}), (ship, {1: 1.0, 2: 1.0}), (ship, {})):
            with self.assertRaises(RuntimeError):
                T.loot_value_check(a, b)

    def test_no_held_out_places_no_table(self):   # P3
        T.A.loot_places = ""
        L, w = self.run_stage()
        self.assertEqual(L["kept"], [])
        self.assertIn("no --loot-places", L["rejected"])
        self.assertTrue(any("no table is kept" in m or "no --loot-places" in m for m in self.logged))
        T.A.loot_world = "stops"
        T.A.out = tempfile.mkdtemp(); T.A.builtin_worth = None
        T.POOL = types.SimpleNamespace(map=lambda f, xs: [f(x) for x in xs])
        T.text_run = lambda key, cmd, world=None, check=None: "CONSUSE 1 48 1 2"
        T.SUMMARY.update({"stages": {}})
        T.stage_loot()
        L = T.SUMMARY["stages"]["loot"]
        self.assertEqual((L["kept"], bool(L["kept_before_reject"])), ([], True))
        self.assertIn("stops lab", L["rejected"])

    @staticmethod
    def text(seeds=(9501, 9502), drop=None, value="12.500"):
        rows = LootGates.ROWS
        out = ["LOOTGATE %-22s %-20s %9.3f  <= %-7.2f %s" % ("ALL", r, 1.0, 2.0, "ok") for r in rows if r != drop]
        out += ["LOOTSEED %d %.6f %s" % (s, 1.0, r) for s in seeds for r in rows]
        return "\n".join(out + ["LOOT-REPORT 8 episodes, 1 places, 1 classes: value/ep with gold %s, without 1.0 (fights 1 + x); gate rows failing: ALL 0, per class 0; wall 1 s" % value]) + "\n"

    def test_the_report_reader_fails_closed(self):
        asked = [9501, 9502]
        self.assertEqual(T.loot_report_parse(self.text(), asked)["value"], 12.5)
        cut = lambda t, bad: "\n".join(x for x in t.splitlines() if not bad(x)) + "\n"
        for t in (cut(self.text(), lambda x: x.startswith("LOOTSEED") and x.endswith("restart%")),           # a row with no LOOTSEED line
                  cut(self.text(), lambda x: x == "LOOTSEED 9502 1.000000 leftRR/ep"),                       # ... with none for one seed
                  cut(self.text(), lambda x: x.startswith("LOOTSEED")), self.text(drop="crimes"), self.text(drop="roadLeft%"), self.text(seeds=(9501, 9503)),
                  cut(self.text(), lambda x: x.startswith("LOOT-REPORT")), self.text(value="nan"), ""):
            with self.assertRaises(RuntimeError):
                T.loot_report_parse(t, asked)

    def test_the_gate_check_refuses_rows_and_seeds_that_do_not_pair(self):
        rep = lambda: T.loot_report_parse(self.text(), [9501, 9502])
        self.assertIsNone(T.loot_gate_check("train", rep(), rep())[0])
        for side in (0, 1):
            for spoil in (lambda x: x["seeds"].pop("leftRR/ep"), lambda x: x["seeds"]["restart%"].pop(9501), lambda x: x["gate"].pop("crimes")):
                two = [rep(), rep()]
                spoil(two[side])
                with self.assertRaises(RuntimeError):
                    T.loot_gate_check("train", *two)
        two = [rep(), rep()]
        two[0]["seeds"]["leftRR/ep"] = {}; two[1]["seeds"]["leftRR/ep"] = {}
        with self.assertRaises(RuntimeError):
            T.loot_gate_check("train", *two)

    def test_the_reports_check_their_text_before_it_is_cached(self):
        seen = []
        T.text_run = lambda key, cmd, world=None, check=None: (seen.append((key, cmd)), check(self.text()), self.text())[2]
        T.loot_report = self.saved["loot_report"]
        T.CFG = dict(T.SMOKE)
        keep, T.loot_table = T.loot_table, lambda s: "/x/%s.loot.json" % s
        try:
            T.loot_report("loot.gold=0.5", True)
            T.POOL = types.SimpleNamespace(map=lambda f, xs: [f(x) for x in xs])
            T.text_run = lambda key, cmd, world=None, check=None: (seen.append((key, cmd)), check(self.text(seeds=(int(cmd[cmd.index("--seeds") + 1]),))), self.text(seeds=(int(cmd[cmd.index("--seeds") + 1]),)))[2]
            self.assertEqual(self.saved["loot_values"]("loot.gold=0.5", True), {9501: 12.5, 9502: 12.5})
        finally:
            T.loot_table = keep
        self.assertTrue(all(k.startswith("lootrep:") and "Tuxborn/heldout" in c and "loot.gold=0.5" in c for k, c in seen))   # one key family, the held-out places


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

    def test_joint_again_after_moved_shared_values(self):   # driver P4
        for stage, kept in (("worth", "worth.floor=0.1"), ("spells", "t.unmeasuredDamage=0.35")):
            T.SUMMARY["stages"] = {"joint": {"verdict": "PASS", "table": [], "sets": None}, stage: {"kept": []}}
            self.assertEqual(T.rerun_after(stage, ["joint", stage], ["summary"])[0], [])            # nothing moved
            T.SUMMARY["stages"][stage]["kept"] = [kept]
            again, why = T.rerun_after(stage, ["joint", stage], ["summary"])
            self.assertEqual(again, ["joint"]); self.assertIn(stage, why)
            self.assertEqual(T.rerun_after(stage, ["joint", stage], ["joint", "summary"])[0], [])   # still to come
            self.assertEqual(T.rerun_after(stage, [stage], ["summary"])[0], [])                     # a run without the joint stage
            T.SUMMARY["stages"]["joint"]["sets"] = kept
            self.assertEqual(T.rerun_after(stage, ["joint", stage, "joint"], ["summary"])[0], [])   # it does not loop
            T.SUMMARY["stages"][stage]["kept"] = []                                                 # a re-run that DROPPED what was graded
            self.assertEqual(T.rerun_after(stage, ["joint", stage], ["summary"])[0], ["joint"])

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
