"""Original GIH delivery discriminators; no inferred source RNG model."""
MODES = ('line', 'path-endpoints', 'path-dabs', 'path-halves', 'path-offgrid', 'discrete')


def plans():
    return [(kind, spacing, mode, replicate) for kind in ('incremental', 'random')
            for spacing in (2, 3, 4) for mode in MODES for replicate in range(4)]


def paths(spacing, mode):
    if spacing not in (2, 3, 4) or mode not in MODES:
        raise ValueError('Unknown original GIH cadence')
    step = 8 * spacing
    centers = [[16 + i * step, 1024] for i in range(32)]
    end = centers[-1][0] + 1  # Final slot lies inside the stroke, not at its endpoint.
    if mode == 'discrete':
        return [[[x, y], [x + 1, y]] for x, y in centers]
    increment = {'path-dabs': step, 'path-halves': step // 2, 'path-offgrid': 17}.get(mode)
    points = ([[x, 1024] for x in range(16, end, increment)] if increment else [[16, 1024]])
    return [points + [[end, 1024]]]


def configure(case, index):
    kind, spacing, mode, replicate = plans()[index]
    authored = paths(spacing, mode)
    case.update(spacing=spacing, start=authored[0][0], end=authored[-1][-1],
                cadencePaths=authored, cadenceProbe={'mode': mode, 'spacing': spacing,
                                                   'slots': 32, 'replicate': replicate})
    case['resourceProbe'].update(expectedSlots=32, rect=[0, 1000, 2048, 48],
                                qualification='Frame advancement acquisition; no random transition model assumed')
