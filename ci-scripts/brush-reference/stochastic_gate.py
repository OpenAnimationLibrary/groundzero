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
REPORT_LIMIT = 32 * 1024 * 1024
RECT = [232, 1000, 1041, 48]


def geometry(width, height, diameter, slots, origin_x):
    values = (width, height, diameter, slots, origin_x)
    if any(type(v) is not int for v in values) or values != (1041, 48, 8, 32, 232):
        raise ValueError('Unsupported fixed GIH statistics geometry')


def read_report(path):
    # Bound the read itself, not only a racy preceding stat or parsed objects.
    with path.open('rb') as stream:
        raw = stream.read(REPORT_LIMIT + 1)
    if len(raw) > REPORT_LIMIT:
        raise ValueError('Statistics report exceeds32MiB')
    return json.loads(raw)



def decode(item, channels):
    if type(channels) is not int or channels not in (1, 4):
        raise ValueError('Unsupported statistics channel count')
    expected_encoding = 'zlib-base64-bgra8-row-major' if channels == 4 else 'zlib-base64-alpha8-row-major'
    if item.get('encoding') != expected_encoding:
        raise ValueError('Unexpected statistics raster encoding')
    rect = item['rect']
    if type(rect) is not list or len(rect) != 4 or any(type(v) is not int for v in rect) or rect != RECT:
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
    geometry(width, height, diameter, slots, origin_x)
    if not isinstance(alpha, (bytes, bytearray)) or len(alpha) != width * height:
        raise ValueError("Invalid bounded alpha plane")
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


def frame_measure(selections, width, diameter, slots, origin_x):
    """Observed per-stroke color labels; never infer behavior from seed IDs."""
    geometry(width, 48, diameter, slots, origin_x)
    if type(selections) is not list or len(selections) != slots or any(v is not None and (type(v) is not int or v not in (0, 1, 2)) for v in selections):
        raise ValueError('Invalid observed GIH frame labels')
    result = {}
    for band in range(3):
        lo, hi = width * band // 3, width * (band + 1) // 3
        labels = [value for slot, value in enumerate(selections)
                  if lo <= 256 + slot * diameter * 4 - origin_x < hi]
        if len(labels) < 2:
            raise ValueError('Insufficient frame transitions in spatial band')
        # Missing cells retain their own probability and stay in denominators.
        # Otherwise dropping them could hide an always-empty renderer.
        result[str(band) + '/missingFrameFraction'] = labels.count(None) / len(labels)
        for frame in range(3):
            result[str(band) + '/frameFraction' + str(frame)] = labels.count(frame) / len(labels)
        pairs = list(zip(labels, labels[1:]))
        for a in range(3):
            for b in range(3):
                result[str(band) + '/transitionJoint' + str(a) + str(b)] = pairs.count((a, b)) / len(pairs)
    return result


def long_frame_measure(labels):
    if type(labels) is not list or len(labels) != 3072 or any(v is not None and (type(v) is not int or v not in (0, 1, 2)) for v in labels):
        raise ValueError('Expected3072 observed original color labels')
    result = {}
    for band in range(3):
        values = labels[band * 1024:(band + 1) * 1024]
        result[str(band) + '/missingFrameFraction'] = values.count(None) / 1024
        for frame in range(3):
            result[str(band) + '/frameFraction' + str(frame)] = values.count(frame) / 1024
        pairs = list(zip(values, values[1:]))
        for a in range(3):
            for b in range(3):
                result[str(band) + '/transitionJoint' + str(a) + str(b)] = pairs.count((a, b)) / 1023
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
            'classification': ('within-thresholds' if interval[0] >= -tolerance and interval[1] <= tolerance and sd_pass else
                               'measured-mismatch' if interval[0] > tolerance or interval[1] < -tolerance or not sd_pass else
                               'insufficient-precision'),
            'passed': interval[0] >= -tolerance and interval[1] <= tolerance and sd_pass}


def run(root):
    out = root / 'output'
    fixture = read_report(out / 'fixtures.json')
    group = fixture['group']
    if group not in ('gihstatistics', 'gihlabels'):
        raise ValueError('Unexpected stochastic group')
    oracle = read_report(out / 'oracle-status.json')
    candidate = read_report(out / 'comparison.json')
    source_rows, candidate_rows = oracle['records'], candidate['records']
    if len(source_rows) != 100 or len(candidate_rows) != 100:
        raise ValueError('Incomplete100-stroke comparison')
    if sorted(row.get('executionOrdinal', -1) for row in source_rows) != list(range(100)):
        raise ValueError('Missing randomized source execution evidence')
    if [row['executionOrdinal'] for row in source_rows] == list(range(100)):
        raise ValueError('Source execution was not randomized')
    seeds = []
    for row in candidate_rows:
        value = row['candidateSeed']
        if type(value) is not str or not value.isascii() or not value.isdecimal() or not 1 <= len(value) <= 20:
            raise ValueError('Invalid bounded candidate seed identity')
        seed = int(value)
        if not 0 < seed < 2**64 or str(seed) != value:
            raise ValueError('Noncanonical candidate seed identity')
        seeds.append(seed)
    if len(set(seeds)) != 100:
        raise ValueError('Candidate random streams are not distinct')
    candidates = {row['preset']: row for row in candidate_rows}
    if len(candidates) != 100 or len({row['preset'] for row in source_rows}) != 100:
        raise ValueError('Duplicate stroke identities')
    measures = [[], []]
    for row in source_rows:
        other = candidates.pop(row['preset'])
        if group == 'gihlabels':
            if row.get('labelProbe') != {'slots': 3072, 'bands': 3, 'slotsPerBand': 1024}:
                raise ValueError('Unexpected long-label fixture contract')
            measures[0].append(long_frame_measure(row['resourceReadback']['frameSelections']))
            measures[1].append(long_frame_measure(other['stochasticFrameSelections']))
            continue
        source, native = row['resourceReadback'], other['stochasticAlpha']
        if source['rect'] != native['rect']:
            raise ValueError('Source/candidate statistical regions differ')
        rect = source['rect']
        for index, (item, channels) in enumerate(((source, 4), (native, 1))):
            measurements = measure(decode(item, channels), rect[2], rect[3],
                                   row['diameter'], row['resourceProbe']['expectedSlots'], rect[0])
            measures[index].append(measurements)
    gates = {key: compare([r[key] for r in measures[0]], [r[key] for r in measures[1]])
             for key in measures[0][0]}
    passed = all(item['passed'] for item in gates.values())
    report = {'status': 'fixture-statistics-passed' if passed else 'statistical-gates-failed',
              'strokesPerApplication': 100, 'bootstrapResamples': 10000, 'analysisSeed': SEED,
              'pixelParityClaimed': False, 'verificationScope': ('Original straight-stroke alpha only; separate gihlabels pass also required' if group == 'gihstatistics' else 'Original long-path color-label distribution only; separate gihstatistics pass also required'), 'gates': gates, 'strokeMeasurements': measures}
    (out / 'stochastic-comparison.json').write_text(json.dumps(report, indent=2, allow_nan=False))
    for name, gate in gates.items():
        print(('::notice::' if gate['passed'] else '::error::') + name + ' ' + json.dumps(gate))
    return passed


if __name__ == '__main__':
    sys.exit(0 if run(Path(sys.argv[1])) else 1)
