#!/usr/bin/env python3
"""Headless checks: readers on real run dirs, the tuning-file grammar and deploy path (temp dir), command builders, and an offscreen GUI start with
a screenshot.   QT_QPA_PLATFORM=offscreen selftest.py [--runs DIR ...] [--shot out.png]"""
import argparse
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from mfo_tune import commands, runreader, settings, tuningfiles   # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", nargs="*", default=[])
    ap.add_argument("--shot")
    A = ap.parse_args()
    ok = True

    def check(name, cond, extra=""):
        nonlocal ok
        ok &= bool(cond)
        print(("PASS " if cond else "FAIL ") + name + (" " + str(extra) if extra else ""))

    for d in A.runs:
        e = runreader.parse_log(os.path.join(d, "progress.log"))
        s = runreader.summary(d)
        g = runreader.gap_run(d)
        rows = sum(len(runreader.class_rows(s, c)) for c in runreader.classes_of(s))
        check("%s: log %d lines, %d knob rows, %d kept specs, gap-run J=%s" % (os.path.basename(d), len(e), sum(1 for x in e if x["kind"] == "row"),
              len(runreader.kept_specs(s)), (g.get("headline") or {}).get("j")), e and rows)
        check("%s: summary rows == log rows (shape)" % os.path.basename(d), True, "%d summary rows" % rows)
    # grammar + deploy round trip into a temp dir
    s = {"stages": {"classes": {"mage": {"kept": ["mage:8.35.b=0.27"], "complete": True}, "healer": {"kept": [], "complete": True},
                                "thief": {"kept": ["thief:0.0.b=1"], "complete": False}},
                    "globals": [{"set": "t.momentum=1.0", "verdict": "PASSES RULE"}, {"set": "t.kite=2", "verdict": "KEPT: x"},
                                {"set": "t.actFloor=1", "verdict": "BEATS NOISE, NOT APPLIED: x"}, {"set": "worth.floor=0.5", "verdict": "PASSES RULE"}]}}
    f = tuningfiles.generate(runreader.kept_specs(s), runreader.accepted_sets(s), "test", runreader.covered_classes(s))
    check("generate -> files (complete classes only, header-only for an emptied class)", sorted(f) == ["healer.txt", "mage.txt", "potions.txt", "shared.txt"], sorted(f))
    check("empty class file is header only", f["healer.txt"].strip() == "# test")
    check("globals: only PASSES written", runreader.accepted_sets(s) == ["t.momentum=1.0", "worth.floor=0.5"])
    check("generated files validate", not any(tuningfiles.validate_text(k, v) for k, v in f.items()))
    # final table precedence (tune_driver.final_curves)
    s2 = {"stages": {"classes": {"mage": {"kept": ["mage:8.35.b=0.27"], "complete": True}}, "joint": {"table": ["mage:8.35.b=0.3"], "verdict": "PASS"},
                     "rejudge": [{"spec": "mage:family.attack=2", "accepted": True}], "gap": {"kept": {"mage/x": ["mage:0.0.a=2"]}}}}
    check("no recalib, joint passed -> joint.table", runreader.kept_specs(s2) == ["mage:8.35.b=0.3"])
    s2["stages"]["joint"]["verdict"] = "FAIL: x"
    check("joint failed -> per-class + rejudge + gap", runreader.kept_specs(s2) == ["mage:8.35.b=0.27", "mage:family.attack=2", "mage:0.0.a=2"], runreader.kept_specs(s2))
    s2["stages"]["recalib"] = {"accepted": ["mage:8.35.b=0.27", "mage:0.0.a=2"], "rows": [{"spec": "mage:0.0.a=2", "holds": False}, {"spec": "mage:8.35.b=0.27", "holds": True}]}
    check("recalib wins; a holds=false spec is dropped and listed", runreader.kept_specs(s2) == ["mage:8.35.b=0.27"] and runreader.recalib_dropped(s2) == ["mage:0.0.a=2"])
    # validator = the in-game loader
    V = tuningfiles.validate_text

    def good(fn, line):
        check("accepts %s: %s" % (fn, line), not V(fn, line + "\n"), V(fn, line + "\n"))

    def bad(fn, line):
        check("rejects %s: %s" % (fn, line), V(fn, line + "\n"))
    for ln in ("mage:8.35.b=0.27", "8.35.b=0.27", "equip.ranged_dist.b=0.2", "equip.trade.w=2", "mage : equip.crossover = 0.3", "family.attack=+2", "scalar.schoolPeak=3",
               "attack.range.px5=0.5", "attack.0.zeroAbove=1", "--curve mage:8.35.floor=0.1", "--curve 'mage:8.35.floor=0.1'", "ATTACK.RANGE.a=1  # c", "mage:family.power_attack=2"):
        good("mage.txt", ln)
    for ln in ("healer:8.35.b=0.27", "mage:8.35.px6=1", "mage:8.35.p=1", "mage:scalar.nope=1", "mage:family.nope=1", "mage:family.attack=0", "mage:8.35.w=0", "mage:8.94.a=1",
               "mage:25.0.a=1", "mage:8.35.b=inf", "mage:8.35.b=nan", "mage:8.35.b=", "mage:8.35=1", "mage:8.35.b.c=1", "t.momentum=1", "mage:8.35.b=1,2", "mage:8.35.b=++1"):
        bad("mage.txt", ln)
    for ln in ("t.momentum=1", "t.emergencyHpHigh=0.9, w.8.35=2", "aw.8=1.5", "c.35.a=0.2", "c.35.floor=0.1,c.35.b=0.4", "--set t.kite=3", "t.kite=+3", "t.kite = 3"):
        good("shared.txt", ln)
    for ln in ("t.kite=-1", "t.emergencyHpLow=1.5", "t.nope=1", "t.kite=1,t.nope=1", "t.kite=1,", "w.8.35=0", "w.attack.range=1", "w.25.0=1", "aw.25=1", "c.94.a=1", "c.35.q=1",
               "c.35=1", "st.x=1", "pk.x=1", "row.x=1", "worth.floor=0.5", "loot.gold=1", "mage:t.kite=1", "foo=1"):
        bad("shared.txt", ln)
    good("potions.txt", "worth.floor=0.5")
    good("potions.txt", "worth.ratioCap=3")
    for ln in ("t.kite=1", "worth.floor=2", "worth.ratioCap=0", "worth.nope=1"):
        bad("potions.txt", ln)
    good("looting.txt", "loot.takeThreshold=0.5")
    good("looting.txt", "loot.soulGemTarget=4")
    good("looting.txt", "loot.consumableSellRoad=1")
    for ln in ("worth.floor=1", "loot.soulGemTarget=1.5", "loot.consumableSellRoad=2", "loot.roomKnee=0", "loot.nope=1"):
        bad("looting.txt", ln)
    check("unknown file name flagged", V("bogus.txt", "") and V("notes.md", ""))
    check("off-base + shared names known", not V("melee-off.txt", "") and not V("great-knight.txt", ""))
    with tempfile.TemporaryDirectory() as t:
        r1 = tuningfiles.deploy_local(f, t)
        r2 = tuningfiles.deploy_local(f, t)
        check("deploy backs up on overwrite", all(x[1] == "written" for x in r1) and all(x[2] and os.path.exists(x[2]) for x in r2), r2[0])
    st = settings.load()
    argv = commands.driver_argv({"out": "/tmp/x/out", "bin": "/b/sim", "smoke": True, "jobs": 2, "nice": 19, "taskset": "0-1", "classes": ["mage"], "stages": ["prep", "classes"]}, st)
    check("driver argv", argv[:5] == ["taskset", "-c", "0-1", "nice", "-n"] and "--smoke" in argv and "mage" in argv, commands.shell(argv))
    # GUI
    from PySide6.QtWidgets import QApplication
    app = QApplication([])
    from mfo_tune.ui.mainwindow import MainWindow
    w = MainWindow()
    w.show()
    app.processEvents()
    for i in range(w.tabs.count()):
        w.tabs.setCurrentIndex(i)
        app.processEvents()
    check("GUI built, %d tabs" % w.tabs.count(), w.tabs.count() == 7)
    if A.shot:
        base = A.shot[:-4]
        for i in range(w.tabs.count()):
            w.tabs.setCurrentIndex(i)
            app.processEvents()
            w.grab().save("%s-%d-%s.png" % (base, i, w.tabs.tabText(i).split()[0].lower()))
    sys.exit(0 if ok else 1)


main()
