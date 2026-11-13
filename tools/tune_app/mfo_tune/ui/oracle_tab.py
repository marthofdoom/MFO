"""Vs oracle: per-class gap to the oracle (bar chart), J headline, metrics table (gap-run.json), compare runs, J over time."""
import os
import time

from PySide6.QtWidgets import QComboBox, QHBoxLayout, QLabel, QSplitter, QTabWidget, QVBoxLayout, QWidget

from .. import runreader
from . import widgets
from .charts import BarChart, LineChart


def class_gaps(g):
    """-> {class: (gap, se)} from gap-run.json (schema 2 class_gaps; falls back to the first classes[] entry per class)."""
    out = {}
    for c in g.get("class_gaps", []) or []:
        out[c["class"]] = (c.get("gap"), c.get("se") or 0)
    if not out:
        for c in g.get("classes", []) or []:
            out.setdefault(c["class"], (c.get("main_gap"), 0))
    return out


class OracleTab(QWidget):
    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        self.g = {}
        self.gb = {}
        lay = QVBoxLayout(self)
        top = QHBoxLayout()
        self.a = QComboBox()
        self.b = QComboBox()
        top.addWidget(QLabel("Run"))
        top.addWidget(self.a, 1)
        top.addWidget(QLabel("Compare with"))
        top.addWidget(self.b, 1)
        lay.addLayout(top)
        self.head = QLabel()
        self.head.setWordWrap(True)
        lay.addWidget(self.head)
        tabs = QTabWidget()
        lay.addWidget(tabs, 1)
        self.bars = BarChart()
        from PySide6.QtWidgets import QScrollArea
        sc = QScrollArea()
        sc.setWidgetResizable(True)
        sc.setWidget(self.bars)
        tabs.addTab(sc, "Gap per class")
        met = QWidget()
        mv = QVBoxLayout(met)
        self.cls = QComboBox()
        mv.addWidget(self.cls)
        self.metrics = widgets.table(["Metric", "Value", "B value"])
        mv.addWidget(self.metrics)
        tabs.addTab(met, "Metrics")
        self.gaps = widgets.table(["Id", "Family", "Class", "Description", "Scenario", "delta U", "Status"])
        tabs.addTab(self.gaps, "Divergences")
        self.tab_gapstage = widgets.table(["Class / world", "Gap", "se", "Fights", "Shipped gap", "Tuning change"])
        tabs.addTab(self.tab_gapstage, "Driver gap stage (shipped vs tuned)")
        self.line = LineChart()
        tabs.addTab(self.line, "J over runs")
        for w in (self.a, self.b):
            w.currentIndexChanged.connect(self.load)
        self.cls.currentIndexChanged.connect(self.render_metrics)
        ctx.changed.connect(self.reload_runs)
        self.reload_runs()

    def reload_runs(self):
        dirs = [d for d in self.ctx.run_dirs if os.path.exists(os.path.join(d, "gap-run.json"))]
        if [self.a.itemData(i) for i in range(self.a.count())] == dirs:
            return
        ca, cb = self.a.currentData(), self.b.currentData()
        for w, first in ((self.a, False), (self.b, True)):
            w.blockSignals(True)
            w.clear()
            if first:
                w.addItem("(none)", "")
            for d in dirs:
                w.addItem(os.path.basename(d), d)
            w.blockSignals(False)
        for w, c in ((self.a, ca), (self.b, cb)):
            i = w.findData(c) if c else -1
            if i >= 0:
                w.setCurrentIndex(i)
        self.load()

    def load(self, *_):
        a, b = self.a.currentData(), self.b.currentData()
        self.g = runreader.gap_run(a) if a else {}
        self.gb = runreader.gap_run(b) if b else {}
        h = self.g.get("headline") or {}
        if h:
            oj = h.get("oracle_j")
            self.head.setText("<b>J %.2f</b> (scenarios %s, natural %s) over %s fights/cell, list %s, run %s.  Oracle J: %s" % (
                h.get("j") or 0, h.get("j_scenarios"), h.get("j_natural"), self.g.get("fights_per_cell"), self.g.get("list"), self.g.get("run_id"),
                "%.2f" % oj if oj else "not measured in this file (the run had no headline oracle)"))
        else:
            self.head.setText("No gap-run.json in this run (it appears after the driver's gap stage or a --class-oracle run).")
        ga, gb = class_gaps(self.g), class_gaps(self.gb)
        names = sorted(ga, key=lambda c: -(ga[c][0] or 0))
        if self.gb:
            rows = [(c, [ga.get(c, (None, 0))[0], gb.get(c, (None, 0))[0]], [ga.get(c, (0, 0))[1], gb.get(c, (0, 0))[1]]) for c in names]
            self.bars.set_data(rows, [os.path.basename(self.a.currentData() or "A"), os.path.basename(self.b.currentData() or "B")], "Gap to oracle (lower is closer), error bar = 1 se", "U")
        else:
            self.bars.set_data([(c, [ga[c][0]], [ga[c][1]]) for c in names], ["gap"], "Gap to oracle (lower is closer), error bar = 1 se", "U")
        self.cls.blockSignals(True)
        cur = self.cls.currentText()
        self.cls.clear()
        self.cls.addItems(sorted({c["class"] for c in self.g.get("classes", [])}))
        i = self.cls.findText(cur)
        if i >= 0:
            self.cls.setCurrentIndex(i)
        self.cls.blockSignals(False)
        self.render_metrics()
        widgets.fill(self.gaps, [[x.get("id"), x.get("family"), x.get("class"), x.get("desc"), x.get("scenario"),
                                  (x.get("delta_u"), "%+.2f" % x["delta_u"] if x.get("delta_u") is not None else ""), x.get("status")] for x in self.g.get("gaps", [])])
        s = runreader.summary(a) if a else {}
        gs = s.get("stages", {}).get("gap")
        rows = []
        if isinstance(gs, dict):
            for k, v in gs.items():
                if isinstance(v, dict) and "gap" in v and "tuning_change" in v:
                    rows.append([k, (v["gap"], "%+.2f" % v["gap"]), (v.get("se"), "%.2f" % (v.get("se") or 0)), v.get("fights"),
                                 (v.get("shipped_gap"), "%+.2f" % v["shipped_gap"]), (v["tuning_change"][0], "%+.2f (se %.2f)" % tuple(v["tuning_change"][:2]))])
        widgets.fill(self.tab_gapstage, rows)
        self.render_line()

    def render_metrics(self, *_):
        c = self.cls.currentText()
        def first(g):
            for x in g.get("classes", []):
                if x["class"] == c:
                    return x.get("metrics", {})
            return {}
        ma, mb = first(self.g), first(self.gb)
        rows = [[k, (ma[k], "%.4g" % ma[k]) if isinstance(ma[k], (int, float)) else str(ma[k]),
                 (mb.get(k), "%.4g" % mb[k]) if isinstance(mb.get(k), (int, float)) else ""] for k in sorted(ma)]
        widgets.fill(self.metrics, rows)

    def render_line(self):
        pts = []
        for d in self.ctx.run_dirs:
            g = runreader.gap_run(d)
            h = g.get("headline") or {}
            if h.get("j") is not None:
                pts.append((g.get("time") or "", os.path.basename(d), h["j"], h.get("j_natural")))
        pts.sort()
        labels = ["%s\n%s" % (p[1][:14], p[0][5:16]) for p in pts]
        self.line.set_data(labels, [("J", list(zip(labels, [p[2] for p in pts]))), ("J natural", list(zip(labels, [p[3] for p in pts])))],
                           "Headline J per run (higher is better), by run time")
