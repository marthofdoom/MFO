"""Run a blocking function off the GUI thread and deliver its result (or exception) back on it."""
from PySide6.QtCore import QObject, QRunnable, QThreadPool, Signal


class _Sig(QObject):
    done = Signal(object, object)       # result, exception


class _Job(QRunnable):
    def __init__(self, fn, sig):
        super().__init__()
        self.fn, self.sig = fn, sig

    def run(self):
        try:
            self.sig.done.emit(self.fn(), None)
        except Exception as e:           # noqa: BLE001 - delivered to the caller, never swallowed
            self.sig.done.emit(None, e)


_keep = set()


def run_bg(fn, callback):
    sig = _Sig()
    _keep.add(sig)

    def fin(res, err):
        _keep.discard(sig)
        callback(res, err)
    sig.done.connect(fin)
    QThreadPool.globalInstance().start(_Job(fn, sig))
