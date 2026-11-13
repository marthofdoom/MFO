"""Small shared widgets and helpers."""
import os

from PySide6.QtCore import Qt
from PySide6.QtGui import QFont, QFontDatabase
from PySide6.QtWidgets import (QAbstractItemView, QCheckBox, QFileDialog, QHBoxLayout, QHeaderView, QLineEdit, QListWidget, QListWidgetItem,
                               QPushButton, QTableWidget, QTableWidgetItem, QWidget)


def mono():
    return QFontDatabase.systemFont(QFontDatabase.FixedFont)


def table(headers, sortable=True):
    t = QTableWidget(0, len(headers))
    t.setHorizontalHeaderLabels(headers)
    t.setEditTriggers(QAbstractItemView.NoEditTriggers)
    t.setSelectionBehavior(QAbstractItemView.SelectRows)
    t.setAlternatingRowColors(True)
    t.verticalHeader().setVisible(False)
    t.horizontalHeader().setStretchLastSection(True)
    t.horizontalHeader().setSectionResizeMode(QHeaderView.Interactive)
    t.setSortingEnabled(sortable)
    return t


class NumItem(QTableWidgetItem):
    """Sorts numerically while displaying text."""
    def __init__(self, v, text=None):
        super().__init__(text if text is not None else ("" if v is None else "%g" % v))
        self.v = v
        self.setTextAlignment(Qt.AlignRight | Qt.AlignVCenter)

    def __lt__(self, o):
        a = self.v if isinstance(self.v, (int, float)) else float("-inf")
        b = o.v if isinstance(getattr(o, "v", None), (int, float)) else float("-inf")
        return a < b


def fill(t, rows):
    """rows: list of lists of str | (value,text) | QTableWidgetItem."""
    was = t.isSortingEnabled()
    t.setSortingEnabled(False)
    t.setRowCount(len(rows))
    for r, row in enumerate(rows):
        for c, v in enumerate(row):
            if isinstance(v, QTableWidgetItem):
                it = v
            elif isinstance(v, tuple):
                it = NumItem(v[0], v[1])
            else:
                it = QTableWidgetItem("" if v is None else str(v))
            t.setItem(r, c, it)
    t.setSortingEnabled(was)
    t.resizeColumnsToContents()


def checklist(items, checked=()):
    w = QListWidget()
    for it in items:
        li = QListWidgetItem(it)
        li.setFlags(li.flags() | Qt.ItemIsUserCheckable)
        li.setCheckState(Qt.Checked if it in checked else Qt.Unchecked)
        w.addItem(li)
    return w


def checked(w):
    return [w.item(i).text() for i in range(w.count()) if w.item(i).checkState() == Qt.Checked]


def set_checked(w, names):
    for i in range(w.count()):
        w.item(i).setCheckState(Qt.Checked if w.item(i).text() in names else Qt.Unchecked)


def path_row(edit, dir_mode=False, parent=None):
    box = QWidget()
    h = QHBoxLayout(box)
    h.setContentsMargins(0, 0, 0, 0)
    h.addWidget(edit, 1)
    b = QPushButton("Browse…")

    def pick():
        cur = edit.text() or os.path.expanduser("~")
        r = QFileDialog.getExistingDirectory(box, "Choose directory", cur) if dir_mode else QFileDialog.getOpenFileName(box, "Choose file", cur)[0]
        if r:
            edit.setText(r)
    b.clicked.connect(pick)
    h.addWidget(b)
    return box


def age(secs):
    secs = int(max(0, secs))
    if secs < 90:
        return "%ds" % secs
    if secs < 5400:
        return "%dm" % (secs // 60)
    return "%dh%02dm" % (secs // 3600, secs % 3600 // 60)
