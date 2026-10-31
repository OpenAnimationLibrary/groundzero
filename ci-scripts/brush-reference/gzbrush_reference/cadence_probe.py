"""Exact bounded public-API delivery contract for original GIH resources."""
MODES = ('line', 'path-endpoints', 'path-dabs', 'path-halves', 'path-offgrid', 'discrete')


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
    if mode == 'discrete':
        expected = [[[x, y], [x + 1, y]] for x, y in centers]
    else:
        increment = {'path-dabs': step, 'path-halves': step // 2, 'path-offgrid': 17}.get(mode)
        points = ([[x, 1024] for x in range(16, end, increment)] if increment else [[16, 1024]])
        expected = [points + [[end, 1024]]]
    if (case.get('cadencePaths') != expected or case.get('start') != expected[0][0] or
            case.get('end') != expected[-1][-1]):
        raise RuntimeError('Authored GIH cadence path differs from fixed original contract')
    return expected
