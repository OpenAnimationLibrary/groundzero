"""Bound and qualify actual paced source delivery, without assumed timer math."""
import base64
import hashlib
import json
import zlib
from xml.etree.ElementTree import fromstring
from PyQt5.QtCore import QEvent


def validate(case):
    spec, rows = case['timedProbe'], case['events']
    if (spec['durationUs'] not in (250000, 375000, 1000000, 1125000) or
            spec['maxLatenessUs'] != (20000 if spec['durationUs'] in (375000, 1125000) else 250000) or
            len(rows) not in (1, 33)):
        raise RuntimeError('Invalid bounded paced acquisition')
    if rows[0]['timeUs'] != 0 or any(type(r['timeUs']) is not int or not 0 <= r['timeUs'] <= spec['durationUs'] for r in rows):
        raise RuntimeError('Invalid paced event deadline')
    if any(b['timeUs'] <= a['timeUs'] for a, b in zip(rows, rows[1:])):
        raise RuntimeError('Nonmonotonic paced event deadlines')


def capture(case, fields, delivery, raw, width):
    validate(case)
    controls = {}
    for key, wanted in case['overrides'].items():
        actual = fields.get(key)
        if key == 'SizeSensor':
            a, b = fromstring(actual), fromstring(wanted)
            if a.tag != b.tag or a.attrib != b.attrib or a.findtext('curve') != b.findtext('curve'):
                raise RuntimeError('Effective timed sensor metadata differs')
        elif actual != wanted:
            raise RuntimeError('Effective timed control differs: ' + key)
        controls[key] = actual
    contacts = [event for event in delivery['events'] if event['type'] in (int(QEvent.TabletPress), int(QEvent.TabletMove)) and event['pressure'] > 0]
    releases = [event for event in delivery['events'] if event['type'] == int(QEvent.TabletRelease)]
    if len(contacts) != len(case['events']) or len(releases) != 1:
        raise RuntimeError('Paced delivery contact/release count differs')
    start = contacts[0]['monotonicUs']
    for authored, observed in zip(case['events'], contacts):
        lateness = observed['monotonicUs'] - start - authored['timeUs']
        if not -2000 <= lateness <= case['timedProbe']['maxLatenessUs']:
            raise RuntimeError('Actual paced event outside lateness bound')
        if observed['image'] != [authored['x'], authored['y']] or observed['pressure'] != 1:
            raise RuntimeError('Paced contact coordinates/pressure differ')
        if abs((observed['timeMs'] - contacts[0]['timeMs']) * 1000 - (observed['monotonicUs'] - start)) > 2000:
            raise RuntimeError('Paced device timestamp differs from measured real clock')
    elapsed = releases[0]['monotonicUs'] - start
    if not case['timedProbe']['durationUs'] - 2000 <= elapsed <= case['timedProbe']['durationUs'] + case['timedProbe']['maxLatenessUs']:
        raise RuntimeError('Actual press/hold/release duration outside bound')
    left, top, w, h = case['timedProbe']['rect']
    pixels = b''.join(raw[(y * width + left) * 4:(y * width + left + w) * 4] for y in range(top, top + h))
    if sum(v != 0 for v in pixels[3::4]) != sum(v != 0 for v in raw[3::4]):
        raise RuntimeError('Timed stroke escaped captured region')
    return {'effectiveControls': controls, 'actualElapsedUs': elapsed, 'deliveredContacts': contacts,
            'release': releases[0], 'actualTimingVerified': True, 'sourceParityQualified': False,
            'rect': case['timedProbe']['rect'], 'encoding': 'zlib-base64-bgra8-row-major',
            'data': base64.b64encode(zlib.compress(pixels)).decode(),
            'sha256Pixels': hashlib.sha256(pixels).hexdigest()}


def emit_notices(name, evidence):
    def emit(kind, payload):
        text = json.dumps({'preset': name, 'capture': kind, **payload}, separators=(',', ':'))
        if len(text) > 3900:
            raise RuntimeError('Timed diagnostic exceeds bound')
        print('::notice title=Timed source capture::' + text.replace('%', '%25').replace('\n', '%0A').replace('\r', '%0D'), flush=True)
    emit('controls', {'effectiveControls': evidence['effectiveControls'], 'actualElapsedUs': evidence['actualElapsedUs']})
    events = evidence['deliveredContacts'] + [evidence['release']]
    for offset in range(0, len(events), 3):
        emit('events', {'offset': offset, 'total': len(events), 'events': events[offset:offset + 3]})
    data = evidence['data']
    for offset in range(0, len(data), 3000):
        emit('color-chunk', {'rect': evidence['rect'], 'offset': offset, 'total': len(data), 'encoding': evidence['encoding'], 'data': data[offset:offset + 3000]})
