"""Original GIH delivery discriminators; no inferred source RNG model."""
MODES = ('line', 'path-endpoints', 'path-dabs', 'path-halves', 'path-offgrid', 'discrete')
CAUSAL_MODES = ('qt-dabs', 'qt-halves', 'path-corners')


def plans():
    return [(kind, spacing, mode, replicate) for kind in ('incremental', 'random')
            for spacing in (2, 3, 4) for mode in MODES for replicate in range(4)] + [
                (kind, 2, mode, replicate) for kind in ('incremental', 'random')
                for mode in CAUSAL_MODES for replicate in range(4)]


def paths(spacing, mode):
    if spacing not in (2, 3, 4) or mode not in MODES + CAUSAL_MODES:
        raise ValueError('Unknown original GIH cadence')
    step = 8 * spacing
    if mode == 'path-corners':
        if spacing != 2:
            raise ValueError('Corner control requires spacing two')
        points = [[768, 768]]
        for index in range(31):
            x, y = points[-1]
            points.append([x + (step if index % 2 == 0 else 0),
                           y + (step if index % 2 else 0)])
        return [points + [[points[-1][0] + 1, points[-1][1]]]]
    centers = [[16 + i * step, 1024] for i in range(32)]
    end = centers[-1][0] + 1  # Final slot lies inside the stroke, not at its endpoint.
    if mode == 'discrete':
        return [[[x, y], [x + 1, y]] for x, y in centers]
    increment = {'path-dabs': step, 'path-halves': step // 2, 'path-offgrid': 17, 'qt-dabs': step, 'qt-halves': step // 2}.get(mode)
    points = ([[x, 1024] for x in range(16, end, increment)] if increment else [[16, 1024]])
    return [points + [[end, 1024]]]


def configure(case, index):
    kind, spacing, mode, replicate = plans()[index]
    authored = paths(spacing, mode)
    case.update(spacing=spacing, start=authored[0][0], end=authored[-1][-1],
                cadencePaths=authored, cadenceProbe={'mode': mode, 'spacing': spacing,
                                                   'slots': 32, 'replicate': replicate})
    case['cadenceCenters'] = centers(spacing, mode)
    case['resourceProbe'].update(expectedSlots=32, rect=[0, 1000, 2048, 48],
                                qualification='Frame advancement acquisition; no random transition model assumed')
    if mode == 'path-corners':
        case['resourceProbe']['rect'] = [744, 744, 320, 304]
    if mode.startswith('qt-'):
        from sensor_fixtures import trace
        template = trace('east')[0]
        case['events'] = [{**template, 'x': x, 'y': y, 'timeUs': index * 4167}
                          for index, (x, y) in enumerate(authored[0])]
        case['delivery'] = 'Qt tablet events'
        case['canvasCenter'] = [264, 1024]


def centers(spacing, mode):
    if mode == 'path-corners':
        return paths(spacing, mode)[0][:-1]
    return [[16 + index * 8 * spacing, 1024] for index in range(32)]
