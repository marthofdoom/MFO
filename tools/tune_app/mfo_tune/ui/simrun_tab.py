"""Oracle / scenario runs: build a simulator command (oracle mode, scenarios, worlds, seeds, classes, curve/set specs), launch it detached and
browse the results of earlier ones (<runs_root>/simruns/<name>/{command.txt,out.txt,result.json})."""
import json
import os
import time

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (QCheckBox, QComboBox, QFormLayout, QHBoxLayout, QLabel, QLineEdit, QMessageBox, QPlainTextEdit, QPushButton,
                               QScrollArea, QSpinBox, QSplitter, QTabWidget, QVBoxLayout, QWidget)

from .. import commands
from . import widgets


class SimRunTab(QWidget):
    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        lay = QVBoxLayout(self)
        sp = QSplitter(Qt.Horizontal)
        lay.addWidget(sp)
        fw = QWidget()
        f = QFormLayout(fw)
        self.name = QLineEdit(time.strftime("%m%d-oracle"))
        f.addRow("Result name", self.name)
        self.world = QComboBox()
        self.world.addItem("builtin (no list data)", "")
        for w in ("LoreRim", "Tuxborn", "Vanilla"):
            self.world.addItem(w, os.path.expanduser("~/.cache/mfo-list-balance/%s.sim.json" % w))
        f.addRow("World (--list)", self.world)
        self.classes = widgets.checklist([])
        self.classes.setMinimumHeight(140)
        f.addRow("Classes (none = default)", self.classes)
        self.scen = widgets.checklist([])
        self.scen.setMinimumHeight(140)
        f.addRow("Scenarios (--scenario; none = natural fights)", self.scen)
        self.oracle = QCheckBox("--oracle (the planner plays the scenario)")
        f.addRow("", self.oracle)
        self.ok = QSpinBox()
        self.ok.setRange(0, 512)
        self.ok.setSpecialValueText("default")
        f.addRow("Oracle K (--oracle-k)", self.ok)
        self.oh = QSpinBox()
        self.oh.setRange(0, 3600)
        self.oh.setSpecialValueText("default")
        f.addRow("Oracle horizon s (--oracle-h)", self.oh)
        self.fights = QSpinBox()
        self.fights.setRange(0, 100000)
        self.fights.setValue(8)
        f.addRow("Fights per cell (--fights)", self.fights)
        self.seeds = QLineEdit()
        self.seeds.setPlaceholderText("e.g. 1,2,3  (empty: --seed below)")
        f.addRow("Seeds (--seeds)", self.seeds)
        self.seed = QSpinBox()
        self.seed.setRange(0, 10**9)
        self.seed.setValue(1)
        f.addRow("Seed (--seed)", self.seed)
        self.curves = QPlainTextEdit()
        self.curves.setPlaceholderText("one --curve spec per line, e.g. mage:8.35.b=0.27")
        self.curves.setMaximumHeight(80)
        f.addRow("Curves (--curve)", self.curves)
        self.sets = QLineEdit()
        self.sets.setPlaceholderText("--set specs, comma separated, e.g. t.momentum=1.0")
        f.addRow("Sets (--set)", self.sets)
        self.co = QLineEdit()
        self.co.setPlaceholderText("--class-oracle N (empty = sim default)")
        f.addRow("Class oracle", self.co)
        self.gs = QLineEdit()
        self.gs.setPlaceholderText("--gap-scenarios N (empty = sim default)")
        f.addRow("Gap scenarios", self.gs)
        self.rating = QSpinBox()
        self.rating.setRange(0, 1000)
        f.addRow("Rating battery (--rating)", self.rating)
        self.decline = QSpinBox()
        self.decline.setRange(0, 1000)
        f.addRow("Health decline (--decline)", self.decline)
        self.flags = QLineEdit("--point-blank --buff-dur real --consumables on")
        f.addRow("Other sim flags", self.flags)
        self.threads = QSpinBox()
        self.threads.setRange(1, 128)
        self.threads.setValue(4)
        f.addRow("Threads", self.threads)
        self.taskset = QLineEdit(ctx.s.get("taskset", ""))
        f.addRow("CPU cores (taskset)", self.taskset)
        self.nice = QSpinBox()
        self.nice.setRange(-20, 19)
        self.nice.setValue(int(ctx.s.get("nice", 10)))
        f.addRow("Nice", self.nice)
        sc = QScrollArea()
        sc.setWidgetResizable(True)
        sc.setWidget(fw)
        sp.addWidget(sc)

        right = QTabWidget()
        run = QWidget()
        rv = QVBoxLayout(run)
        rv.addWidget(QLabel("<b>Command</b>"))
        self.preview = QPlainTextEdit()
        self.preview.setReadOnly(True)
        self.preview.setFont(widgets.mono())
        self.preview.setMaximumHeight(130)
        rv.addWidget(self.preview)
        self.go = QPushButton("Launch simulator run")
        self.go.clicked.connect(self.launch)
        rv.addWidget(self.go)
        rv.addWidget(QLabel("<b>Results</b> (earlier sim runs; double-click to open)"))
        self.res = widgets.table(["Result", "State", "When", "Size"], sortable=False)
        self.res.itemSelectionChanged.connect(self.show_result)
        rv.addWidget(self.res)
        right.addTab(run, "Launch + results")
        self.view = QPlainTextEdit()
        self.view.setReadOnly(True)
        self.view.setFont(widgets.mono())
        self.view.setLineWrapMode(QPlainTextEdit.NoWrap)
        right.addTab(self.view, "Output")
        self.tabs = right
        sp.addWidget(right)
        sp.setSizes([560, 620])
        for w in (self.name, self.seeds, self.sets, self.co, self.gs, self.flags, self.taskset):
            w.textChanged.connect(self.update_preview)
        for w in (self.oracle,):
            w.toggled.connect(self.update_preview)
        for w in (self.ok, self.oh, self.fights, self.seed, self.rating, self.decline, self.threads, self.nice):
            w.valueChanged.connect(self.update_preview)
        for w in (self.classes, self.scen):
            w.itemChanged.connect(self.update_preview)
        self.world.currentIndexChanged.connect(self.update_preview)
        self.curves.textChanged.connect(self.update_preview)
        ctx.settings_changed.connect(self.reload_catalog)
        ctx.changed.connect(self.refresh_results)
        self.reload_catalog()
        self.refresh_results()

    def root(self):
        return os.path.join(self.ctx.s["runs_root"], "simruns")

    def reload_catalog(self):
        for lst, names in ((self.classes, self.ctx.classes()), (self.scen, self.ctx.scenarios())):
            lst.clear()
            for n in names:
                li = widgets.QListWidgetItem(n)
                li.setFlags(li.flags() | Qt.ItemIsUserCheckable)
                li.setCheckState(Qt.Unchecked)
                lst.addItem(li)
        self.update_preview()

    def options(self):
        name = self.name.text().strip() or "simrun"
        d = os.path.join(self.root(), name)
        return {
            "bin": self.ctx.s["sim_bin"], "list": self.world.currentData() or "", "classes": widgets.checked(self.classes), "scenario": widgets.checked(self.scen),
            "oracle": self.oracle.isChecked(), "oracle_k": self.ok.value(), "oracle_h": self.oh.value(), "seeds": self.seeds.text().strip(),
            "seed": self.seed.value(), "fights": self.fights.value() or None, "threads": self.threads.value(),
            "curves": [ln.strip() for ln in self.curves.toPlainText().splitlines() if ln.strip() and not ln.strip().startswith("#")],
            "sets": self.sets.text().strip(), "class_oracle": self.co.text().strip(), "gap_scenarios": self.gs.text().strip(),
            "rating": self.rating.value(), "decline": self.decline.value(), "label": name, "json_path": os.path.join(d, "result.json"),
            "flags": self.flags.text().strip(), "taskset": self.taskset.text().strip(), "nice": self.nice.value(), "dir": d,
        }

    def update_preview(self, *_):
        o = self.options()
        self.preview.setPlainText(commands.shell(commands.sim_argv(o, self.ctx.s)))

    def launch(self):
        o = self.options()
        argv = commands.sim_argv(o, self.ctx.s)
        if not os.path.isfile(o["bin"]):
            QMessageBox.warning(self, "No simulator", "The simulator binary is missing: set it in Settings or build it on the Runs tab.")
            return
        if o["list"] and not os.path.isfile(o["list"]):
            QMessageBox.warning(self, "World missing", "%s does not exist." % o["list"])
            return
        os.makedirs(o["dir"], exist_ok=True)
        with open(os.path.join(o["dir"], "command.txt"), "w") as fh:
            fh.write(commands.shell(argv) + "\n")
        pid = commands.launch_detached(argv, os.path.join(o["dir"], "out.txt"), cwd=self.ctx.s["repo"])
        with open(os.path.join(o["dir"], "pid"), "w") as fh:
            fh.write(str(pid))
        self.refresh_results()

    def refresh_results(self):
        rows = []
        r = self.root()
        names = sorted(os.listdir(r), reverse=True) if os.path.isdir(r) else []
        live = {os.path.dirname(p.get("json", "")) for p in self.ctx.procs}
        for n in names:
            d = os.path.join(r, n)
            if not os.path.isdir(d):
                continue
            running = any(p["kind"] == "sim" and d in p["cmd"] for p in self.ctx.procs)
            out = os.path.join(d, "out.txt")
            rows.append([n, "RUNNING" if running else ("done" if os.path.exists(os.path.join(d, "result.json")) else "finished (no json)"),
                         time.strftime("%m-%d %H:%M", time.localtime(os.path.getmtime(out))) if os.path.exists(out) else "",
                         "%d KB" % (os.path.getsize(out) // 1024) if os.path.exists(out) else ""])
        keep = self.res.currentRow()
        self.res.blockSignals(True)
        widgets.fill(self.res, rows)
        self.res.blockSignals(False)
        if 0 <= keep < self.res.rowCount():
            self.res.selectRow(keep)

    def show_result(self):
        r = self.res.currentRow()
        if r < 0:
            return
        d = os.path.join(self.root(), self.res.item(r, 0).text())
        text = ""
        try:
            text = open(os.path.join(d, "out.txt"), errors="replace").read()[-300000:]
        except OSError:
            pass
        j = os.path.join(d, "result.json")
        head = ""
        if os.path.exists(j):
            try:
                g = json.load(open(j))
                h = g.get("headline") or {}
                head = "== result.json: headline J=%s (scenarios %s, natural %s), %d classes, %d scenarios ==\n" % (
                    h.get("j"), h.get("j_scenarios"), h.get("j_natural"), len(g.get("class_gaps", [])), len(g.get("scenarios", [])))
                for c in g.get("class_gaps", []):
                    head += "  %-14s gap %+7.2f (se %.2f, %s fights)\n" % (c["class"], c.get("gap") or 0, c.get("se") or 0, c.get("fights"))
                head += "\n"
            except (OSError, ValueError) as e:
                head = "result.json unreadable: %s\n" % e
        cmdp = os.path.join(d, "command.txt")
        cmd = open(cmdp).read() if os.path.exists(cmdp) else ""
        self.view.setPlainText("$ " + cmd + "\n" + head + text)
        self.tabs.setCurrentWidget(self.view)
