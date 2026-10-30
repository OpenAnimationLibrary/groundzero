"""Bounded original3072-slot path and actual source color readback."""

def centers():
    return [(16 + 32 * (column if row % 2 == 0 else 63 - column), 16 + 32 * row)
            for row in range(48) for column in range(64)]


def vertices():
    return [[x, 16 + row * 32] for row in range(48)
            for x in ((16, 2032) if row % 2 == 0 else (2032, 16))]


def validate(case):
    if (case.get('labelProbe') != {'slots': 3072, 'bands': 3, 'slotsPerBand': 1024} or
            case.get('sourcePath') != vertices() or case.get('diameter') != 8 or
            case.get('spacing') != 4 or case['resourceProbe']['expectedSlots'] != 3072):
        raise RuntimeError('Unexpected bounded GIH label path')


def capture(raw, width, case):
    validate(case)
    if width != 2048 or len(raw) != 2048 * 2048 * 4:
        raise RuntimeError('Invalid fixed source label canvas')
    labels = []
    for cx, cy in centers():
        sums = [0, 0, 0]
        for y in range(cy - 6, cy + 7):
            for x in range(cx - 6, cx + 7):
                offset = (y * width + x) * 4
                for channel in range(3):
                    sums[channel] += raw[offset + 2 - channel] * raw[offset + 3]
        winner = max(range(3), key=lambda c: sums[c])
        if any(sums[c] for c in range(3) if c != winner):
            raise RuntimeError('Source GIH label cell mixes original primary colors')
        labels.append(winner if sums[winner] else None)
    if not any(value is not None for value in labels):
        raise RuntimeError('Source long label path did not paint')
    return {'frameSelections': labels, 'actualPaintedTipReadback': True,
            'qualification': 'Long-path observed color labels, no seed parity'}
