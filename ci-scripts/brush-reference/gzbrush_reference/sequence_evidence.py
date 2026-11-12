"""Bounded raw observed labels plus descriptive lag/turn evidence, never a fit."""
import base64
import hashlib
import json
import zlib


def summarize(labels, row_size=0):
    if not isinstance(labels, list) or len(labels) not in (1, 12, 32, 3072):
        raise ValueError('Unexpected bounded frame sequence length')
    if any(value is not None and (type(value) is not int or value not in (0, 1, 2)) for value in labels):
        raise ValueError('Invalid observed original frame label')
    lags = {}
    for lag in (1, 2, 3, 4, 8, 16, 32, 63, 64):
        pairs = [(a, b) for a, b in zip(labels, labels[lag:]) if a is not None and b is not None]
        lags[str(lag)] = {'equal': sum(a == b for a, b in pairs), 'pairs': len(pairs)}
    boundaries, interior = [0, 0], [0, 0]
    runs = [0] * 17
    previous, length = None, 0
    for i, value in enumerate(labels):
        if i and previous is not None and value is not None:
            target = boundaries if row_size and i % row_size == 0 else interior
            target[0] += int(previous == value)
            target[1] += 1
        if value is None or value != previous:
            if length: runs[min(16, length)] += 1
            length = 0
        if value is not None: length += 1
        previous = value
    if length: runs[min(16, length)] += 1
    return {'lags': lags, 'rowBoundaryEqualPairs': boundaries, 'interiorEqualPairs': interior,
            'runLengthCountsLastBinAtLeast16': runs}


def emit(case, readback, delivery):
    labels = readback.get('frameSelections')
    if labels is None:
        return
    summary = summarize(labels, 64 if 'labelProbe' in case else 0)
    raw = bytes(3 if value is None else value for value in labels)
    record = {'preset': case['preset'], 'capture': 'frame-sequence', 'slots': len(labels),
              'encoding': 'zlib-base64-u8-labels-null3', 'data': base64.b64encode(zlib.compress(raw)).decode('ascii'),
              'sha256': hashlib.sha256(raw).hexdigest(), 'mode': delivery['mode'],
              'cadence': case.get('cadenceProbe'), 'summary': summary,
              'sourceSelectionModelVerified': False}
    line = json.dumps(record, separators=(',', ':'))
    if len(line) > 3800:
        raise RuntimeError('Frame sequence diagnostic exceeds notice bound')
    print('::notice title=Original GIH sequence::' + line.replace('%', '%25'), flush=True)
