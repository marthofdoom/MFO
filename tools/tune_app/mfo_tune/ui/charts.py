"""Two small QPainter charts (no extra dependency): horizontal BarChart (1-2 series per label) and LineChart. Colours follow the palette."""
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFontMetrics, QPainter, QPen
from PySide6.QtWidgets import QSizePolicy, QToolTip, QWidget

SERIES = [QColor("#3b82f6"), QColor("#f59e0b"), QColor("#10b981"), QColor("#ef4444"), QColor("#8b5cf6"), QColor("#14b8a6")]


def _fmt(v):
    return "%.1f" % v if abs(v) >= 10 else "%.2f" % v


class BarChart(QWidget):
    """rows: list of (label, [values...], [errors...] or None). series: list of names."""
    ROW = 22

    def __init__(self, parent=None):
        super().__init__(parent)
        self.rows, self.series, self.title, self.unit = [], [], "", ""
        self.setMouseTracking(True)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.MinimumExpanding)

    def set_data(self, rows, series=("",), title="", unit=""):
        self.rows, self.series, self.title, self.unit = rows, list(series), title, unit
        n = max(1, len(self.series))
        self.setMinimumHeight(40 + len(rows) * (self.ROW * (1 if n == 1 else 1.6)))
        self.update()

    def _geom(self):
        fm = QFontMetrics(self.font())
        lw = max([fm.horizontalAdvance(r[0]) for r in self.rows] + [40]) + 12
        return lw, 30, self.width() - lw - 70

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        pal = self.palette()
        fg, grid = pal.color(pal.ColorRole.WindowText), pal.color(pal.ColorRole.Mid)
        p.setPen(fg)
        p.drawText(4, 14, self.title)
        if not self.rows:
            p.drawText(self.rect(), Qt.AlignCenter, "no data")
            return
        lw, top, pw = self._geom()
        pw = max(40, pw)
        vals = [v for r in self.rows for v in r[1] if v is not None]
        lo, hi = min(0.0, min(vals, default=0)), max(0.0, max(vals, default=1))
        if hi == lo:
            hi = lo + 1
        x0 = lw + pw * (-lo / (hi - lo))
        n = max(1, len(self.series))
        rh = self.ROW * (1 if n == 1 else 1.6)
        p.setPen(QPen(grid, 1))
        p.drawLine(QPointF(x0, top - 4), QPointF(x0, top + rh * len(self.rows)))
        for i, (label, vs, errs) in enumerate(self.rows):
            y = top + i * rh
            p.setPen(fg)
            p.drawText(QRectF(0, y, lw - 8, rh), Qt.AlignRight | Qt.AlignVCenter, label)
            bh = (rh - 6) / n
            for k, v in enumerate(vs):
                if v is None:
                    continue
                w = pw * (v - 0) / (hi - lo)
                r = QRectF(min(x0, x0 + w), y + 3 + k * bh, abs(w), max(2, bh - 2))
                p.fillRect(r, SERIES[k % len(SERIES)])
                if errs and errs[k]:
                    e = pw * errs[k] / (hi - lo)
                    p.setPen(QPen(fg, 1))
                    xe = x0 + w
                    yc = r.center().y()
                    p.drawLine(QPointF(max(lw, xe - e), yc), QPointF(min(lw + pw + 50, xe + e), yc))
                p.setPen(fg)
                tx = x0 + w + 4 if v >= 0 else x0 + 4
                p.drawText(QRectF(tx, r.top() - 2, 60, bh + 4), Qt.AlignLeft | Qt.AlignVCenter, _fmt(v))
        if len(self.series) > 1:
            x = lw
            for k, name in enumerate(self.series):
                p.fillRect(QRectF(x, 18, 10, 8), SERIES[k % len(SERIES)])
                p.setPen(fg)
                p.drawText(QPointF(x + 14, 26), name)
                x += QFontMetrics(self.font()).horizontalAdvance(name) + 34

    def mouseMoveEvent(self, ev):
        lw, top, pw = self._geom()
        n = max(1, len(self.series))
        rh = self.ROW * (1 if n == 1 else 1.6)
        i = int((ev.position().y() - top) // rh)
        if 0 <= i < len(self.rows):
            lab, vs, _ = self.rows[i]
            QToolTip.showText(ev.globalPosition().toPoint(), "%s\n%s" % (lab, "\n".join("%s %s" % (self.series[k] if k < len(self.series) else "", _fmt(v)) for k, v in enumerate(vs) if v is not None)), self)


class LineChart(QWidget):
    """series: list of (name, [(x_label, y)...]) with a shared ordered x axis given as labels."""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.series, self.xlabels, self.title = [], [], ""
        self.setMinimumHeight(180)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)

    def set_data(self, xlabels, series, title=""):
        self.xlabels, self.series, self.title = xlabels, series, title
        self.update()

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        pal = self.palette()
        fg, grid = pal.color(pal.ColorRole.WindowText), pal.color(pal.ColorRole.Mid)
        p.setPen(fg)
        p.drawText(4, 14, self.title)
        ys = [y for _, pts in self.series for _, y in pts if y is not None]
        if not ys or not self.xlabels:
            p.drawText(self.rect(), Qt.AlignCenter, "no data")
            return
        lo, hi = min(ys), max(ys)
        pad = (hi - lo) * 0.1 or 1
        lo, hi = lo - pad, hi + pad
        L, T, R, B = 46, 28, self.width() - 12, self.height() - 34
        p.setPen(QPen(grid, 1))
        for k in range(5):
            y = T + (B - T) * k / 4
            p.drawLine(QPointF(L, y), QPointF(R, y))
            p.setPen(fg)
            p.drawText(QRectF(0, y - 8, L - 4, 16), Qt.AlignRight | Qt.AlignVCenter, _fmt(hi - (hi - lo) * k / 4))
            p.setPen(QPen(grid, 1))
        n = len(self.xlabels)
        xs = {lab: L + (R - L) * (i / max(1, n - 1) if n > 1 else 0.5) for i, lab in enumerate(self.xlabels)}
        p.setPen(fg)
        step = max(1, n // 8)
        for i, lab in enumerate(self.xlabels):
            if i % step == 0:
                p.drawText(QRectF(xs[lab] - 40, B + 4, 80, 28), Qt.AlignHCenter | Qt.AlignTop, lab)
        for k, (name, pts) in enumerate(self.series):
            col = SERIES[k % len(SERIES)]
            p.setPen(QPen(col, 2))
            last = None
            for lab, y in pts:
                if y is None or lab not in xs:
                    continue
                pt = QPointF(xs[lab], B - (B - T) * (y - lo) / (hi - lo))
                if last:
                    p.drawLine(last, pt)
                p.setBrush(col)
                p.drawEllipse(pt, 3, 3)
                last = pt
            p.setPen(fg)
            p.drawText(R - 140, T + 12 + 14 * k, "— " + name)
