"""Shared application state: settings, the process scan and the known run directories. Tabs read it; the main window refreshes it on a timer."""
import os

from PySide6.QtCore import QObject, Signal

from .. import catalog, procs, runreader, settings


class Ctx(QObject):
    changed = Signal()            # processes / run list refreshed
    settings_changed = Signal()

    def __init__(self):
        super().__init__()
        self.s = settings.load()
        self.procs = []
        self.run_dirs = []
        self.meter = procs.CpuMeter()
        self.cpu_rate = {}
        self._classes = self._scen = None
        self._flags = {}

    def roots(self):
        return [self.s["runs_root"]] + list(self.s.get("extra_run_roots", []))

    def refresh(self):
        self.procs = procs.scan()
        self.cpu_rate = self.meter.update(self.procs)
        dirs = runreader.discover_runs(self.roots())
        for p in self.procs:                           # a running driver whose --out is outside the scanned roots still shows up
            if p.get("out") and os.path.isdir(p["out"]) and os.path.abspath(p["out"]) not in [os.path.abspath(d) for d in dirs]:
                dirs.append(os.path.abspath(p["out"]))
        def mt(d):
            m = 0
            for fn in ("progress.log", "summary.json", "gap-run.json"):
                try:
                    m = max(m, os.path.getmtime(os.path.join(d, fn)))
                except OSError:
                    pass
            return m
        dirs.sort(key=mt, reverse=True)
        self.run_dirs = dirs
        self.changed.emit()

    def update_settings(self, s):
        self.s = s
        settings.save(s)
        self._classes = self._scen = None
        self._flags = {}
        self.settings_changed.emit()

    def classes(self):
        if self._classes is None:
            self._classes = catalog.classes(self.s["sim_bin"]) if os.path.isfile(self.s["sim_bin"]) else catalog.CLASSES_FALLBACK
        return self._classes

    def scenarios(self):
        if self._scen is None:
            self._scen = catalog.scenarios(self.s["sim_bin"]) if os.path.isfile(self.s["sim_bin"]) else catalog.SCENARIOS_FALLBACK
        return self._scen

    def driver_py(self):
        return os.path.join(self.s["repo"], "tools", "iaus_sim", "tune_driver.py")

    def flags(self):
        d = self.driver_py()
        if d not in self._flags:
            self._flags[d] = catalog.driver_flags(d) if os.path.isfile(d) else set()
        return self._flags[d]

    def driver_procs_for(self, run_dir):
        a = os.path.abspath(run_dir)
        return [p for p in self.procs if p["is_driver"] and p.get("out") and os.path.abspath(p["out"]) == a]

    def is_running(self, run_dir):
        return bool(self.driver_procs_for(run_dir))
