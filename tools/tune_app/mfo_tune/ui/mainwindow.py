from PySide6.QtCore import QTimer
from PySide6.QtWidgets import QMainWindow, QTabWidget

from .ctx import Ctx
from .current_tab import CurrentTab
from .files_tab import FilesTab
from .launch_tab import LaunchTab
from .oracle_tab import OracleTab
from .settings_tab import SettingsTab
from .simrun_tab import SimRunTab
from .tunings_tab import TuningsTab


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("MFO Tuning Lab")
        self.resize(1360, 860)
        self.ctx = Ctx()
        self.tabs = QTabWidget()
        self.tabs.setDocumentMode(True)
        self.setCentralWidget(self.tabs)
        self.current = CurrentTab(self.ctx)
        self.launch = LaunchTab(self.ctx, self.show_run)
        self.tabs.addTab(self.launch, "Launch run")
        self.tabs.addTab(self.current, "Current runs")
        self.tabs.addTab(TuningsTab(self.ctx), "Tunings")
        self.tabs.addTab(OracleTab(self.ctx), "Vs oracle")
        self.tabs.addTab(SimRunTab(self.ctx), "Oracle / scenario runs")
        self.tabs.addTab(FilesTab(self.ctx), "Tuning files")
        self.tabs.addTab(SettingsTab(self.ctx), "Settings")
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.ctx.refresh)
        self.timer.start(4000)
        self.ctx.refresh()

    def show_run(self, d):
        self.tabs.setCurrentWidget(self.current)
        self.current.select_dir(d)
