"""Unchanged RCP1 metric/10,000-bootstrap gates for candidate-ready scatter holdouts.

Pending size/amount/pressure acquisitions cannot enter this native comparison
route. Coordinate transforms are fixed by authored paths, never registration.
"""
import json
from pathlib import Path
import sys
from scatter_holdout_contract import candidate_expected, decode_capture, validate_manifest, validate_execution, validate_effective_preset, strict_equal
from scatter_statistics import measure
from stochastic_gate import compare, SEED


def canonical_alpha(item, case, channels):
    rect = case['scatterHoldoutProbe']['rect']
    if rect[2:] != [1280, 256] or case['diameter'] != 8 or case['spacing'] != 4:
        raise ValueError('Only the measured static scatter holdout domain has candidate statistics')
    pixels = decode_capture(item, rect, channels)
    alpha = pixels[3::4] if channels == 4 else pixels
    direction = case['scatterHoldoutProbe']['direction']
    if direction == 'west':
        # Fixed 180-degree path normalization for both source and candidate.
        # No rendered feature determines the transform or its translation.
        return alpha[::-1], 'north'  # Same first authored cell index159 as the retained north layout.
    if direction != 'east':
        raise ValueError('Unknown candidate holdout authored direction')
    return alpha, 'east'


def analyze(root):
    root = Path(root)
    output = root / 'output'
    fixtures = json.loads((output / 'fixtures.json').read_text())
    validate_manifest(fixtures)
    if not candidate_expected(fixtures['group']):
        raise ValueError('Pending scatter scaling/pressure cannot gain native comparison through statistics')
    oracle = json.loads((output / 'oracle-status.json').read_text())
    if oracle.get('status') != 'complete' or oracle.get('version') != '5.3.4.1':
        raise ValueError('Focused scatter source workload is incomplete')
    validate_execution(fixtures, oracle)
    candidate = json.loads((output / 'comparison.json').read_text())
    rows = candidate.get('records', [])
    if type(rows) is not list or len(rows) != 102 or [r.get('preset') for r in rows] != fixtures['presets']:
        raise ValueError('Focused scatter native comparison omitted or reordered original cases')
    measurements = [[], []]
    seeds = []
    for case, source, other in zip(fixtures['cases'], oracle['records'], rows):
        if not strict_equal({key: other.get('inputCase', {}).get(key) for key in case}, case):
            raise ValueError('Candidate focused scatter replay changed original source inputs')
        xml = case['preset'].replace(' ', '_') + '.effective.xml'
        if source.get('effectivePresetXml') != xml:
            raise ValueError('Focused scatter XML identity changed')
        validate_effective_preset(fixtures['group'], case, source, (output / xml).read_bytes())
        if not case['scatterHoldoutProbe']['enabled']:
            continue  # Disabled controls retain the comparator's ordinary full-image gates.
        seeds.append(other.get('candidateSeed'))
        for index, (item, channels) in enumerate(((source['scatterHoldoutEvidence'], 4), (other['stochasticAlpha'], 1))):
            alpha, layout = canonical_alpha(item, case, channels)
            measurements[index].append(measure(alpha, layout))
    if (len(measurements[0]) != 100 or len(measurements[1]) != 100 or
            any(type(seed) is not str or not seed.isascii() or not seed.isdecimal() or len(seed) > 20 or
                not 0 < int(seed) < 2**64 or str(int(seed)) != seed for seed in seeds) or len(set(seeds)) != 100):
        raise ValueError('Focused scatter requires exactly100 independent candidate streams')
    gates = {key: compare([row[key] for row in measurements[0]], [row[key] for row in measurements[1]])
             for key in measurements[0][0]}
    return {'status': 'original-fixture-statistics-passed' if all(g['passed'] for g in gates.values()) else 'statistical-gates-failed',
            'group': fixtures['group'], 'strokesPerApplication': 100, 'bands': 3,
            'bootstrapResamples': 10000, 'analysisSeed': SEED, 'sourceProfileVerified': False,
            'sourceReferenceVerified': False, 'sourceSemanticallySupported': False, 'sourceParityQualified': False,
            'pixelParityClaimed': False, 'sourceRngLawIdentified': False,
            'qualification': 'Focused original static scatter holdout only; source seed identity and full RCP1 qualification remain separate',
            'gates': gates, 'strokeMeasurements': measurements}


if __name__ == '__main__':
    root = Path(sys.argv[1])
    report = analyze(root)
    (root / 'output/scatter-holdout-statistics.json').write_text(json.dumps(report, indent=2, allow_nan=False))
    for metric, gate in report['gates'].items():
        print(('::notice::' if gate['passed'] else '::error::') + metric + ' ' + json.dumps(gate))
    raise SystemExit(0 if report['status'] == 'original-fixture-statistics-passed' else 1)
