#!/usr/bin/env python3
"""Tests of tools/iaus_sim/live. Run: python3 tools/iaus_sim/live/test_live.py
The stop / resume proof needs a built simulator: SIM_BIN=<sim> python3 tools/iaus_sim/live/test_live.py   (about two minutes; skipped without it)."""
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import gen_class_files as G  # noqa: E402
import priority as P  # noqa: E402
import run_round as R  # noqa: E402
import tune_driver as T  # noqa: E402

CELLS38 = T.CELLS15 + T.NEW_CELLS
BASE = tempfile.TemporaryDirectory(prefix="mfo-live-test-")   # every directory a test makes lives here and goes with the process


def summary(cells, kept, recalib_accepted=None, withheld=(), joint="PASS", joint_table=None, incomplete=()):
    """A summary.json as the driver writes it: `kept` = {class: [specs]} for every searched class."""
    full = [s for c in kept for s in kept[c]]
    S = {"config": {"cells": "all" if len(cells) > 15 else "15", "cell_list": list(cells), "smoke": False},
         "stages": {"classes": {c: {"kept": list(v), "complete": c not in incomplete} for c, v in kept.items()}}, "open_decisions": [], "auto_decisions": [], "withheld": list(withheld)}
    if recalib_accepted is not None:
        S["stages"]["recalib"] = {"accepted": list(recalib_accepted), "rows": [{"spec": s, "d": 0.1, "se": 0.1, "holds": s not in withheld} for s in recalib_accepted]}
    if joint:
        S["stages"]["joint"] = {"verdict": joint, "table": [s for s in full if s not in withheld] if joint_table is None else joint_table}
    return S


def round_dir(S, log_lines=()):
    d = tempfile.mkdtemp(dir=BASE.name)
    json.dump(S, open(os.path.join(d, "summary.json"), "w"))
    if log_lines is not None:
        open(os.path.join(d, "progress.log"), "w").write("\n".join(log_lines) + "\n")
    return d


def log_for(kept):
    out = ["11-19 10:00:00  START stages=prep,classes"]
    for c, v in kept.items():
        out.append("11-19 10:00:01  classes %s: 40 knobs, 9 live, 31 dead" % c)
        out += ["11-19 10:00:02  classes %s pass 1  %-34s 1 -> 2 train +0.50 (se 0.10)  ACCEPTED: holdout, many-seed gates, cross-world on fresh seeds" % (c, s.split(":")[1].split("=")[0]) for s in v]
    return out


KEPT38 = {c: ["%s:0.8.a=1.%d" % (c, i), "%s:family.0=1.2" % c] for i, c in enumerate(CELLS38)}
KEPT38["thief"] = []   # a searched class whose shipped values were the best found


class Generator(unittest.TestCase):
    def files(self, d):
        dest = os.path.join(d, "MFO_Tuning")
        return {f[:-4]: G.parse_file(os.path.join(dest, f)) for f in os.listdir(dest) if f.endswith(".txt")} if os.path.isdir(dest) else None

    def test_every_class_of_a_38_cell_round_gets_its_values(self):
        full = [s for v in KEPT38.values() for s in v]
        d = round_dir(summary(CELLS38, KEPT38, recalib_accepted=full), log_for(KEPT38))
        self.assertEqual(G.main(["--out", d]), 0)
        got = self.files(d)
        self.assertEqual(set(got), set(CELLS38))
        for c in CELLS38:
            self.assertEqual(got[c], KEPT38[c], c)
        self.assertEqual(json.load(open(os.path.join(d, "MFO_Tuning", "MANIFEST.json")))["classes_with_values"], 37)

    def test_round_2_recalib_rows_of_15_classes_cannot_empty_the_23_new_ones(self):
        """THE FAILURE OF 11-19: accepted values for 38 classes, a recalib stage (from a seeded 15-cell summary) that covers 15."""
        old15 = [s for c in T.CELLS15 for s in KEPT38[c]]
        d = round_dir(summary(CELLS38, KEPT38, recalib_accepted=old15), log_for(KEPT38))
        self.assertEqual(G.main(["--out", d]), 2)
        self.assertIsNone(self.files(d))                                   # nothing written at all
        with self.assertRaises(G.Refused) as e:
            G.plan(d)
        self.assertIn("ANOTHER table", str(e.exception))
        self.assertIn("arch-mage", str(e.exception))                        # the classes the stale stage never saw are named
        # and no flag turns recalib rows into the value list: with the stale stage removed (re-run), all 38 are written
        S = json.load(open(os.path.join(d, "summary.json")))
        S["stages"]["recalib"]["accepted"] = [s for v in KEPT38.values() for s in v]
        json.dump(S, open(os.path.join(d, "summary.json"), "w"))
        self.assertEqual(G.main(["--out", d]), 0)
        self.assertEqual(self.files(d)["arch-mage"], KEPT38["arch-mage"])

    def test_the_driver_drops_a_seeded_recalib_of_another_cell_set(self):
        old15 = [s for c in T.CELLS15 for s in KEPT38[c]]
        old = summary(T.CELLS15, {c: KEPT38[c] for c in T.CELLS15}, recalib_accepted=old15, withheld=[old15[0]])
        saved = dict(T.SUMMARY)
        try:
            lines = T.carry_over(json.loads(json.dumps(old)), "all")
            self.assertEqual(len(lines), 1)
            self.assertNotIn("recalib", T.SUMMARY["stages"])
            self.assertNotIn("joint", T.SUMMARY["stages"])
            self.assertEqual(T.SUMMARY["withheld"], [])
            self.assertIn("classes", T.SUMMARY["stages"])                  # the searched values themselves are kept
            self.assertEqual(T.carry_over(json.loads(json.dumps(old)), "15"), [])                   # the same cell set: everything carries over
            self.assertIn("recalib", T.SUMMARY["stages"])
        finally:
            T.SUMMARY.clear()
            T.SUMMARY.update(saved)

    def test_a_withheld_value_is_left_out_and_named(self):
        full = [s for v in KEPT38.values() for s in v]
        wh = ["knight:0.8.a=1.3"]
        self.assertIn(wh[0], full)
        d = round_dir(summary(CELLS38, KEPT38, recalib_accepted=full, withheld=wh), log_for(KEPT38))
        self.assertEqual(G.main(["--out", d]), 0)
        self.assertEqual(self.files(d)["knight"], ["knight:family.0=1.2"])
        self.assertIn(wh[0], open(os.path.join(d, "MFO_Tuning", "knight.txt")).read())   # in the header, as a comment

    def test_withheld_without_a_recalib_stage_is_refused(self):
        d = round_dir(summary(CELLS38, KEPT38, withheld=["knight:0.8.a=1.3"]), log_for(KEPT38))
        self.assertEqual(G.main(["--out", d]), 2)

    def test_no_joint_pass_no_files_unless_unshipped(self):
        d = round_dir(summary(CELLS38, KEPT38, joint="FAIL: knight/g3 worse"), log_for(KEPT38))
        self.assertEqual(G.main(["--out", d]), 2)
        self.assertIsNone(self.files(d))
        self.assertEqual(G.main(["--out", d, "--unshipped"]), 0)
        self.assertIn("NOT JOINT-GATED", open(os.path.join(d, "MFO_Tuning", "knight.txt")).read())

    def test_log_shows_accepted_values_the_summary_lacks(self):
        emptied = dict(KEPT38, **{c: [] for c in T.NEW_CELLS})             # a summary that lost the 23 new classes' values
        d = round_dir(summary(CELLS38, emptied), log_for(KEPT38))           # ... while the log shows them accepted
        self.assertEqual(G.main(["--out", d]), 2)
        self.assertIsNone(self.files(d))
        self.assertEqual(G.main(["--out", d, "--accept-log-mismatch"]), 0)
        self.assertIn("LOG MISMATCH ACCEPTED", json.load(open(os.path.join(d, "MFO_Tuning", "MANIFEST.json")))["notes"][0])

    def test_unfinished_search_is_refused(self):
        d = round_dir(summary(CELLS38, KEPT38, incomplete=["wizard"]), log_for(KEPT38))
        self.assertEqual(G.main(["--out", d]), 2)

    def test_a_cell_never_searched_gets_no_file_and_a_searched_empty_one_does(self):
        kept = {"thief": [], "knight": ["knight:0.8.a=1.3"]}
        d = round_dir(summary(T.CELLS15, kept), log_for(kept))
        self.assertEqual(G.main(["--out", d]), 0)
        got = self.files(d)
        self.assertEqual(set(got), {"thief", "knight"})
        self.assertEqual(got["thief"], [])

    def test_a_cut_off_summary_is_refused(self):
        d = tempfile.mkdtemp(dir=BASE.name)
        open(os.path.join(d, "summary.json"), "w").write('{"stages": {"classes": {"kni')
        self.assertEqual(G.main(["--out", d]), 2)


class Priority(unittest.TestCase):
    def test_the_table(self):
        self.assertEqual(set(P.ORDER), set(P.LEVELS))
        self.assertEqual(P.DEFAULT, "idle")
        self.assertEqual(P.props("idle"), ["CPUWeight=idle", "IOWeight=1"])
        self.assertEqual(P.props("low"), ["CPUWeight=20", "IOWeight=20"])
        self.assertEqual(P.props("normal"), ["CPUWeight=100", "IOWeight=100"])
        self.assertEqual(P.props("high"), ["CPUWeight=400", "IOWeight=400"])
        with self.assertRaises(ValueError):
            P.props("realtime")

    def test_remembered_without_a_running_round(self):
        d = tempfile.mkdtemp(dir=BASE.name)
        self.assertEqual(P.get(d)["level"], "idle")                         # never set: the default
        st = P.set_level(d, "low")
        self.assertEqual((st["level"], st["applied"], st["active"]), ("low", False, False))
        self.assertEqual(open(os.path.join(d, "priority")).read().strip(), "low")
        with self.assertRaises(ValueError):
            P.set_level(d, "fastest")
        self.assertEqual(P.get(d)["level"], "low")                          # a refused level changes nothing

    def test_the_runner_never_sets_nice_chrt_or_ionice_on_the_scope_road(self):
        src = open(os.path.join(HERE, "run_round.py")).read()
        self.assertNotIn('"chrt"', src)
        self.assertNotIn('"ionice"', src)
        self.assertEqual(src.count("os.nice(19)"), 1)                       # the no-systemd fallback only


class Runner(unittest.TestCase):
    def test_split_args(self):
        self.assertEqual(R.split_args(["--out", "x", "--priority", "low", "--", "--cells", "all"]), (["--out", "x", "--priority", "low"], ["--cells", "all"]))
        self.assertEqual(R.split_args(["--out", "x", "--smoke", "--classes", "thief"]), (["--out", "x"], ["--smoke", "--classes", "thief"]))

    def test_unit_name_is_safe_and_per_path(self):
        a, b = R.unit_name("/x/round 3 (new)"), R.unit_name("/y/round 3 (new)")
        self.assertRegex(a, r"^mfo-tune-[A-Za-z0-9_.-]+-[0-9a-f]{6}\.scope$")
        self.assertNotEqual(a, b)

    def test_cache_tail(self):
        d = tempfile.mkdtemp(dir=BASE.name)
        p = os.path.join(d, "cache.jsonl")
        open(p, "w").write('{"k": "a", "s": 1, "v": [1, 0, 0]}\n{"k": "a", "s": 2, "v": [1')
        R.fix_cache_tail(d)
        self.assertTrue(open(p).read().endswith("[1\n"))
        R.fix_cache_tail(d)
        self.assertEqual(open(p).read().count("\n"), 2)

    @unittest.skipUnless(os.environ.get("SIM_BIN"), "set SIM_BIN=<built simulator> for the stop / resume proof")
    def test_stop_and_resume_equals_an_uninterrupted_round(self):
        args = ["--", "--smoke", "--worlds", "builtin", "--stages", "prep,classes,joint,recalib,summary", "--classes", "thief", "--jobs", str(min(8, os.cpu_count() or 2)), "--no-emit-runs"]
        run = lambda out: [sys.executable, os.path.join(HERE, "run_round.py"), "--out", out, "--bin", os.environ["SIM_BIN"]] + args
        a, b = tempfile.mkdtemp(dir=BASE.name), tempfile.mkdtemp(dir=BASE.name)
        self.assertEqual(subprocess.run(run(a), capture_output=True).returncode, 0)
        p = subprocess.Popen(run(b), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        deadline = time.time() + 120
        lines = lambda: sum(1 for _ in open(os.path.join(b, "cache.jsonl"))) if os.path.exists(os.path.join(b, "cache.jsonl")) else 0
        while time.time() < deadline and lines() < 300:
            time.sleep(0.5)                                                  # stop it INSIDE the classes stage, with results on disk
        p.send_signal(signal.SIGTERM)
        self.assertEqual(p.wait(timeout=60), 128 + signal.SIGTERM)
        self.assertEqual(json.load(open(os.path.join(b, "round-status.json")))["state"], "stopped")
        self.assertFalse(P.unit_active(P.unit_of(b)))                       # no simulator of the round is left running
        n_stop = sum(1 for _ in open(os.path.join(b, "cache.jsonl")))
        self.assertEqual(subprocess.run(run(b), capture_output=True).returncode, 0)   # THE SAME COMMAND
        self.assertEqual(P.get(b)["level"], "idle")

        def norm(d):
            s = json.load(open(os.path.join(d, "summary.json")))
            s.pop("cache_stats", None)                                       # this invocation's cache hits and wall time
            s["config"].pop("bin", None)                                     # the wrapper's path is inside --out
            return s
        self.assertEqual(norm(a), norm(b))
        self.assertGreater(n_stop, 100)
        self.assertEqual(G.parse_file(os.path.join(a, "MFO_Tuning", "thief.txt")), G.parse_file(os.path.join(b, "MFO_Tuning", "thief.txt")))
        last = [ln for ln in open(os.path.join(b, "progress.log")) if "STAGE classes done" in ln][-1]
        self.assertNotIn(" 0 of them served from the cache", last)           # the resume reused what the stopped run finished


if __name__ == "__main__":
    unittest.main()
