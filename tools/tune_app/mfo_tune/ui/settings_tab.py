"""Settings: paths (repo, simulator, runs root, installs), persisted in ~/.config/mfo-tune-app/settings.json."""
import os

from PySide6.QtWidgets import QFormLayout, QLineEdit, QPlainTextEdit, QPushButton, QSpinBox, QVBoxLayout, QWidget

from .. import settings
from . import widgets

FIELDS = [("repo", "MFO repo (or worktree)", True), ("sim_bin", "Simulator binary", False), ("runs_root", "Runs root", True),
          ("lorerim_tuning", "LoreRim MFO_Tuning dir", True), ("lorerim_log", "LoreRim MFO.log", False), ("deck_host", "Deck ssh host", None),
          ("deck_tuning", "Deck MFO_Tuning dir", None), ("deck_log", "Deck MFO.log", None), ("taskset", "Default CPU cores (taskset)", None),
          ("scratch_files", "Working tuning files dir", True)]


class SettingsTab(QWidget):
    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        v = QVBoxLayout(self)
        f = QFormLayout()
        v.addLayout(f)
        self.e = {}
        for key, label, mode in FIELDS:
            le = QLineEdit(ctx.s.get(key, ""))
            self.e[key] = le
            f.addRow(label, widgets.path_row(le, mode) if mode is not None else le)
        self.nice = QSpinBox()
        self.nice.setRange(-20, 19)
        self.nice.setValue(int(ctx.s.get("nice", 10)))
        f.addRow("Default nice", self.nice)
        self.jobs = QSpinBox()
        self.jobs.setRange(1, 128)
        self.jobs.setValue(int(ctx.s.get("jobs") or os.cpu_count() or 4))
        f.addRow("Default jobs", self.jobs)
        self.roots = QPlainTextEdit("\n".join(ctx.s.get("extra_run_roots", [])))
        self.roots.setMaximumHeight(90)
        f.addRow("Extra run roots (one per line)", self.roots)
        b = QPushButton("Save settings")
        b.clicked.connect(self.save)
        v.addWidget(b)
        v.addStretch(1)

    def save(self):
        s = dict(self.ctx.s)
        for k, le in self.e.items():
            s[k] = le.text().strip()
        s["nice"], s["jobs"] = self.nice.value(), self.jobs.value()
        s["extra_run_roots"] = [x.strip() for x in self.roots.toPlainText().splitlines() if x.strip()]
        self.ctx.update_settings(s)
        self.ctx.refresh()
