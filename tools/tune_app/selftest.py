#!/usr/bin/env python3
"""Headless checks: readers on real run dirs, the tuning-file grammar and deploy path (temp dir), command builders, and an offscreen GUI start with
a screenshot.   QT_QPA_PLATFORM=offscreen selftest.py [--runs DIR ...] [--shot out.png]"""
import argparse
import os
import sys
import shutil
import subprocess
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from mfo_tune import commands, procs, runreader, settings, tuningfiles   # noqa: E402


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
    for ln in ("healer:8.35.b=0.27", "mage:8.35.px6=1", "mage:8.35.p=1", "mage:scalar.nope=1", "mage:family.nope=1", "mage:family.attack=0", "mage:8.35.w=0", "mage:8.97.a=1",
               "mage:25.0.a=1", "mage:8.35.b=inf", "mage:8.35.b=nan", "mage:8.35.b=", "mage:8.35=1", "mage:8.35.b.c=1", "t.momentum=1", "mage:8.35.b=1,2", "mage:8.35.b=++1"):
        bad("mage.txt", ln)
    for ln in ("t.momentum=1", "t.emergencyHpHigh=0.9, w.8.35=2", "aw.8=1.5", "c.35.a=0.2", "c.35.floor=0.1,c.35.b=0.4", "--set t.kite=3", "t.kite=+3", "t.kite = 3"):
        good("shared.txt", ln)
    for ln in ("t.kite=-1", "t.emergencyHpLow=1.5", "t.nope=1", "t.kite=1,t.nope=1", "t.kite=1,", "w.8.35=0", "w.attack.range=1", "w.25.0=1", "aw.25=1", "c.97.a=1", "c.35.q=1",
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
    argv = commands.driver_argv({"out": "/mnt/x/out", "bin": "/b/sim", "smoke": True, "jobs": 2, "priority": "low", "taskset": "0-1", "classes": ["mage"], "stages": ["prep", "classes"]}, st)
    j = argv.index("--")
    check("driver argv: round runner, level, driver args after --", argv[:3] == ["taskset", "-c", "0-1"] and argv[3] == "python3" and argv[4].endswith("live/run_round.py")
          and "--priority" in argv[:j] and argv[argv.index("--priority") + 1] == "low" and "--bin" in argv[:j] and "--out" in argv[:j] and "--smoke" in argv[j:]
          and "mage" in argv[j:] and "--bin" not in argv[j:] and "--out" not in argv[j:] and "nice" not in argv, commands.shell(argv))
    check("driver argv: no level asked -> no --priority (the remembered one applies)", "--priority" not in commands.driver_argv({"out": "/mnt/x/o", "bin": "/b/sim"}, st))
    check("settings: runs_root default is the durable tuning root when it exists", not os.path.isdir("/mnt/gaming/modlists/Projects/_tuning") or settings.DEFAULTS["runs_root"] == "/mnt/gaming/modlists/Projects/_tuning", settings.DEFAULTS["runs_root"])
    check("T_SCALARS has the 7 dying / window scalars", all(n in tuningfiles.T_SCALARS for n in ("t.dyingSelfHp", "t.dyingAllyHp", "t.dyingHitsToDown", "t.urgencyHitsDiv", "t.burstWindowSec", "t.grossWindowSec", "t.maxHitWindowSec")))
    check("looting accepts offClassDwellSec", not V("looting.txt", "loot.offClassDwellSec=5\n"))
    check("procs classifies run_round.py as the round's wrapper", procs._classify(["python3", "/r/tools/iaus_sim/live/run_round.py", "--out", "/x"]) == "wrapper")
    # priority: a harmless scope WE start (a sleep), never anyone else's process
    from mfo_tune import prio
    pm = prio.load(st)
    check("priority table from priority.py: idle first and default", list(pm.ORDER) == ["idle", "low", "normal", "high"] and pm.DEFAULT == "idle")
    scope_out = tempfile.mkdtemp(prefix="mfo-selftest-run-")
    unit = "mfo-tune-selftest-%d.scope" % os.getpid()
    sleeper = None
    if pm.have_systemd():
        sleeper = subprocess.Popen(["systemd-run", "--user", "--scope", "--quiet", "--unit=" + unit, "-p", "CPUWeight=idle", "-p", "IOWeight=1", "sleep", "600"])
        for _ in range(50):
            if pm.unit_active(unit):
                break
            time.sleep(0.1)
        open(os.path.join(scope_out, "unit"), "w").write(unit + "\n")
        open(os.path.join(scope_out, "priority"), "w").write("idle\n")
        open(os.path.join(scope_out, "progress.log"), "w").write("")
    else:
        print("SKIP priority live change: no systemd user manager here")
    SCOPE = (sleeper, unit, scope_out)
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
    L = w.launch
    check("Launch tab: priority drop-down lists the levels, default idle, no nice field", [L.prio.itemText(i) for i in range(L.prio.count())] == ["idle", "low", "normal", "high"]
          and L.prio.currentText() == "idle" and not hasattr(L, "nice"))
    L.out.setText("/tmp/zz-selftest-out")
    app.processEvents()
    check("Launch tab warns about an out dir under /tmp", "/tmp" in L.warn.text() and "wiped" in L.warn.text(), L.warn.text()[:60])
    rem = tempfile.mkdtemp(prefix="mfo-selftest-resume-")
    open(os.path.join(rem, "priority"), "w").write("high\n")
    L.out.setText(rem)
    app.processEvents()
    check("Launch tab: resuming a run shows its remembered level", L.prio.currentText() == "high")
    L.prio.setCurrentText("low")
    app.processEvents()
    check("Launch tab: the user's change wins in the command", "--priority low" in L.preview.toPlainText())
    sleeper, unit, scope_out = SCOPE
    C = w.current
    if sleeper:
        w.ctx.s["extra_run_roots"] = [scope_out]
        w.ctx.refresh()
        C.select_dir(scope_out)
        C.detail()
        app.processEvents()
        check("Current tab: drop-down shows idle, enabled for a running unit", C.prio.currentText() == "idle" and C.prio.isEnabled(), C.prio_note.text())
        C.prio.setCurrentIndex(C.prio.findText("normal"))
        C.change_priority()
        live = pm.live_weights(unit)
        check("Current tab: change applied LIVE to the unit (CPUWeight=100 IOWeight=100)", live.get("CPUWeight") == "100" and live.get("IOWeight") == "100", live)
        pm.set_level(scope_out, "low")             # a change made elsewhere
        C.detail()
        check("Current tab: a change made elsewhere shows on refresh", C.prio.currentText() == "low")
        C.prio.setCurrentIndex(C.prio.findText("idle"))
        C.change_priority()
        check("Current tab: back to idle on the unit", pm.live_weights(unit).get("CPUWeight") == "idle", pm.live_weights(unit))
        if A.shot:
            w.tabs.setCurrentWidget(C)
            app.processEvents()
            w.grab().save(A.shot[:-4] + "-current-running.png")
        subprocess.run(["systemctl", "--user", "stop", unit], capture_output=True)   # our own sleep scope only
        sleeper.wait(timeout=20)
        C.detail()
        check("Current tab: disabled with a reason once the run is not running", not C.prio.isEnabled() and "not running" in C.prio_note.text(), C.prio_note.text())
    open(os.path.join(rem, "progress.log"), "w").write("")
    nu = tempfile.mkdtemp(prefix="mfo-selftest-nounit-")
    open(os.path.join(nu, "progress.log"), "w").write("")
    w.ctx.s["extra_run_roots"] = [nu]
    w.ctx.refresh()
    C.select_dir(nu)
    C.detail()
    check("Current tab: disabled with a reason for a run without a unit", not C.prio.isEnabled() and "no priority unit" in C.prio_note.text(), C.prio_note.text())
    if A.shot:
        base = A.shot[:-4]
        for i in range(w.tabs.count()):
            w.tabs.setCurrentIndex(i)
            app.processEvents()
            w.grab().save("%s-%d-%s.png" % (base, i, w.tabs.tabText(i).split()[0].lower()))
    for d in (scope_out, rem, nu):
        shutil.rmtree(d, ignore_errors=True)
    sys.exit(0 if ok else 1)


main()
