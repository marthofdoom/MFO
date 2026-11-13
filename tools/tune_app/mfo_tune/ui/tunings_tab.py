"""Tunings: per run and class the knob table (shipped / new / d / se / verdict), the kept list, a diff between two runs, open decisions."""
import os

from PySide6.QtCore import Qt
from PySide6.QtGui import QBrush, QColor
from PySide6.QtWidgets import (QCheckBox, QComboBox, QHBoxLayout, QLabel, QLineEdit, QListWidget, QPlainTextEdit, QSplitter, QTabWidget,
                               QVBoxLayout, QWidget)

from .. import runreader
from . import widgets

GREEN, AMBER = QColor("#2e9e5b"), QColor("#c08a1e")


def spec_key(spec):
    return spec.split("=", 1)[0]


def spec_val(spec):
    return spec.split("=", 1)[1] if "=" in spec else ""


class TuningsTab(QWidget):
    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        self.summ = {}
        self.summ_b = {}
        lay = QVBoxLayout(self)
        top = QHBoxLayout()
        self.run_a = QComboBox()
        self.run_b = QComboBox()
        self.run_b.addItem("(none)", "")
        self.cls = QComboBox()
        for w, lab in ((self.run_a, "Run"), (self.run_b, "Compare with"), (self.cls, "Class")):
            top.addWidget(QLabel(lab))
            top.addWidget(w, 1)
        lay.addLayout(top)
        self.tabs = QTabWidget()
        lay.addWidget(self.tabs, 1)

        kn = QWidget()
        kv = QVBoxLayout(kn)
        fl = QHBoxLayout()
        self.verdict = QComboBox()
        self.verdict.addItems(["all verdicts", "accepted", "raced out", "noise", "holdout", "gates", "cross-world", "knight", "other"])
        self.text = QLineEdit()
        self.text.setPlaceholderText("filter knob / spec")
        self.live = QCheckBox("only knobs with a measured effect (|d| > 0)")
        fl.addWidget(self.verdict)
        fl.addWidget(self.text, 1)
        fl.addWidget(self.live)
        kv.addLayout(fl)
        self.knobs = widgets.table(["Knob", "Spec", "Shipped", "New", "Pass", "d (train)", "se", "d/se", "Verdict", "Why"])
        kv.addWidget(self.knobs, 1)
        self.kept_lbl = QLabel()
        kv.addWidget(self.kept_lbl)
        self.kept = QPlainTextEdit()
        self.kept.setReadOnly(True)
        self.kept.setMaximumHeight(110)
        self.kept.setFont(widgets.mono())
        kv.addWidget(self.kept)
        self.tabs.addTab(kn, "Knobs")

        self.diff = widgets.table(["Spec key", "A", "B", "Change"])
        self.tabs.addTab(self.diff, "Diff of kept tables (A vs B)")
        self.dec = QListWidget()
        self.dec.setWordWrap(True)
        self.tabs.addTab(self.dec, "Open decisions")
        self.final = widgets.table(["Class", "Kept values", "Searched", "Live knobs", "Complete"])
        self.tabs.addTab(self.final, "Per-class overview")

        for w in (self.run_a, self.run_b):
            w.currentIndexChanged.connect(self.load)
        self.cls.currentIndexChanged.connect(self.render)
        self.verdict.currentIndexChanged.connect(self.render)
        self.text.textChanged.connect(self.render)
        self.live.toggled.connect(self.render)
        ctx.changed.connect(self.reload_runs)
        self.reload_runs()

    def reload_runs(self):
        dirs = [d for d in self.ctx.run_dirs if os.path.exists(os.path.join(d, "summary.json"))]
        cur_a, cur_b = self.run_a.currentData(), self.run_b.currentData()
        if [self.run_a.itemData(i) for i in range(self.run_a.count())] == dirs and self.run_b.count() == len(dirs) + 1:
            return
        for w, first in ((self.run_a, False), (self.run_b, True)):
            w.blockSignals(True)
            w.clear()
            if first:
                w.addItem("(none)", "")
            for d in dirs:
                w.addItem(os.path.basename(d), d)
            w.blockSignals(False)
        for w, cur in ((self.run_a, cur_a), (self.run_b, cur_b)):
            i = w.findData(cur) if cur else -1
            if i >= 0:
                w.setCurrentIndex(i)
        self.load()

    def load(self, *_):
        a, b = self.run_a.currentData(), self.run_b.currentData()
        self.summ = runreader.summary(a) if a else {}
        self.summ_b = runreader.summary(b) if b else {}
        self.cls.blockSignals(True)
        cur = self.cls.currentText()
        self.cls.clear()
        self.cls.addItems(runreader.classes_of(self.summ))
        i = self.cls.findText(cur)
        if i >= 0:
            self.cls.setCurrentIndex(i)
        self.cls.blockSignals(False)
        self.render()

    def render(self, *_):
        s = self.summ
        cls = self.cls.currentText()
        rows = []
        want = self.verdict.currentText()
        t = self.text.text().lower()
        for r in runreader.class_rows(s, cls):
            v = runreader.verdict_class(r)
            if want != "all verdicts" and v != want:
                continue
            tr = r.get("train") or [None, None]
            if self.live.isChecked() and not (tr[0]):
                continue
            if t and t not in (r["knob"] + " " + r.get("spec", "")).lower():
                continue
            d, se = tr[0], tr[1]
            z = d / se if d is not None and se else None
            it = widgets.QTableWidgetItem(v)
            if v == "accepted":
                it.setForeground(QBrush(GREEN))
            elif v == "raced out":
                it.setForeground(QBrush(AMBER))
            rows.append([r["knob"], r.get("spec", ""), (r.get("shared"), "%g" % r["shared"] if r.get("shared") is not None else ""),
                         (r.get("best"), "%g" % r["best"] if r.get("best") is not None else ""), (r.get("pass"), str(r.get("pass", ""))),
                         (d, "%+.3f" % d if d is not None else ""), (se, "%.3f" % se if se is not None else ""), (z, "%+.1f" % z if z is not None else ""),
                         it, r.get("why", "")])
        widgets.fill(self.knobs, rows)
        self.knobs.sortByColumn(7, Qt.DescendingOrder)
        c = (s.get("stages", {}).get("classes", {}) or {}).get(cls) or {}
        kept = c.get("kept", [])
        self.kept_lbl.setText("<b>Kept for %s:</b> %d value(s)%s" % (cls, len(kept), "" if c.get("complete", True) else "  (class search incomplete)"))
        self.kept.setPlainText("\n".join(kept) or "(none)")
        self.render_diff()
        self.dec.clear()
        for d in s.get("open_decisions", []):
            self.dec.addItem("[%s] %s\n    %s" % (d.get("cls", ""), d.get("what", ""), d.get("evidence", "")))
        ov = []
        for k, v in (s.get("stages", {}).get("classes", {}) or {}).items():
            ov.append([k, (len(v.get("kept", [])), str(len(v.get("kept", [])))), (len(v.get("rows", [])), str(len(v.get("rows", [])))),
                       (len(v.get("live", [])), str(len(v.get("live", [])))), "yes" if v.get("complete") else "no"])
        widgets.fill(self.final, ov)

    def render_diff(self):
        a = {spec_key(x): spec_val(x) for x in runreader.kept_specs(self.summ)}
        b = {spec_key(x): spec_val(x) for x in runreader.kept_specs(self.summ_b)} if self.summ_b else {}
        rows = []
        if self.summ_b:
            for k in sorted(set(a) | set(b)):
                ch = "same" if a.get(k) == b.get(k) else "only in A" if k not in b else "only in B" if k not in a else "changed"
                if ch != "same":
                    rows.append([k, a.get(k, ""), b.get(k, ""), ch])
        widgets.fill(self.diff, rows)
