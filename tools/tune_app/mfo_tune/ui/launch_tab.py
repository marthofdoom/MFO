"""Runs: configure and launch a tune_driver pass (every driver option), show the exact command first, launch detached."""
import os
import time

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (QCheckBox, QComboBox, QFormLayout, QGroupBox, QHBoxLayout, QLabel, QLineEdit, QMessageBox, QPlainTextEdit,
                               QPushButton, QScrollArea, QSpinBox, QSplitter, QVBoxLayout, QWidget)

from .. import catalog, commands
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
        self.nice = QSpinBox()
        self.nice.setRange(-20, 19)
        self.nice.setValue(int(s.get("nice", 10)))
        f.addRow("Nice", self.nice)
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
        for w in (self.smoke, self.no_race, self.no_emit, self.rej_on):
            w.toggled.connect(self.update_preview)
        for w in (self.classes, self.stages, self.worlds):
            w.itemChanged.connect(self.update_preview)
        self.for_class.currentIndexChanged.connect(self.update_preview)
        self.jobs.valueChanged.connect(self.update_preview)
        self.nice.valueChanged.connect(self.update_preview)
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

    def reload_catalog(self):
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
            "builtin_worth": self.bworth.text().strip(), "jobs": self.jobs.value(), "taskset": self.taskset.text().strip(), "nice": self.nice.value(),
            "extra": self.extra.text().strip(),
        }

    def _stages_default(self):
        return widgets.checked(self.stages) == catalog.DEFAULT_STAGES.split(",")

    def argv(self):
        return commands.driver_argv(self.options(), self.ctx.s)

    def update_preview(self, *_):
        o = self.options()
        self.stages.setEnabled(not o["for_class"])
        self.classes.setEnabled(not o["for_class"])
        self.preview.setPlainText(commands.shell(self.argv()))
        w = []
        if not o["bin"]:
            w.append("Set the simulator binary first (Browse… to an existing one, or Build…).")
        elif not os.path.isfile(o["bin"]):
            w.append("The simulator binary does not exist (use Build…).")
        if self.ctx.is_running(o["out"]):
            w.append("A driver is already running on this out directory.")
        elif os.path.exists(os.path.join(o["out"], "cache.jsonl")):
            w.append("The out directory holds an earlier run: the driver RESUMES it (finished work replays from cache.jsonl).")
        self.warn.setText("\n".join(w))
        self.go.setEnabled(bool(o["out"]) and os.path.isfile(o["bin"]) and not self.ctx.is_running(o["out"]))

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
        if QMessageBox.question(self, "Launch run", "Launch this run detached?\n\n" + commands.shell(argv)) != QMessageBox.Yes:
            return
        try:
            os.makedirs(o["out"], exist_ok=True)
            with open(os.path.join(o["out"], "launch-command.txt"), "w") as fh:
                fh.write(commands.shell(argv) + "\n")
            pid = commands.launch_detached(argv, os.path.join(o["out"], "driver.out"), cwd=self.ctx.s["repo"])
        except OSError as e:
            QMessageBox.critical(self, "Launch failed", str(e))
            return
        with open(os.path.join(o["out"], "pid"), "w") as fh:
            fh.write(str(pid) + "\n")
        self.ctx.refresh()
        self.on_launched(o["out"])
