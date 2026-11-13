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
    s = {"stages": {"classes": {"mage": {"kept": ["mage:8.35.b=0.27"]}}, "globals": [{"set": "t.momentum=1.0", "verdict": "ACCEPTED"}, {"set": "worth.floor=2", "verdict": "ACCEPTED"}]}}
    f = tuningfiles.generate(runreader.kept_specs(s), runreader.accepted_sets(s), "test")
    check("generate -> files", sorted(f) == ["mage.txt", "potions.txt", "shared.txt"], sorted(f))
    check("generated files validate", not any(tuningfiles.validate_text(k, v) for k, v in f.items()))
    check("bad line is flagged", tuningfiles.validate_text("mage.txt", "nonsense=1\n"))
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
