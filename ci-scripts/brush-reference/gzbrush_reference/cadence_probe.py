"""Exact bounded public-API delivery contract for original GIH resources."""
MODES = ('line', 'path-endpoints', 'path-dabs', 'path-halves', 'path-offgrid', 'discrete', 'qt-dabs', 'qt-halves', 'path-corners')


def validate(case):
    spec = case.get('cadenceProbe', {})
    spacing, mode = spec.get('spacing'), spec.get('mode')
    if (type(spacing) is not int or spacing not in (2, 3, 4) or mode not in MODES or
            spec.get('slots') != 32 or type(spec.get('replicate')) is not int or
            not 0 <= spec['replicate'] < 4 or case.get('diameter') != 8 or
            case.get('spacing') != spacing or case['resourceProbe']['expectedSlots'] != 32):
        raise RuntimeError('Invalid bounded GIH cadence settings')
    step = 8 * spacing
    centers = [[16 + i * step, 1024] for i in range(32)]
    end = centers[-1][0] + 1
    if mode == 'path-corners':
        if spacing != 2:
            raise RuntimeError('Corner discriminator has unsupported spacing')
        centers = [[768 + ((index + 1) // 2) * step,
                    768 + (index // 2) * step] for index in range(32)]
        expected = [centers + [[centers[-1][0] + 1, centers[-1][1]]]]
    elif mode == 'discrete':
        expected = [[[x, y], [x + 1, y]] for x, y in centers]
    else:
        increment = {'path-dabs': step, 'path-halves': step // 2, 'path-offgrid': 17, 'qt-dabs': step, 'qt-halves': step // 2}.get(mode)
        points = ([[x, 1024] for x in range(16, end, increment)] if increment else [[16, 1024]])
        expected = [points + [[end, 1024]]]
    if case.get('cadenceCenters') != centers:
        raise RuntimeError('GIH readback centers differ from authored path')
    if (case.get('cadencePaths') != expected or case.get('start') != expected[0][0] or
            case.get('end') != expected[-1][-1]):
        raise RuntimeError('Authored GIH cadence path differs from fixed original contract')
    if mode.startswith('qt-'):
        rows = case.get('events', [])
        if (spacing != 2 or len(rows) != len(expected[0]) or
                case.get('delivery') != 'Qt tablet events' or
                case.get('canvasCenter') != [264, 1024] or
                any([row.get('x'), row.get('y')] != point or
                    row.get('timeUs') != index * 4167 or row.get('pressure') != 1 or
                    row.get('tiltX') != 0 or row.get('tiltY') != 0 or row.get('rotation') != 0
                    for index, (row, point) in enumerate(zip(rows, expected[0])))):
            raise RuntimeError('GIH Qt cadence differs from original control')
    return expected
