"""Bounded public Qt GUI-thread timer observations, never renderer inspection."""
import time
from PyQt5.QtCore import QObject, QEvent
from PyQt5.QtWidgets import QApplication


class Recorder(QObject):
    def __init__(self):
        super().__init__()
        self.rows = []
        self.dropped = 0
        self.total = 0

    def eventFilter(self, receiver, event):
        if event.type() != QEvent.Timer:
            return False
        self.total += 1
        # The harness uses short single-shot GUI timers. Retain matching source
        # rate intervals and non-QTimer receivers (possible basic timers).
        interval = receiver.property('interval')
        if type(interval) is int and interval not in (50, 200):
            return False
        if len(self.rows) >= 512:
            self.dropped += 1
            return False
        self.rows.append({'monotonicUs': time.monotonic_ns() // 1000,
                          'timerId': int(event.timerId()),
                          'receiverClass': receiver.metaObject().className()[:128],
                          'intervalMs': interval if type(interval) is int else None})
        return False  # Observe without consuming the source event.


def observe(operation):
    app = QApplication.instance()
    recorder = Recorder()
    app.installEventFilter(recorder)
    try:
        delivery = yield from operation
        delivery['qtTimerObservation'] = {
            'scope': 'GUI-thread Qt timers: interval 50/200ms or no interval property',
            'totalTimerEvents': recorder.total, 'droppedMatchingEvents': recorder.dropped,
            'events': recorder.rows,
            'sourceTimerIdentified': False}
        return delivery
    finally:
        app.removeEventFilter(recorder)
        recorder.deleteLater()
