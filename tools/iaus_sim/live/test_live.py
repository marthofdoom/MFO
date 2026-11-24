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


def knob(spec):
    return spec.split(":")[1].split("=")[0]


def summary(cells, kept, recalib_accepted=None, withheld=(), joint="PASS", joint_table=None, incomplete=(), smoke=False, scope=None):
    """A summary.json as the driver writes it: `kept` = {class: [specs]} for every searched class."""
    full = [s for c in kept for s in kept[c]]
    S = {"config": {"cells": "all" if len(cells) > 15 else "15", "cell_list": list(cells), "smoke": smoke, "classes": scope},
         "stages": {"classes": {c: {"kept": list(v), "complete": c not in incomplete,
                                    "rows": [{"knob": knob(s), "spec": s, "best": float(s.split("=")[1]), "accepted": True} for s in v]} for c, v in kept.items()}},
         "open_decisions": [], "auto_decisions": [], "withheld": list(withheld)}
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
        out += ["11-19 10:00:02  classes %s pass 1  %-34s %-8g -> %-8g train %+.2f (se %.2f)  %s" % (c, knob(s), 1.0, float(s.split("=")[1]), 0.5, 0.1, "ACCEPTED: holdout, many-seed gates, cross-world on fresh seeds") for s in v]
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

    def test_one_accepted_value_differs_between_log_and_summary(self):
        """Not only "the summary has none": ONE knob the log accepted with another value, or that the summary lacks, stops the tool."""
        full = [s for v in KEPT38.values() for s in v]
        log = log_for(KEPT38)
        other = dict(KEPT38, knight=["knight:0.8.a=1.9", "knight:family.0=1.2"])          # the summary holds another value for one knob
        d = round_dir(summary(CELLS38, other, recalib_accepted=[s for v in other.values() for s in v]), log)
        self.assertEqual(G.main(["--out", d]), 2)
        with self.assertRaises(G.Refused) as e:
            G.plan(d)
        self.assertIn("knight 0.8.a (the log accepted 1.3, the summary holds 1.9)", str(e.exception))
        less = dict(KEPT38, knight=["knight:family.0=1.2"])                                # the summary lacks one accepted knob of a class that has others
        d = round_dir(summary(CELLS38, less, recalib_accepted=[s for v in less.values() for s in v]), log)
        self.assertEqual(G.main(["--out", d]), 2)
        d = round_dir(summary(CELLS38, KEPT38, recalib_accepted=full), log)                # and the same log with its own summary passes
        self.assertEqual(G.main(["--out", d]), 0)

    def test_a_smoke_round_writes_nothing(self):
        full = [s for v in KEPT38.values() for s in v]
        d = round_dir(summary(CELLS38, KEPT38, recalib_accepted=full, smoke=True), log_for(KEPT38))
        self.assertEqual(G.main(["--out", d]), 2)
        self.assertIsNone(self.files(d))
        self.assertEqual(G.main(["--out", d, "--unshipped"]), 2)                           # --unshipped does not make a smoke round a result
        self.assertEqual(G.main(["--out", d, "--smoke-ok"]), 0)                            # the tests' own switch
        self.assertIn("SMOKE ROUND, NOT RESULTS", open(os.path.join(d, "MFO_Tuning", "knight.txt")).read())

    def test_a_round_whose_joint_never_ran_writes_nothing(self):
        d = round_dir(summary(CELLS38, KEPT38, joint=None), log_for(KEPT38))
        self.assertEqual(G.main(["--out", d]), 2)
        self.assertIsNone(self.files(d))
        nothing = {c: [] for c in CELLS38}                                                 # even when no value was accepted: it is not a finished round
        d = round_dir(summary(CELLS38, nothing, joint=None), log_for(nothing))
        self.assertEqual(G.main(["--out", d]), 2)
        self.assertEqual(G.main(["--out", d, "--unshipped"]), 0)
        self.assertIn("NOT JOINT-GATED (the joint stage never ran)", open(os.path.join(d, "MFO_Tuning", "knight.txt")).read())

    def test_a_partial_round_writes_nothing(self):
        part = {c: KEPT38[c] for c in CELLS38[:30]}                                        # 8 cells of the run's scope never searched
        d = round_dir(summary(CELLS38, part), log_for(part))
        self.assertEqual(G.main(["--out", d]), 2)
        self.assertIsNone(self.files(d))
        with self.assertRaises(G.Refused) as e:
            G.plan(d)
        self.assertIn("PARTIAL", str(e.exception))
        self.assertIn(CELLS38[-1], str(e.exception))
        self.assertEqual(G.main(["--out", d, "--unshipped"]), 0)
        self.assertEqual(set(self.files(d)), set(CELLS38[:30]))
        self.assertIn("NOT JOINT-GATED", open(os.path.join(d, "MFO_Tuning", "knight.txt")).read())

    def test_shared_values_no_file_carries_are_named(self):
        full = [s for v in KEPT38.values() for s in v]
        S = summary(CELLS38, KEPT38, recalib_accepted=full)
        S["stages"]["worth"] = {"kept": ["worth.ratioCap=3.0"], "rows": []}
        S["stages"]["loot"] = {"kept": ["loot.healthTarget=8"], "rows": []}
        S["stages"]["globals"] = [{"set": "t.momentum=1.1", "verdict": "PASSES RULE"}, {"set": "t.powerCd=7", "verdict": "REJECTED: x"}]
        d = round_dir(S, log_for(KEPT38))
        self.assertEqual(G.main(["--out", d]), 0)
        man = json.load(open(os.path.join(d, "MFO_Tuning", "MANIFEST.json")))
        self.assertEqual(man["shared_values_not_written"], ["worth: worth.ratioCap=3.0", "loot: loot.healthTarget=8", "globals: t.momentum=1.1"])
        self.assertTrue(any("SHARED VALUES NOT IN ANY FILE" in n for n in man["notes"]))

    def test_log_shows_accepted_values_the_summary_lacks(self):
        emptied = dict(KEPT38, **{c: [] for c in T.NEW_CELLS})             # a summary that lost the 23 new classes' values
        d = round_dir(summary(CELLS38, emptied), log_for(KEPT38))           # ... while the log shows them accepted
        self.assertEqual(G.main(["--out", d]), 2)
        self.assertIsNone(self.files(d))
        self.assertEqual(G.main(["--out", d, "--accept-log-mismatch"]), 0)
        self.assertTrue(any("LOG MISMATCH ACCEPTED" in n for n in json.load(open(os.path.join(d, "MFO_Tuning", "MANIFEST.json")))["notes"]))

    def test_unfinished_search_is_refused(self):
        d = round_dir(summary(CELLS38, KEPT38, incomplete=["wizard"]), log_for(KEPT38))
        self.assertEqual(G.main(["--out", d]), 2)

    def test_a_cell_never_searched_gets_no_file_and_a_searched_empty_one_does(self):
        kept = {"thief": [], "knight": ["knight:0.8.a=1.3"]}
        d = round_dir(summary(T.CELLS15, kept, scope="thief,knight"), log_for(kept))        # a round scoped to two classes (--classes)
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

    def fake_round(self, body="import time\ntime.sleep(60)\n"):
        """A round directory and a stand-in simulator binary. The driver is replaced by a script through MFO_LIVE_TEST_DRIVER."""
        out = tempfile.mkdtemp(dir=BASE.name)
        binary = os.path.join(out, "fake-sim")
        open(binary, "w").write("#!/bin/sh\nexit 0\n")
        os.chmod(binary, 0o755)
        drv = os.path.join(out, "fake-driver.py")
        open(drv, "w").write(body)
        env = dict(os.environ, MFO_LIVE_TEST_DRIVER=drv)
        cmd = [sys.executable, os.path.join(HERE, "run_round.py"), "--out", out, "--bin", binary, "--no-generate", "--", "--cells", "all"]
        return out, binary, env, cmd

    def wait_state(self, out, state, secs=30):
        end = time.time() + secs
        while time.time() < end:
            try:
                if json.load(open(os.path.join(out, "round-status.json"))).get("state") == state:
                    return True
            except (OSError, ValueError):
                pass
            time.sleep(0.2)
        return False

    def test_a_second_runner_changes_nothing_and_cannot_stop_the_first(self):
        out, binary, env, cmd = self.fake_round()
        first = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            self.assertTrue(self.wait_state(out, "running"))
            before = {f: open(os.path.join(out, f)).read() for f in ("round-status.json", "round.json", "unit", "priority")}
            second = subprocess.run(cmd[:-2] + ["--cells", "15", "--new-args"], env=env, capture_output=True, text=True)   # other arguments, and it says it means them
            self.assertEqual(second.returncode, 3)
            self.assertIn("another run_round.py holds this round", second.stderr)
            self.assertEqual(before, {f: open(os.path.join(out, f)).read() for f in before})   # the loser wrote nothing
            self.assertIsNone(first.poll())                                                    # ... and stopped nobody
            self.assertEqual(json.load(open(os.path.join(out, "round.json")))["driver_args"], ["--cells", "all"])
        finally:
            first.send_signal(signal.SIGTERM)
            self.assertEqual(first.wait(timeout=30), 128 + signal.SIGTERM)
        self.assertEqual(json.load(open(os.path.join(out, "round-status.json")))["state"], "stopped")

    def test_a_stop_from_outside_is_recorded_as_stopped(self):
        out, binary, env, cmd = self.fake_round()
        p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertTrue(self.wait_state(out, "running"))
        unit = P.unit_of(out)
        if unit and unit != "none":
            self.assertEqual(subprocess.run(["systemctl", "--user", "stop", unit], capture_output=True).returncode, 0)
        else:   # no systemd user manager: a kill of the driver is the outside stop
            os.kill(json.load(open(os.path.join(out, "round-status.json")))["pid"], signal.SIGTERM)
        self.assertEqual(p.wait(timeout=30), 128 + signal.SIGTERM)
        st = json.load(open(os.path.join(out, "round-status.json")))
        self.assertEqual(st["state"], "stopped")
        self.assertIn("outside", st["stopped_by"])

    def test_the_original_arguments_and_the_binary_are_kept_and_compared(self):
        out, binary, env, cmd = self.fake_round("import sys\nsys.exit(0)\n")
        self.assertEqual(subprocess.run(cmd, env=env, capture_output=True).returncode, 0)
        rec = json.load(open(os.path.join(out, "round.json")))
        self.assertEqual(rec["driver_args"], ["--cells", "all"])
        sha0 = rec["bin_sha"]
        r = subprocess.run(cmd[:-2] + ["--cells", "15"], env=env, capture_output=True, text=True)            # other arguments: refused
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("other driver arguments", r.stderr)
        self.assertEqual(subprocess.run(cmd[:-3] + ["--new-args", "--", "--cells", "15"], env=env, capture_output=True).returncode, 0)
        rec = json.load(open(os.path.join(out, "round.json")))
        self.assertEqual(rec["driver_args"], ["--cells", "all"])                                             # the ORIGINAL arguments stay
        self.assertEqual(rec["history"][-1]["driver_args"], ["--cells", "15"])
        open(binary, "a").write("# rebuilt\n")                                                              # another simulator binary
        r = subprocess.run(cmd, env=env, capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("another simulator binary", r.stderr)
        self.assertEqual(subprocess.run(cmd[:-3] + ["--new-bin", "--", "--cells", "all"], env=env, capture_output=True).returncode, 0)
        rec = json.load(open(os.path.join(out, "round.json")))
        self.assertEqual(rec["bin_sha"], sha0)
        self.assertNotEqual(rec["bin_sha_now"], sha0)

    @unittest.skipUnless(os.environ.get("SIM_BIN"), "set SIM_BIN=<built simulator> for the stop / resume proof")
    def test_stop_and_resume_equals_an_uninterrupted_round(self):
        args = ["--", "--smoke", "--worlds", "builtin", "--stages", "prep,classes,joint,recalib,summary", "--classes", "thief", "--jobs", str(min(8, os.cpu_count() or 2)), "--no-emit-runs"]
        run = lambda out: [sys.executable, os.path.join(HERE, "run_round.py"), "--out", out, "--bin", os.environ["SIM_BIN"], "--smoke-ok"] + args
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
