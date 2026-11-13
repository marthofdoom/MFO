"""Current runs: every known run dir + running driver processes; live progress log with filters, stage timings, ETA, CPU, Stop."""
import os
import time

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (QCheckBox, QComboBox, QHBoxLayout, QLabel, QLineEdit, QMessageBox, QPlainTextEdit, QPushButton, QSplitter,
                               QVBoxLayout, QWidget)

from .. import procs, runreader
from . import widgets


class CurrentTab(QWidget):
    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        self.sel = None
        self._cache = {}                  # run dir -> ((mtime, size), entries)
        lay = QVBoxLayout(self)
        sp = QSplitter(Qt.Vertical)
        lay.addWidget(sp)
        self.runs = widgets.table(["Run", "State", "Stage", "Class", "Cores", "Elapsed", "ETA", "Last line", "Dir"], sortable=False)
        self.runs.itemSelectionChanged.connect(self._select)
        sp.addWidget(self.runs)

        low = QWidget()
        lv = QVBoxLayout(low)
        top = QHBoxLayout()
        self.head = QLabel("Select a run")
        self.head.setWordWrap(True)
        top.addWidget(self.head, 1)
        self.stop = QPushButton("Stop run…")
        self.stop.clicked.connect(self.stop_run)
        top.addWidget(self.stop)
        lv.addLayout(top)
        self.stages = widgets.table(["Stage", "Seconds", "Detail"], sortable=False)
        self.stages.setMaximumHeight(130)
        lv.addWidget(self.stages)
        flt = QHBoxLayout()
        self.acc = QCheckBox("accepted only")
        self.cls = QComboBox()
        self.cls.addItem("all classes")
        self.cls.setMinimumWidth(130)
        self.text = QLineEdit()
        self.text.setPlaceholderText("filter text")
        self.tail = QCheckBox("follow")
        self.tail.setChecked(True)
        flt.addWidget(self.acc)
        flt.addWidget(self.cls)
        flt.addWidget(self.text, 1)
        flt.addWidget(self.tail)
        lv.addLayout(flt)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setFont(widgets.mono())
        self.log.setLineWrapMode(QPlainTextEdit.NoWrap)
        lv.addWidget(self.log, 1)
        sp.addWidget(low)
        sp.setSizes([220, 520])
        for w in (self.acc,):
            w.toggled.connect(self.render_log)
        self.cls.currentIndexChanged.connect(self.render_log)
        self.text.textChanged.connect(self.render_log)
        ctx.changed.connect(self.refresh)
        self.refresh()

    def entries(self, d):
        p = os.path.join(d, "progress.log")
        try:
            st = os.stat(p)
            key = (st.st_mtime, st.st_size)
        except OSError:
            return []
        c = self._cache.get(d)
        if c and c[0] == key:
            return c[1]
        e = runreader.parse_log(p)
        self._cache[d] = (key, e)
        return e

    def planned(self, d):
        """Classes the run will search: its --classes, else the driver's gap_order (15 cells by default)."""
        for p in self.ctx.driver_procs_for(d):
            args = p["cmd"].split()
            if "--for-class" in args:      # the new-class workflow searches exactly one class
                return 1
            if "--classes" in args and args.index("--classes") + 1 < len(args):
                return len(args[args.index("--classes") + 1].split(","))
        go = runreader.summary(d).get("stages", {}).get("gap_order")
        return len(go) if go else 15

    def select_dir(self, d):
        self.sel = d
        for r in range(self.runs.rowCount()):
            if self.runs.item(r, 8).text() == d:
                self.runs.selectRow(r)

    def _select(self):
        r = self.runs.currentRow()
        if r >= 0 and self.runs.item(r, 8):
            self.sel = self.runs.item(r, 8).text()
            self.cls.blockSignals(True)
            self.cls.clear()
            self.cls.addItem("all classes")
            cs = sorted({e["cls"] for e in self.entries(self.sel) if e["cls"]})
            self.cls.addItems(cs)
            self.cls.blockSignals(False)
            self.detail()

    def refresh(self):
        rows = []
        now = time.time()
        for d in self.ctx.run_dirs:
            ents = self.entries(d)
            running = self.ctx.driver_procs_for(d)
            act = runreader.current_activity(ents)
            e, _ = runreader.eta(ents, self.planned(d))
            cores = sum(self.ctx.cpu_rate.get(g, 0) for g in {p["pgid"] for p in running})
            st = runreader.last_start(ents)
            last = ents[-1]["t"] if ents else 0
            el = (now - st["t"]) if st and running else None
            rows.append([os.path.basename(d.rstrip("/")), "RUNNING" if running else "idle", act[0], act[1] + (" p%s" % act[2] if act[2] else ""),
                         "%.1f" % cores if running else "", widgets.age(el) if el is not None else "", widgets.age(e) if e and running else "",
                         widgets.age(now - last) + " ago" if last else "", d])
        rows.sort(key=lambda r: r[1] != "RUNNING")   # stable: running first, the rest newest first
        keep = self.sel
        self.runs.blockSignals(True)
        widgets.fill(self.runs, rows)
        self.runs.blockSignals(False)
        for r in range(self.runs.rowCount()):
            if self.runs.item(r, 1).text() == "RUNNING":
                f = self.runs.item(r, 1).font()
                f.setBold(True)
                self.runs.item(r, 1).setFont(f)
            if keep and self.runs.item(r, 8).text() == keep:
                self.runs.blockSignals(True)
                self.runs.selectRow(r)
                self.runs.blockSignals(False)
        if not self.sel and rows:
            self.select_dir(rows[0][8])
        if self.sel:
            self.detail()

    def detail(self):
        d = self.sel
        if not d:
            return
        ents = self.entries(d)
        running = self.ctx.driver_procs_for(d)
        e, note = runreader.eta(ents, self.planned(d))
        st = runreader.last_start(ents)
        self.head.setText("<b>%s</b>  %s<br>%s" % (os.path.basename(d), "RUNNING (pid %s)" % ",".join(str(p["pid"]) for p in running) if running else "not running",
                                                    "ETA %s (%s)" % (widgets.age(e), note) if running and e else (st["msg"][:200] if st else "")))
        self.stop.setEnabled(bool(running))
        hist = runreader.stage_timings(ents)
        rows = [[s, (v, str(v)), ""] for s, v in hist.items()]
        for name, rc, secs in runreader.parse_phases(os.path.join(d, "phases.txt")):
            rows.append(["phase " + name, (secs, str(secs)), "rc=%d" % rc])
        widgets.fill(self.stages, rows)
        self.render_log()

    def render_log(self, *_):
        d = self.sel
        if not d:
            return
        ents = self.entries(d)
        c = self.cls.currentText()
        t = self.text.text().lower()
        out = []
        for e in ents:
            if self.acc.isChecked() and not (e["kind"] == "decision" or e.get("accepted") or e["kind"] in ("stage", "start")):
                continue
            if c != "all classes" and e["cls"] != c and e["kind"] not in ("stage", "start"):
                continue
            if t and t not in e["raw"].lower():
                continue
            out.append(e["raw"])
        out = out[-4000:]
        bar = self.log.verticalScrollBar()
        at_end = bar.value() >= bar.maximum() - 4
        pos = bar.value()
        self.log.setPlainText("\n".join(out))
        bar.setValue(bar.maximum() if (self.tail.isChecked() and at_end) or self.tail.isChecked() else pos)

    def stop_run(self):
        running = self.ctx.driver_procs_for(self.sel) if self.sel else []
        if not running:
            return
        pg = sorted({p["pgid"] for p in running})
        lines, bad = [], []
        for g in pg:
            ok, mem, why = procs.group_check(g)
            lines += ["group %d:" % g] + ["  %d  %s%s" % (m["pid"], m["cmd"][:110], "" if m["ok"] else "   <- NOT a tuning process") for m in mem]
            if not ok:
                bad.append(why)
        if bad:
            QMessageBox.critical(self, "Stop refused", "Nothing was signalled.\n\n" + "\n".join(bad) + "\n\n" + "\n".join(lines[:40]))
            return
        if QMessageBox.warning(self, "Stop run", "Send SIGTERM to every process of these group(s)?\n\n%s\n\nThe run is resumable: running the same command "
                               "again continues from cache.jsonl." % "\n".join(lines[:40]),
                               QMessageBox.Yes | QMessageBox.Cancel, QMessageBox.Cancel) != QMessageBox.Yes:
            return
        try:
            for g in pg:
                procs.stop_group(g)
        except (OSError, ValueError) as e:
            QMessageBox.critical(self, "Stop failed", str(e))
        self.ctx.refresh()
