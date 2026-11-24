"""Runs: configure and launch a tune_driver pass (every driver option), show the exact command first, launch detached."""
import os
import time

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (QCheckBox, QComboBox, QFormLayout, QGroupBox, QHBoxLayout, QLabel, QLineEdit, QMessageBox, QPlainTextEdit,
                               QPushButton, QScrollArea, QSpinBox, QSplitter, QVBoxLayout, QWidget)

from .. import catalog, commands, prio
from . import bg, widgets


class LaunchTab(QWidget):
    def __init__(self, ctx, on_launched):
        super().__init__()
        self.ctx, self.on_launched = ctx, on_launched
        sp = QSplitter(Qt.Horizontal)
        lay = QVBoxLayout(self)
        lay.addWidget(sp)

        form_w = QWidget()
        f = QFormLayout(form_w)
        s = ctx.s
        self.name = QLineEdit(time.strftime("%m%d-pass"))
        self.out = QLineEdit()
        f.addRow("Run name", self.name)
        f.addRow("Out directory", widgets.path_row(self.out, True))
        self.bin = QLineEdit(s["sim_bin"])
        bb = QWidget()
        h = QHBoxLayout(bb)
        h.setContentsMargins(0, 0, 0, 0)
        h.addWidget(widgets.path_row(self.bin), 1)
        self.build_btn = QPushButton("Build…")
        self.build_btn.setToolTip("Compile the simulator with tools/iaus_sim/run.sh into the path on the left")
        self.build_btn.clicked.connect(self.build)
        h.addWidget(self.build_btn)
        f.addRow("Simulator binary", bb)

        self.smoke = QCheckBox("--smoke (a few seeds and knobs; numbers are NOT results)")
        f.addRow("", self.smoke)
        self.for_class = QComboBox()
        self.for_class.addItem("(none: choose classes and stages below)")
        f.addRow("New-class workflow (--for-class)", self.for_class)
        self.classes = widgets.checklist([])
        self.classes.setMinimumHeight(150)
        f.addRow("Classes (none checked = all)", self._with_buttons(self.classes, [("Beta 1 (15)", catalog.BETA1), ("Clear", [])]))
        self.stages = widgets.checklist(catalog.STAGES, catalog.DEFAULT_STAGES.split(","))
        self.stages.setMinimumHeight(150)
        f.addRow("Stages", self._with_buttons(self.stages, [("Default", catalog.DEFAULT_STAGES.split(",")), ("classes only", ["prep", "classes", "summary"]),
                                                            ("All", catalog.STAGES), ("Clear", [])]))
        self.worlds = widgets.checklist(catalog.WORLDS, catalog.WORLDS)
        self.worlds.setMaximumHeight(90)
        f.addRow("Worlds", self.worlds)
        self.knobs = QLineEdit()
        self.knobs.setPlaceholderText(r"regex, e.g. ^equip\.  (classes / edge / gap search only matching knobs)")
        f.addRow("Knobs filter (--knobs)", self.knobs)
        self.no_race = QCheckBox("--no-race (every candidate on every seed)")
        f.addRow("", self.no_race)
        self.no_emit = QCheckBox("--no-emit-runs (no dashboard results file per sweep)")
        f.addRow("", self.no_emit)
        self.rej_on = QCheckBox("override")
        self.rej = QLineEdit("warrior:6.32.b=0.15")
        self.rej.setPlaceholderText("space-separated --curve specs, or empty for none")
        rj = QWidget()
        hr = QHBoxLayout(rj)
        hr.setContentsMargins(0, 0, 0, 0)
        hr.addWidget(self.rej_on)
        hr.addWidget(self.rej, 1)
        f.addRow("Re-judge (--rejudge)", rj)
        self.sim_flags = QLineEdit("--point-blank --buff-dur real --consumables on")
        f.addRow("Sim flags (--sim-flags)", self.sim_flags)
        self.bworth = QLineEdit()
        f.addRow("Built-in potion worth", widgets.path_row(self.bworth))
        self.jobs = QSpinBox()
        self.jobs.setRange(1, 128)
        self.jobs.setValue(int(s.get("jobs") or os.cpu_count() or 4))
        f.addRow("Jobs (--jobs)", self.jobs)
        self.taskset = QLineEdit(s.get("taskset", ""))
        self.taskset.setPlaceholderText("CPU core set for taskset -c, e.g. 0-7 (empty = any)")
        f.addRow("CPU cores (taskset)", self.taskset)
        self.prio = widgets.FocusCombo()             # live/priority.py ORDER; idle is the default (marth 2026-11-23)
        self._prio_default = "idle"
        self._prio_reason = ""
        self._prio_touched = None                    # (out dir, level) the user picked (only then is --priority sent, only for that dir)
        self._load_levels()
        self.prio.activated.connect(self._prio_picked)   # `activated` = the user chose; a programmatic set does not fire it
        self.prio.setToolTip("The round runs in one systemd user scope at this level (idle unless you pick another). A new run dir always starts at idle; "
                             "a resumed run dir shows its remembered level. The level is sent to run_round.py only when you pick one here. Change it live on the Current runs tab.")
        f.addRow("Priority", self.prio)
        self.new_args = QCheckBox("New arguments (--new-args): this resume changes the driver arguments on purpose")
        self.new_bin = QCheckBox("New binary (--new-bin): this resume uses another simulator binary; nothing of the earlier run is reused")
        f.addRow("", self.new_args)
        f.addRow("", self.new_bin)
        self.extra = QLineEdit()
        self.extra.setPlaceholderText("extra driver arguments, appended verbatim")
        f.addRow("Extra args", self.extra)
        sc = QScrollArea()
        sc.setWidgetResizable(True)
        sc.setWidget(form_w)
        sp.addWidget(sc)

        right = QWidget()
        rv = QVBoxLayout(right)
        rv.addWidget(QLabel("<b>Command</b> (exactly what Launch runs, detached with setsid)"))
        self.preview = QPlainTextEdit()
        self.preview.setReadOnly(True)
        self.preview.setFont(widgets.mono())
        rv.addWidget(self.preview, 1)
        self.warn = QLabel("")
        self.warn.setWordWrap(True)
        rv.addWidget(self.warn)
        self.go = QPushButton("Launch run")
        self.go.setStyleSheet("font-weight: bold; padding: 8px;")
        self.go.clicked.connect(self.launch)
        rv.addWidget(self.go)
        sp.addWidget(right)
        sp.setSizes([620, 560])

        for w in (self.name, self.out, self.bin, self.knobs, self.rej, self.sim_flags, self.bworth, self.taskset, self.extra):
            w.textChanged.connect(self.update_preview)
        for w in (self.smoke, self.no_race, self.no_emit, self.rej_on, self.new_args, self.new_bin):
            w.toggled.connect(self.update_preview)
        for w in (self.classes, self.stages, self.worlds):
            w.itemChanged.connect(self.update_preview)
        self.for_class.currentIndexChanged.connect(self.update_preview)
        self.jobs.valueChanged.connect(self.update_preview)
        self._procs = {}                             # out dir -> (Popen, size of driver.out before launch): watched for an early exit
        self._watch = QTimer(self)
        self._watch.timeout.connect(self._poll_children)
        self.name.textChanged.connect(self._autoout)
        ctx.settings_changed.connect(self.reload_catalog)
        self.reload_catalog()
        self._autoout()

    def _with_buttons(self, lst, presets):
        box = QWidget()
        v = QVBoxLayout(box)
        v.setContentsMargins(0, 0, 0, 0)
        v.addWidget(lst)
        h = QHBoxLayout()
        for label, names in presets:
            b = QPushButton(label)
            b.clicked.connect(lambda _=False, n=names, l=lst: widgets.set_checked(l, n))
            h.addWidget(b)
        h.addStretch(1)
        v.addLayout(h)
        return box

    def _load_levels(self):
        """Fill the drop-down from priority.py (the one table). Called at start and whenever Settings change the repo."""
        self._prio_reason = ""
        keep = self.prio.currentText()
        self.prio.blockSignals(True)
        self.prio.clear()
        try:
            pm = prio.load(self.ctx.s)
            self.prio.addItems(list(pm.ORDER))
            self._prio_default = pm.DEFAULT
            self.prio.setEnabled(True)
        except (ImportError, OSError, SyntaxError) as e:
            self._prio_reason = "priority table not loaded: %s" % e
            self.prio.setEnabled(False)
        self.prio.setCurrentIndex(max(0, self.prio.findText(keep if keep else self._prio_default)))
        self.prio.blockSignals(False)

    def _out_abs(self):
        o = self.out.text().strip()
        return os.path.abspath(os.path.expanduser(o)) if o else ""

    def _prio_picked(self, *_):
        self._prio_touched = (self._out_abs(), self.prio.currentText())
        self.update_preview()

    def _sync_prio(self):
        """The drop-down for the current out dir: the user's pick for THIS dir stays; otherwise the dir's remembered level (a resume), otherwise idle
        (a new dir never inherits the last dir's level)."""
        out = self._out_abs()
        if not self.prio.isEnabled():
            return
        lv = prio.read_file_level(out) if out else ""
        want = lv if self.prio.findText(lv) >= 0 else self._prio_default
        if self._prio_touched and self._prio_touched[0] == out and self.prio.findText(self._prio_touched[1]) >= 0:
            want = self._prio_touched[1]
        if self.prio.currentText() != want:
            self.prio.blockSignals(True)
            self.prio.setCurrentIndex(max(0, self.prio.findText(want)))
            self.prio.blockSignals(False)

    def reload_catalog(self):
        self._load_levels()
        cs = self.ctx.classes()
        old = set(widgets.checked(self.classes))
        self.classes.clear()
        for c in cs:
            li = widgets.QListWidgetItem(c)
            li.setFlags(li.flags() | Qt.ItemIsUserCheckable)
            li.setCheckState(Qt.Checked if c in old else Qt.Unchecked)
            self.classes.addItem(li)
        self.for_class.blockSignals(True)
        self.for_class.clear()
        self.for_class.addItem("(none: choose classes and stages below)")
        self.for_class.addItems(cs)
        self.for_class.blockSignals(False)
        self.bin.setText(self.ctx.s["sim_bin"]) if not self.bin.text() else None
        fl = self.ctx.flags()
        self.no_race.setEnabled("--no-race" in fl or not fl)
        if fl and "--no-race" not in fl:
            self.no_race.setToolTip("this checkout's tune_driver.py has no --no-race (Settings: point the repo at a newer worktree)")
            self.no_race.setChecked(False)
        self.update_preview()

    def _autoout(self):
        self.out.setText(os.path.join(self.ctx.s["runs_root"], self.name.text().strip() or "run"))

    def options(self):
        fc = self.for_class.currentIndex() > 0
        return {
            "out": self.out.text().strip(), "bin": self.bin.text().strip(), "smoke": self.smoke.isChecked(),
            "for_class": self.for_class.currentText() if fc else "", "classes": widgets.checked(self.classes),
            "stages": [x for x in widgets.checked(self.stages)] if not self._stages_default() else [],
            "worlds": widgets.checked(self.worlds) if len(widgets.checked(self.worlds)) != len(catalog.WORLDS) else [],
            "knobs": self.knobs.text().strip(), "no_race": self.no_race.isChecked() and self.no_race.isEnabled(), "no_emit_runs": self.no_emit.isChecked(),
            "rejudge": self.rej.text().strip(), "rejudge_set": self.rej_on.isChecked(), "sim_flags": self.sim_flags.text().strip(),
            "builtin_worth": self.bworth.text().strip(), "jobs": self.jobs.value(), "taskset": self.taskset.text().strip(),
            "priority": self.prio.currentText() if (self.prio.isEnabled() and self._prio_touched and self._prio_touched[0] == self._out_abs()) else "",
            "new_args": self.new_args.isChecked(), "new_bin": self.new_bin.isChecked(), "no_generate": self.smoke.isChecked(),
            "extra": self.extra.text().strip(),
        }

    def _stages_default(self):
        return widgets.checked(self.stages) == catalog.DEFAULT_STAGES.split(",")

    def argv(self):
        return commands.driver_argv(self.options(), self.ctx.s)

    def update_preview(self, *_):
        self._sync_prio()
        o = self.options()
        self.stages.setEnabled(not o["for_class"])
        self.classes.setEnabled(not o["for_class"])
        self.preview.setPlainText(commands.shell(self.argv()))
        w = []
        if not o["bin"]:
            w.append("Set the simulator binary first (Browse… to an existing one, or Build…).")
        elif not os.path.isfile(o["bin"]):
            w.append("The simulator binary does not exist (use Build…).")
        if not os.path.isfile(prio.run_round_path(self.ctx.s)):
            w.append("This checkout has no tools/iaus_sim/live/run_round.py: rounds cannot start (Settings: point the repo at a worktree that has it).")
        if self._prio_reason:
            w.append(self._prio_reason)
        if os.path.abspath(os.path.expanduser(o["out"] or "/")).startswith("/tmp/") or os.path.abspath(os.path.expanduser(o["out"] or "/")) == "/tmp":
            w.append("WARNING: the out directory is under /tmp, which is wiped on reboot. A round's cache and results belong on a disk that survives one.")
        if self._running(o["out"]):
            w.append("A round is already running on this out directory.")
        elif os.path.exists(os.path.join(o["out"], "cache.jsonl")):
            w.append("The out directory holds an earlier run: the driver RESUMES it (finished work replays from cache.jsonl).")
        if o["out"] and os.path.exists(os.path.join(o["out"], "round.json")):
            am, _ = prio.resume_conflicts(os.path.abspath(os.path.expanduser(o["out"])), self._drv_args(), "")
            if am and not o["new_args"]:
                w.append("run_round.py will REFUSE this resume: " + am + "\nTick 'New arguments' if the change is meant, or use a new run name.")
        self.warn.setText("\n".join(w))
        self.go.setEnabled(bool(o["out"]) and os.path.isfile(o["bin"]) and os.path.isfile(prio.run_round_path(self.ctx.s)) and not self._prio_reason and not self._running(o["out"]))

    def _running(self, out):
        """A round is on this out dir: its driver or run_round.py process, its round.lock held, or a child this tab just started and has not yet seen end."""
        if not out:
            return False
        a = os.path.abspath(os.path.expanduser(out))
        p = self._procs.get(a)
        return bool(self.ctx.is_running(a) or (p and p[0].poll() is None))

    def _drv_args(self):
        a = self.argv()
        return a[a.index("--") + 1:]

    def build(self):
        out = self.bin.text().strip()
        if not out:
            return
        self.build_btn.setEnabled(False)
        self.build_btn.setText("Building…")

        def fin(res, err):
            self.build_btn.setEnabled(True)
            self.build_btn.setText("Build…")
            msg = str(err) if err else ("build %s\n\n%s" % ("OK" if res[0] == 0 else "FAILED rc=%d" % res[0], res[1]))
            QMessageBox.information(self, "Simulator build", msg)
            self.update_preview()
        bg.run_bg(lambda: commands.build_sim(self.ctx.s, out), fin)

    def launch(self):
        o = self.options()
        o["out"] = os.path.abspath(os.path.expanduser(o["out"]))   # a relative out would resolve against cwd=repo for the driver but not for the launch files
        argv = commands.driver_argv(o, self.ctx.s)
        # run_round.py refuses a resume that changes the driver arguments or the binary unless told it is meant: say so HERE, not in driver.out
        try:
            am, bm = prio.resume_conflicts(o["out"], argv[argv.index("--") + 1:], o["bin"])
        except OSError as e:
            QMessageBox.critical(self, "Launch refused", "The simulator binary could not be read for the resume check: %s" % e)
            return
        why = []
        if am and not o["new_args"]:
            why.append(am + "\nTick 'New arguments' if this change is meant, or give the run a new name.")
        if bm and not o["new_bin"]:
            why.append(bm + "\nTick 'New binary' if this change is meant, or give the run a new name.")
        if why:
            QMessageBox.critical(self, "run_round.py would refuse this resume", "Nothing was started.\n\n" + "\n\n".join(why))
            return
        if QMessageBox.question(self, "Launch run", "Launch this run detached?\n\n" + commands.shell(argv)) != QMessageBox.Yes:
            return
        log = os.path.join(o["out"], "driver.out")
        try:
            os.makedirs(o["out"], exist_ok=True)
            with open(os.path.join(o["out"], "launch-command.txt"), "w") as fh:
                fh.write(commands.shell(argv) + "\n")
            before = os.path.getsize(log) if os.path.exists(log) else 0
            proc = commands.launch_detached_proc(argv, log, cwd=self.ctx.s["repo"])
        except OSError as e:
            QMessageBox.critical(self, "Launch failed", str(e))
            return
        with open(os.path.join(o["out"], "pid"), "w") as fh:
            fh.write(str(proc.pid) + "\n")
        self._procs[o["out"]] = (proc, before)
        self._watch.start(1000)
        self._prio_touched = None                    # the level now lives in <out>/priority; the next resume follows the file again
        self.ctx.refresh()
        self.on_launched(o["out"])

    def _poll_children(self):
        """A launched run_round.py that ends is reaped here. A nonzero exit (1 = refused or failed, 2 = bad usage, 3 = another runner holds the round)
        shows what it printed (the part of driver.out written by THIS launch). A stop (128+signal) is not an error."""
        for out, (proc, before) in list(self._procs.items()):
            rc = proc.poll()
            if rc is None:
                continue
            del self._procs[out]
            if rc in (1, 2, 3):
                self._show_failure(out, rc, before)
        if not self._procs:
            self._watch.stop()
        self.update_preview()

    def _show_failure(self, out, rc, before):
        try:
            with open(os.path.join(out, "driver.out"), "rb") as f:
                f.seek(before)
                tail = f.read().decode(errors="replace")[-3000:]
        except OSError as e:
            tail = "(driver.out could not be read: %s)" % e
        meaning = {1: "the round was refused or the driver failed", 2: "run_round.py was called wrongly", 3: "another run_round.py already holds this round"}[rc]
        QMessageBox.critical(self, "The run ended with exit %d" % rc, "%s\n\nrun_round.py exited %d: %s.\n\nWhat it printed:\n\n%s" % (out, rc, meaning, tail.strip() or "(nothing)"))
