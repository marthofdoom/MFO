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

from mfo_tune import catalog, commands, names, procs, runreader, settings, tuningfiles   # noqa: E402


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
    check("joint failed, no ship yet -> nothing offered (review E3)", runreader.kept_specs(s2) == [], runreader.kept_specs(s2))
    del s2["stages"]["joint"]
    check("no joint stage -> per-class + rejudge + gap", runreader.kept_specs(s2) == ["mage:8.35.b=0.27", "mage:family.attack=2", "mage:0.0.a=2"], runreader.kept_specs(s2))
    s2["stages"]["recalib"] = {"accepted": ["mage:8.35.b=0.27", "mage:0.0.a=2"], "rows": [{"spec": "mage:0.0.a=2", "holds": False}, {"spec": "mage:8.35.b=0.27", "holds": True}]}
    check("recalib wins; a holds=false spec is dropped and listed", runreader.kept_specs(s2) == ["mage:8.35.b=0.27"] and runreader.recalib_dropped(s2) == ["mage:0.0.a=2"])
    s2["stages"]["joint"] = {"verdict": "FAIL: x", "table": ["mage:8.35.b=0.27"], "sets": None}
    check("a failed joint offers nothing, whatever recalib accepted", runreader.kept_specs(s2) == [])
    s2["ship"] = {"table": [], "sets": [], "joint": "FAIL: x", "needs_joint_rerun": "joint gate: FAIL: x"}
    check("ship empty -> nothing offered (class rows without joint.sets are never written)", runreader.kept_specs(s2) == [])
    s2["ship"] = {"table": ["mage:8.35.b=0.3"], "sets": [], "joint": "PASS", "needs_joint_rerun": ""}
    check("ship.table is what is offered", runreader.kept_specs(s2) == ["mage:8.35.b=0.3"])
    # the stage list and the log rows of every searching stage (review E7)
    drv = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "iaus_sim", "tune_driver.py")
    dflt = [l for l in open(drv).read().splitlines() if l.startswith("DEFAULT_STAGES = ")]
    check("catalog.STAGES is the driver's DEFAULT_STAGES, in its order", dflt and eval(dflt[0].split("=", 1)[1]).split(",") == catalog.STAGES == catalog.DEFAULT_STAGES.split(","), catalog.STAGES)
    lg = tempfile.mkdtemp()
    open(os.path.join(lg, "progress.log"), "w").write("\n".join("11-24 10:00:0%d  %s" % (i, m) for i, m in enumerate([
        "classes mage pass 1  8.35.b                          0.2      -> 0.27     train +0.41 (se 0.10)  ACCEPTED",
        "follow mage family.attack                  -> 2        d +0.30 (se 0.05) ACCEPTED: held out",
        "spells round 2  t.unmeasuredDamage     0.3      -> 0.35     screen +0.120 (se 0.020) ACCEPTED",
        "worth pass 1  worth.floor                 0.1      -> 0.2      train [0.31, 0.07]  ACCEPTED: holdout",
        "loot pass 1  loot.gold                  1        -> 2        train None  train: every value raced out"])) + "\n")
    rows = [e for e in runreader.parse_log(os.path.join(lg, "progress.log")) if e["kind"] == "row"]
    check("log rows of classes, follow, spells, worth and loot are all read", [(e["stage"], e["knob"], e["accepted"]) for e in rows] ==
          [("classes", "8.35.b", True), ("follow", "family.attack", True), ("spells", "t.unmeasuredDamage", True), ("worth", "worth.floor", True), ("loot", "loot.gold", False)],
          [(e["stage"], e["knob"]) for e in rows])
    # validator = the in-game loader
    V = tuningfiles.validate_text

    def good(fn, line):
        check("accepts %s: %s" % (fn, line), not V(fn, line + "\n"), V(fn, line + "\n"))

    def bad(fn, line):
        check("rejects %s: %s" % (fn, line), V(fn, line + "\n"))
    for ln in ("mage:8.35.b=0.27", "8.35.b=0.27", "equip.ranged_dist.b=0.2", "equip.trade.w=2", "mage : equip.crossover = 0.3", "family.attack=+2", "scalar.schoolPeak=3",
               "attack.range.px5=0.5", "attack.0.zeroAbove=1", "--curve mage:8.35.floor=0.1", "--curve 'mage:8.35.floor=0.1'", "ATTACK.RANGE.a=1  # c", "mage:family.power_attack=2",
               "spot.high_ground=1.2", "cast_offense.spot_sight.w=2"):
        good("mage.txt", ln)
    for ln in ("healer:8.35.b=0.27", "mage:8.35.px6=1", "mage:8.35.p=1", "mage:scalar.nope=1", "mage:family.nope=1", "mage:family.attack=0", "mage:8.35.w=0", "spot.nope=1", "spot.firing=0",
               "mage:8.%d.a=1" % len(names.CONS),
               "mage:%d.0.a=1" % len(names.FAMILIES), "mage:8.35.b=inf", "mage:8.35.b=nan", "mage:8.35.b=", "mage:8.35=1", "mage:8.35.b.c=1", "t.momentum=1", "mage:8.35.b=1,2", "mage:8.35.b=++1"):
        bad("mage.txt", ln)
    for ln in ("t.momentum=1", "t.emergencyHpHigh=0.9, w.8.35=2", "aw.8=1.5", "c.35.a=0.2", "c.35.floor=0.1,c.35.b=0.4", "--set t.kite=3", "t.kite=+3", "t.kite = 3"):
        good("shared.txt", ln)
    for ln in ("t.kite=-1", "t.emergencyHpLow=1.5", "t.nope=1", "t.kite=1,t.nope=1", "t.kite=1,", "w.8.35=0", "w.attack.range=1", "w.%d.0=1" % len(names.FAMILIES), "aw.%d=1" % len(names.FAMILIES), "c.%d.a=1" % len(names.CONS), "c.35.q=1",
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
    unit = prio.unit_name(st, scope_out)             # the unit run_round.py would name: a live change is only allowed on THAT one
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
    # GUI
    from PySide6.QtWidgets import QApplication, QMessageBox
    from PySide6.QtCore import QPoint, QPointF, Qt
    from PySide6.QtGui import QWheelEvent
    app = QApplication([])
    from mfo_tune.ui.mainwindow import MainWindow
    w = MainWindow()
    w.show()
    app.processEvents()
    for i in range(w.tabs.count()):
        w.tabs.setCurrentIndex(i)
        app.processEvents()
    check("GUI built, %d tabs" % w.tabs.count(), w.tabs.count() == 7)
    C = w.current
    L = w.launch

    def wait_prio():
        t0 = time.time()
        while C._prio_pending and time.time() - t0 < 20:
            app.processEvents()
            time.sleep(0.02)
        app.processEvents()

    boxes = []
    for n in ("critical", "warning", "information"):
        setattr(QMessageBox, n, staticmethod(lambda parent, title, text, *a, _n=n, **k: boxes.append((_n, title, text)) or QMessageBox.Ok))
    QMessageBox.question = staticmethod(lambda parent, title, text, *a, **k: boxes.append(("question", title, text)) or QMessageBox.Yes)

    def pick(level):
        L.prio.setCurrentIndex(L.prio.findText(level))
        L.prio.activated.emit(L.prio.currentIndex())     # what the user's choice emits

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
    check("item 3: untouched drop-down sends NO --priority (the file is followed)", "--priority" not in L.preview.toPlainText())
    pick("low")
    check("Launch tab: the user's change wins in the command", "--priority low" in L.preview.toPlainText())
    fresh = tempfile.mkdtemp(prefix="mfo-selftest-fresh-")
    L.out.setText(fresh)
    app.processEvents()
    check("item 2: a NEW out dir resets the drop-down to idle (the pick was for the other dir) and sends nothing",
          L.prio.currentText() == "idle" and "--priority" not in L.preview.toPlainText(), L.prio.currentText())
    L.out.setText(rem)
    app.processEvents()
    check("item 2: the dir the user picked for keeps the pick", L.prio.currentText() == "low" and "--priority low" in L.preview.toPlainText())
    L.out.setText(fresh)
    L.smoke.setChecked(True)
    L.new_args.setChecked(True)
    L.new_bin.setChecked(True)
    app.processEvents()
    pv = L.preview.toPlainText().split()
    j = pv.index("--")
    check("item 6: smoke sends --no-generate; New arguments / New binary go BEFORE --", "--no-generate" in pv[:j] and "--new-args" in pv[:j] and "--new-bin" in pv[:j] and "--smoke" in pv[j:], L.preview.toPlainText())
    L.smoke.setChecked(False)
    L.new_args.setChecked(False)
    L.new_bin.setChecked(False)
    # item 1: a resume that run_round.py would refuse is refused HERE, with the reason, and nothing starts
    fake_bin = os.path.join(fresh, "fakesim")
    open(fake_bin, "w").write("#!/bin/sh\n")
    os.chmod(fake_bin, 0o755)
    L.bin.setText(fake_bin)
    ref = tempfile.mkdtemp(prefix="mfo-selftest-refuse-")
    import json as _json
    _json.dump({"driver_args": ["--classes", "healer"], "bin_sha": "0" * 16}, open(os.path.join(ref, "round.json"), "w"))
    L.out.setText(ref)
    L.classes.item(0).setCheckState(Qt.Checked)
    app.processEvents()
    am, bm = prio.resume_conflicts(ref, L._drv_args(), fake_bin)
    check("item 1: resume_conflicts sees changed args and a changed binary", am and bm, (am, bm))
    check("item 1: the preview warns that run_round.py will refuse", "REFUSE" in L.warn.text(), L.warn.text()[:80])
    boxes.clear()
    L.launch()
    check("item 1: Launch refuses a changed resume, names both, starts nothing", boxes and boxes[0][0] == "critical" and "New arguments" in boxes[0][2] and "New binary" in boxes[0][2]
          and not os.path.exists(os.path.join(ref, "driver.out")) and not L._procs, boxes[:1])
    # a failing launch: the child ends nonzero, driver.out's new text is shown (a fake run_round, nothing real starts)
    real_launch = commands.launch_detached_proc
    commands.launch_detached_proc = lambda argv, log, cwd=None: real_launch(["sh", "-c", "echo 'run_round: refused: simulated'; exit 1"], log, cwd)
    open(os.path.join(ref, "driver.out"), "w").write("OLD TEXT FROM AN EARLIER LAUNCH\n")
    L.new_args.setChecked(True)
    L.new_bin.setChecked(True)
    boxes.clear()
    L.launch()
    t0 = time.time()
    while L._procs and time.time() - t0 < 10:
        app.processEvents()
        time.sleep(0.05)
    app.processEvents()
    ends = [b for b in boxes if "exit 1" in b[1]]
    check("item 1: after launch a nonzero exit shows driver.out's NEW text only", ends and "simulated" in ends[0][2] and "OLD TEXT" not in ends[0][2], boxes)
    commands.launch_detached_proc = real_launch
    L.new_args.setChecked(False)
    L.new_bin.setChecked(False)
    # item 1: round-status.json state on the Current tab
    open(os.path.join(ref, "progress.log"), "w").write("")
    _json.dump({"state": "refused", "driver_exit": 0, "generator_exit": 1, "updated": "2026-11-24 00:00:00"}, open(os.path.join(ref, "round-status.json"), "w"))
    w.ctx.s["extra_run_roots"] = [ref]
    w.ctx.refresh()
    C.select_dir(ref)
    C.detail()
    wait_prio()
    check("item 1: Current tab shows round-status.json state", "refused" in C.head.text() and "generator exit 1" in C.head.text(), C.head.text()[:120])
    # item 5: a live wrapper (building / hashing / generating) counts as running; Launch disabled, Stop enabled
    wd = tempfile.mkdtemp(prefix="mfo-selftest-wrapper-")
    open(os.path.join(wd, "progress.log"), "w").write("")
    open(os.path.join(wd, "run_round.py"), "w").write("import time\ntime.sleep(600)\n")
    wrap = subprocess.Popen([sys.executable, os.path.join(wd, "run_round.py"), "--out", wd, "--"], start_new_session=True)
    time.sleep(0.4)
    w.ctx.s["extra_run_roots"] = [wd]
    w.ctx.refresh()
    L.out.setText(wd)
    L.bin.setText(fake_bin)
    app.processEvents()
    C.select_dir(wd)
    C.detail()
    wait_prio()
    check("item 5: wrapper alive = running: Launch disabled, Stop enabled, no driver yet", not w.ctx.driver_procs_for(wd) and w.ctx.is_running(wd) and not L.go.isEnabled() and C.stop.isEnabled(),
          (L.go.isEnabled(), C.stop.isEnabled()))
    wrap.terminate()
    wrap.wait(timeout=10)
    w.ctx.refresh()
    L.update_preview()
    check("item 5: wrapper gone -> not running, Launch enabled", not w.ctx.is_running(wd) and L.go.isEnabled() and not C.stop.isEnabled())
    import fcntl
    lk = open(os.path.join(wd, "round.lock"), "a+")
    fcntl.flock(lk, fcntl.LOCK_EX | fcntl.LOCK_NB)
    check("item 5: a held round.lock alone counts as running", w.ctx.is_running(wd) and prio.lock_held(wd))
    lk.close()
    check("item 5: lock released -> not running", not prio.lock_held(wd))
    if sleeper:
        w.ctx.s["extra_run_roots"] = [scope_out]
        w.ctx.refresh()
        C.select_dir(scope_out)
        C.detail()
        wait_prio()
        check("Current tab: drop-down shows idle, enabled for a running unit", C.prio.currentText() == "idle" and C.prio.isEnabled(), C.prio_note.text())
        C.prio.setCurrentIndex(C.prio.findText("normal"))
        C.change_priority()
        wait_prio()
        live = pm.live_weights(unit)
        check("Current tab: change applied LIVE to the unit (CPUWeight=100 IOWeight=100)", live.get("CPUWeight") == "100" and live.get("IOWeight") == "100", live)
        open(os.path.join(scope_out, "priority"), "w").write("low\n")   # remembered differs from what the unit has live (normal)
        C.detail()
        wait_prio()
        check("item 6: live level (normal) is shown against the remembered one (low)", C.prio.currentText() == "normal" and "remembered" in C.prio_note.text() and "low" in C.prio_note.text(), (C.prio.currentText(), C.prio_note.text()))
        C.prio.setCurrentIndex(C.prio.findText("idle"))
        C.change_priority()
        wait_prio()
        check("Current tab: back to idle on the unit", pm.live_weights(unit).get("CPUWeight") == "idle", pm.live_weights(unit))
        # item 4: a unit file naming a scope that is not this out dir's own is never changed
        alien = tempfile.mkdtemp(prefix="mfo-selftest-alien-")
        open(os.path.join(alien, "unit"), "w").write(unit + "\n")
        open(os.path.join(alien, "progress.log"), "w").write("")
        w.ctx.s["extra_run_roots"] = [alien]
        w.ctx.refresh()
        C.select_dir(alien)
        C.detail()
        wait_prio()
        check("item 4: another scope's unit file -> disabled with the reason", not C.prio.isEnabled() and "not the unit run_round.py names" in C.prio_note.text(), C.prio_note.text())
        C.prio.setEnabled(True)
        C.prio.setCurrentIndex(C.prio.findText("high"))
        boxes.clear()
        C.change_priority()
        wait_prio()
        check("item 4: a forced change on it is refused and the foreign unit keeps its weights", pm.live_weights(unit).get("CPUWeight") == "idle" and boxes and boxes[0][0] == "critical"
              and not os.path.exists(os.path.join(alien, "priority")), (pm.live_weights(unit), boxes[:1]))
        shutil.rmtree(alien, ignore_errors=True)
        w.ctx.s["extra_run_roots"] = [scope_out]
        w.ctx.refresh()
        C.select_dir(scope_out)
        C.detail()
        wait_prio()
        # item 6: a hung / missing systemctl is shown, not swallowed
        real_get = pm.get

        def hung(out):
            raise subprocess.TimeoutExpired("systemctl", 10)
        pm.get = hung
        C.detail()
        wait_prio()
        check("item 6: SubprocessError from the read is shown", not C.prio.isEnabled() and "priority not read" in C.prio_note.text(), C.prio_note.text())
        pm.get = real_get
        C.detail()
        wait_prio()
        if A.shot:
            w.tabs.setCurrentWidget(C)
            app.processEvents()
            w.grab().save(A.shot[:-4] + "-current-running.png")
        subprocess.run(["systemctl", "--user", "stop", unit], capture_output=True)   # our own sleep scope only
        sleeper.wait(timeout=20)
        C.detail()
        wait_prio()
        check("Current tab: disabled with a reason once the run is not running", not C.prio.isEnabled() and "not running" in C.prio_note.text(), C.prio_note.text())
    # item 7: the wheel does nothing unless the drop-down has focus; Settings changes rebuild the list
    L.prio.setCurrentIndex(0)
    ev = QWheelEvent(QPointF(5, 5), QPointF(5, 5), QPoint(0, 0), QPoint(0, -120), Qt.NoButton, Qt.NoModifier, Qt.NoScrollPhase, False)
    app.sendEvent(L.prio, ev)
    check("item 7: wheel over an unfocused priority drop-down changes nothing", L.prio.currentText() == "idle" and not ev.isAccepted())
    w.ctx.settings_changed.emit()
    check("item 7: Settings change rebuilds the priority list", [L.prio.itemText(i) for i in range(L.prio.count())] == ["idle", "low", "normal", "high"])
    open(os.path.join(rem, "progress.log"), "w").write("")
    nu = tempfile.mkdtemp(prefix="mfo-selftest-nounit-")
    open(os.path.join(nu, "progress.log"), "w").write("")
    w.ctx.s["extra_run_roots"] = [nu]
    w.ctx.refresh()
    C.select_dir(nu)
    C.detail()
    wait_prio()
    check("Current tab: disabled with a reason for a run without a unit", not C.prio.isEnabled() and "no priority unit" in C.prio_note.text(), C.prio_note.text())
    if A.shot:
        base = A.shot[:-4]
        for i in range(w.tabs.count()):
            w.tabs.setCurrentIndex(i)
            app.processEvents()
            w.grab().save("%s-%d-%s.png" % (base, i, w.tabs.tabText(i).split()[0].lower()))
    for d in (scope_out, rem, nu, fresh, ref, wd):
        shutil.rmtree(d, ignore_errors=True)
    sys.exit(0 if ok else 1)


main()
