"""RCP1 independent-stroke statistical gate; stdlib only, bounded inputs."""
import base64
import json
import math
import random
import statistics
import sys
import zlib
from pathlib import Path

SEED = 0x52435031


def decode(item, channels):
    expected_encoding = 'zlib-base64-bgra8-row-major' if channels == 4 else 'zlib-base64-alpha8-row-major'
    if item.get('encoding') != expected_encoding:
        raise ValueError('Unexpected statistics raster encoding')
    rect = item['rect']
    if len(rect) != 4 or any(type(v) is not int for v in rect) or not (0 < rect[2] <= 2048 and 0 < rect[3] <= 128):
        raise ValueError('Invalid bounded statistics region')
    size = rect[2] * rect[3] * channels
    if len(item['data']) > 2 * size + 1024:
        raise ValueError('Oversized statistics encoding')
    decoder = zlib.decompressobj()
    raw = decoder.decompress(base64.b64decode(item['data'], validate=True), size + 1)
    if len(raw) != size or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
        raise ValueError('Invalid statistics raster length/stream')
    return raw[3::4] if channels == 4 else raw


def measure(alpha, width, height, diameter, slots, origin_x):
    result = {}
    for band in range(3):
        lo, hi = width * band // 3, width * (band + 1) // 3
        w = hi - lo
        a = [alpha[y * width + x] / 255 for y in range(height) for x in range(lo, hi)]
        coverage = sum(v > 0 for v in a)
        columns = [sum(a[y * w + x] > 0 for y in range(height)) for x in range(w)]
        occupied = sum(v > 0 for v in columns)
        # Count occupied authored tip cells, not color classes or connected
        # components (an original frame may contain disconnected islands).
        cell_count = cell_total = 0
        for slot in range(slots):
            cx = 256 + slot * diameter * 4 - origin_x
            if not lo <= cx < hi:
                continue
            cell_total += 1
            radius = math.ceil(diameter / 2) + 2
            cell_count += any(alpha[y * width + x] for y in range(height)
                              for x in range(max(0, cx - radius), min(width, cx + radius + 1)))
        if not cell_total:
            raise ValueError('Empty authored tip band')
        values = {'coverage': coverage / len(a), 'integratedAlpha': sum(a) / len(a),
                  'meanWidth': coverage / max(1, occupied) / diameter,
                  'occupiedTips': cell_count / cell_total}
        for scale in (1, 2, 4, 8):
            differences = ((a[y * w + x] - a[y * w + x + scale]) ** 2
                           for y in range(height) for x in range(w - scale))
            vertical = ((a[y * w + x] - a[(y + scale) * w + x]) ** 2
                        for y in range(height - scale) for x in range(w))
            count = height * (w - scale) + (height - scale) * w
            values['energy' + str(scale)] = (sum(differences) + sum(vertical)) / count
        for key, value in values.items():
            result[str(band) + '/' + key] = value
    return result


def compare(reference, candidate, resamples=10000):
    if len(reference) != 100 or len(candidate) != 100 or not 100 <= resamples <= 10000:
        raise ValueError('Expected100 independent strokes and bounded bootstrap count')
    if any(not math.isfinite(v) for v in reference + candidate):
        raise ValueError('Nonfinite statistic')
    rm, cm = statistics.mean(reference), statistics.mean(candidate)
    rs, cs = statistics.stdev(reference), statistics.stdev(candidate)
    rng = random.Random(SEED)
    differences = sorted(sum(candidate[rng.randrange(100)] - reference[rng.randrange(100)]
                             for _ in range(100)) / 100 for _ in range(resamples))
    # Conservative order-statistic endpoints, no interpolation narrowing.
    interval = [differences[math.floor(.025 * (resamples - 1))],
                differences[math.ceil(.975 * (resamples - 1))]]
    tolerance = max(.01, .05 * abs(rm))
    ratio = cs / rs if rs else (1 if not cs else None)
    # Contract says variances below floor, not standard deviations below floor.
    variance_exempt = rs * rs < .01 and cs * cs < .01
    sd_pass = variance_exempt or (ratio is not None and .8 <= ratio <= 1.25)
    return {'referenceMean': rm, 'candidateMean': cm, 'meanDifferenceCI95': interval,
            'tolerance': tolerance, 'referenceSD': rs, 'candidateSD': cs,
            'sdRatio': ratio, 'varianceFloorExemption': variance_exempt,
            'passed': interval[0] >= -tolerance and interval[1] <= tolerance and sd_pass}


def run(root):
    out = root / 'output'
    oracle = json.loads((out / 'oracle-status.json').read_text())
    candidate = json.loads((out / 'comparison.json').read_text())
    source_rows, candidate_rows = oracle['records'], candidate['records']
    if len(source_rows) != 100 or len(candidate_rows) != 100:
        raise ValueError('Incomplete100-stroke comparison')
    if sorted(row.get('executionOrdinal', -1) for row in source_rows) != list(range(100)):
        raise ValueError('Missing randomized source execution evidence')
    if [row['executionOrdinal'] for row in source_rows] == list(range(100)):
        raise ValueError('Source execution was not randomized')
    if len({row['candidateSeed'] for row in candidate_rows}) != 100:
        raise ValueError('Candidate random streams are not distinct')
    candidates = {row['preset']: row for row in candidate_rows}
    if len(candidates) != 100 or len({row['preset'] for row in source_rows}) != 100:
        raise ValueError('Duplicate stroke identities')
    measures = [[], []]
    for row in source_rows:
        other = candidates.pop(row['preset'])
        source, native = row['resourceReadback'], other['stochasticAlpha']
        if source['rect'] != native['rect']:
            raise ValueError('Source/candidate statistical regions differ')
        rect = source['rect']
        for index, (item, channels) in enumerate(((source, 4), (native, 1))):
            measures[index].append(measure(decode(item, channels), rect[2], rect[3],
                                           row['diameter'], row['resourceProbe']['expectedSlots'], rect[0]))
    gates = {key: compare([r[key] for r in measures[0]], [r[key] for r in measures[1]])
             for key in measures[0][0]}
    passed = all(item['passed'] for item in gates.values())
    report = {'status': 'statistical-reference-verified' if passed else 'statistical-gates-failed',
              'strokesPerApplication': 100, 'bootstrapResamples': 10000, 'analysisSeed': SEED,
              'pixelParityClaimed': False, 'gates': gates, 'strokeMeasurements': measures}
    (out / 'stochastic-comparison.json').write_text(json.dumps(report, indent=2, allow_nan=False))
    for name, gate in gates.items():
        print(('::notice::' if gate['passed'] else '::error::') + name + ' ' + json.dumps(gate))
    return passed


if __name__ == '__main__':
    sys.exit(0 if run(Path(sys.argv[1])) else 1)
