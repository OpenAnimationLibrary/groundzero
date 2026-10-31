"""Timing-only acquisition failure policy; never inspect rendered pixels."""
import json


class TimingFailure(RuntimeError):
    def __init__(self, detail):
        self.detail = detail
        super().__init__('Paced delivery outside authored bound: ' + json.dumps(detail, separators=(',', ':')))


def check(phase, index, authored_us, elapsed_us, bound_us):
    if (phase not in ('contact', 'release') or type(index) is not int or not 0 <= index <= 65 or
            bound_us not in (20000, 250000)):
        raise RuntimeError('Invalid paced timing policy input')
    late_us = elapsed_us - authored_us
    if late_us > bound_us or late_us < -2000:
        raise TimingFailure({'phase': phase, 'index': index, 'authoredUs': authored_us,
                             'elapsedUs': elapsed_us, 'latenessUs': late_us, 'boundUs': bound_us})


def validate_discarded(entries, case):
    if not isinstance(entries, list) or len(entries) > 1:
        raise RuntimeError('Too many discarded paced acquisitions')
    for entry in entries:
        if entry.get('attempt') != 1 or entry.get('reason') != 'timing-only' or entry.get('cleanup') != 'release-and-leave':
            raise RuntimeError('Invalid discarded paced acquisition provenance')
        detail = entry.get('detail', {})
        phase, index = detail.get('phase'), detail.get('index')
        rows = case['events']
        if phase not in ('contact', 'release') or type(index) is not int or not 0 <= index <= len(rows):
            raise RuntimeError('Invalid discarded paced event index')
        if phase == 'release' and index != len(rows):
            raise RuntimeError('Invalid discarded release index')
        authored = (case['timedProbe']['durationUs'] if phase == 'release'
                    else rows[index]['timeUs'] if index < len(rows) else None)
        if (detail.get('authoredUs') != authored or detail.get('boundUs') != case['timedProbe']['maxLatenessUs'] or
                type(detail.get('elapsedUs')) is not int or
                detail.get('latenessUs') != detail['elapsedUs'] - authored):
            raise RuntimeError('Discarded timing values differ from authored contract')
        try:
            check(phase, index, authored, detail['elapsedUs'], detail['boundUs'])
        except TimingFailure:
            continue
        raise RuntimeError('A within-bound acquisition was discarded')
