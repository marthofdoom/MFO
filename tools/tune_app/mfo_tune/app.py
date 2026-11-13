import sys

from PySide6.QtWidgets import QApplication

from .ui.mainwindow import MainWindow


def main(argv=None):
    app = QApplication(argv or sys.argv)
    app.setApplicationName("MFO Tuning Lab")
    app.setDesktopFileName("mfo-tuning-lab")
    w = MainWindow()
    w.show()
    return app.exec()
