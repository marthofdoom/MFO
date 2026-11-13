"""Tuning files: generate from a run's kept values, edit, validate, back up + deploy to LoreRim and/or the Deck, and show the in-game load result."""
import os

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QTextCharFormat, QTextCursor
from PySide6.QtWidgets import (QComboBox, QHBoxLayout, QLabel, QListWidget, QMessageBox, QPlainTextEdit, QPushButton, QSplitter, QTextEdit,
                               QVBoxLayout, QWidget)

from .. import runreader, tuningfiles
from . import bg, widgets


class FilesTab(QWidget):
    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        self.files = {}          # filename -> text (the working set)
        self.cur = None
        lay = QVBoxLayout(self)
        top = QHBoxLayout()
        self.run = QComboBox()
        self.gen = QPushButton("Generate from run's kept values")
        self.gen.clicked.connect(self.generate)
        self.load_lore = QPushButton("Load deployed (LoreRim)")
        self.load_lore.clicked.connect(self.load_deployed)
        self.save = QPushButton("Save working set to disk")
        self.save.clicked.connect(self.save_set)
        top.addWidget(QLabel("Run"))
        top.addWidget(self.run, 1)
        for b in (self.gen, self.load_lore, self.save):
            top.addWidget(b)
        lay.addLayout(top)
        sp = QSplitter(Qt.Horizontal)
        lay.addWidget(sp, 1)
        left = QWidget()
        lv = QVBoxLayout(left)
        self.lst = QListWidget()
        self.lst.currentTextChanged.connect(self.pick)
        lv.addWidget(self.lst, 1)
        nb = QHBoxLayout()
        self.new_name = QComboBox()
        self.new_name.setEditable(True)
        self.new_name.addItems(["shared.txt", "potions.txt", "looting.txt"])
        add = QPushButton("Add file")
        add.clicked.connect(self.add_file)
        nb.addWidget(self.new_name, 1)
        nb.addWidget(add)
        lv.addLayout(nb)
        sp.addWidget(left)
        mid = QWidget()
        mv = QVBoxLayout(mid)
        self.edit = QPlainTextEdit()
        self.edit.setFont(widgets.mono())
        self.edit.textChanged.connect(self.edited)
        mv.addWidget(self.edit, 1)
        self.status = QLabel("")
        self.status.setWordWrap(True)
        mv.addWidget(self.status)
        db = QHBoxLayout()
        self.d_lore = QPushButton("Deploy to LoreRim")
        self.d_deck = QPushButton("Deploy to Deck")
        self.d_both = QPushButton("Deploy to both")
        self.d_lore.clicked.connect(lambda: self.deploy(True, False))
        self.d_deck.clicked.connect(lambda: self.deploy(False, True))
        self.d_both.clicked.connect(lambda: self.deploy(True, True))
        for b in (self.d_lore, self.d_deck, self.d_both):
            db.addWidget(b)
        mv.addLayout(db)
        sp.addWidget(mid)
        right = QWidget()
        rv = QVBoxLayout(right)
        rv.addWidget(QLabel("<b>In-game load result</b> ([tuning] lines of MFO.log)"))
        self.glog = QPlainTextEdit()
        self.glog.setReadOnly(True)
        self.glog.setFont(widgets.mono())
        self.glog.setLineWrapMode(QPlainTextEdit.NoWrap)
        rv.addWidget(self.glog, 1)
        rb = QPushButton("Refresh game logs (LoreRim + Deck)")
        rb.clicked.connect(self.refresh_logs)
        rv.addWidget(rb)
        self.out = QPlainTextEdit()
        self.out.setReadOnly(True)
        self.out.setMaximumHeight(130)
        rv.addWidget(QLabel("<b>Deploy results</b>"))
        rv.addWidget(self.out)
        sp.addWidget(right)
        sp.setSizes([180, 560, 460])
        ctx.changed.connect(self.reload_runs)
        self.reload_runs()

    def reload_runs(self):
        dirs = [d for d in self.ctx.run_dirs if os.path.exists(os.path.join(d, "summary.json"))]
        if [self.run.itemData(i) for i in range(self.run.count())] == dirs:
            return
        cur = self.run.currentData()
        self.run.clear()
        for d in dirs:
            self.run.addItem(os.path.basename(d), d)
        i = self.run.findData(cur)
        if i >= 0:
            self.run.setCurrentIndex(i)

    def set_files(self, files):
        self.files = dict(files)
        self.lst.clear()
        self.lst.addItems(sorted(self.files))
        if self.lst.count():
            self.lst.setCurrentRow(0)
        self.out.appendPlainText("working set: %d file(s)" % len(self.files))

    def generate(self):
        d = self.run.currentData()
        if not d:
            return
        s = runreader.summary(d)
        kept, sets = runreader.kept_specs(s), runreader.accepted_sets(s)
        hdr = "generated from run %s (%s) by MFO Tuning Lab; one spec per line" % (os.path.basename(d), "joint table" if (s.get("stages", {}).get("joint") or {}).get("table") else "kept values")
        files = tuningfiles.generate(kept, sets, hdr, runreader.covered_classes(s))
        if not files:
            QMessageBox.information(self, "Nothing kept", "This run has no kept (accepted) values and no finished class.")
            return
        self.set_files(files)
        dropped = runreader.recalib_dropped(s)
        if dropped:
            self.out.appendPlainText("OPEN DECISIONS (not written: they do not hold under the re-measured potion worth):\n  " + "\n  ".join(dropped))

    def load_deployed(self):
        f = tuningfiles.list_dir_local(self.ctx.s["lorerim_tuning"])
        if not f:
            QMessageBox.information(self, "Nothing deployed", "No .txt files in %s" % self.ctx.s["lorerim_tuning"])
            return
        self.set_files(f)

    def add_file(self):
        n = self.new_name.currentText().strip()
        if not n.endswith(".txt"):
            n += ".txt"
        if "/" in n or n.startswith("."):
            return
        self.files.setdefault(n, "")
        self.lst.clear()
        self.lst.addItems(sorted(self.files))
        self.lst.setCurrentItem(self.lst.findItems(n, Qt.MatchExactly)[0])

    def pick(self, name):
        self.cur = name or None
        self.edit.blockSignals(True)
        self.edit.setPlainText(self.files.get(name, ""))
        self.edit.blockSignals(False)
        self.validate()

    def edited(self):
        if self.cur:
            self.files[self.cur] = self.edit.toPlainText()
        self.validate()

    def validate(self):
        if not self.cur:
            self.status.setText("")
            return
        errs = tuningfiles.validate_text(self.cur, self.edit.toPlainText())
        sel = []
        for ln, _ in errs:
            c = QTextCursor(self.edit.document().findBlockByLineNumber(ln - 1))
            c.select(QTextCursor.LineUnderCursor)
            e = QTextEdit.ExtraSelection()
            e.cursor = c
            fmt = QTextCharFormat()
            fmt.setBackground(QColor(200, 60, 60, 90))
            e.format = fmt
            sel.append(e)
        self.edit.setExtraSelections(sel)
        n = sum(1 for ln in self.edit.toPlainText().splitlines() if ln.split("#", 1)[0].strip())
        self.status.setText("%s: %d spec(s), %s" % (self.cur, n, "valid" if not errs else "; ".join("line %d: %s" % e for e in errs[:6])))

    def all_errors(self):
        out = []
        for fn, t in self.files.items():
            out += ["%s line %d: %s" % (fn, ln, m) for ln, m in tuningfiles.validate_text(fn, t)]
        return out

    def save_set(self):
        d = os.path.join(self.ctx.s["scratch_files"], self.run.currentText() or "manual")
        res = tuningfiles.deploy_local(self.files, d)
        self.out.appendPlainText("saved %d file(s) to %s" % (len(res), d))

    def deploy(self, lore, deck):
        errs = self.all_errors()
        if not self.files:
            QMessageBox.information(self, "Nothing to deploy", "The working set is empty.")
            return
        if errs:
            QMessageBox.warning(self, "Invalid files", "Fix these first:\n" + "\n".join(errs[:15]))
            return
        tg = []
        if lore:
            tg.append("LoreRim: " + self.ctx.s["lorerim_tuning"])
        if deck:
            tg.append("Deck (%s): %s" % (self.ctx.s["deck_host"], self.ctx.s["deck_tuning"]))
        if QMessageBox.question(self, "Deploy", "Write %d file(s):\n  %s\n\nto\n  %s\n\nExisting files are backed up first (name.bak-<time>)." % (
                len(self.files), ", ".join(sorted(self.files)), "\n  ".join(tg))) != QMessageBox.Yes:
            return
        files = dict(self.files)
        s = self.ctx.s

        def work():
            r = {}
            if lore:
                r["LoreRim"] = tuningfiles.deploy_local(files, s["lorerim_tuning"])
            if deck:
                try:
                    r["Deck"] = tuningfiles.deploy_deck(files, s["deck_host"], s["deck_tuning"], os.path.join(s["scratch_files"], ".deck-stage"))
                except RuntimeError as e:
                    r["Deck"] = str(e)
            return r

        def fin(res, err):
            if err:
                self.out.appendPlainText("ERROR: %s" % err)
                return
            for k, v in res.items():
                if isinstance(v, str):
                    self.out.appendPlainText("%s: FAILED: %s" % (k, v))
                    if k == "Deck" and QMessageBox.question(self, "Deck unavailable", v + "\n\nRetry now?") == QMessageBox.Yes:
                        self.deploy(False, True)
                else:
                    for fn, st, bak in v:
                        self.out.appendPlainText("%s: %s %s%s" % (k, fn, st, " (backup %s)" % bak if bak else ""))
            self.refresh_logs()
        bg.run_bg(work, fin)

    def refresh_logs(self):
        s = self.ctx.s

        def work():
            lore = tuningfiles.tuning_log_lines(tuningfiles.read_log_local(s["lorerim_log"]))
            deck = tuningfiles.read_log_deck(s["deck_host"], s["deck_log"])
            return lore, deck

        def fin(res, err):
            if err:
                self.glog.setPlainText(str(err))
                return
            lore, deck = res
            t = "== LoreRim (%s) ==\n%s\n\n== Deck ==\n%s" % (s["lorerim_log"], "\n".join(lore) or "(no [tuning] lines)",
                                                            "(Deck unreachable: asleep? press refresh after waking it)" if deck is None else
                                                            "\n".join(tuningfiles.tuning_log_lines(deck)) or "(no [tuning] lines)")
            self.glog.setPlainText(t)
        bg.run_bg(work, fin)
