"""Validate N3a engine CPU reports; no source-fidelity or display-latency claim."""
import json
import math
from pathlib import Path
import sys

ORIGINAL = (
    'standard-dry', 'standard-dry-antialias', 'heavy-texture-dual', 'heavy-smear',
    'source-candidate-hard-wash', 'source-candidate-default-circle-aa',
    'legacy-smear', 'legacy-dulling', 'new-smear', 'new-dulling',
)
EXPANDED = ('source-generated-soft65', 'source-generated-gaussian', 'source-scatter',
            'source-random-gih', 'source-new-dulling-radius-half')
NAMES = ORIGINAL + tuple(name + suffix for name in EXPANDED for suffix in ('-first', '-repeat'))
TIMINGS = ('backingRasterSetupMilliseconds', 'bufferedSurfaceSetupMilliseconds',
           'strokeConstructorMilliseconds', 'firstAddMilliseconds', 'renderMilliseconds',
           'eventP95Milliseconds', 'eventP99Milliseconds', 'eventMaxMilliseconds',
           'finishMilliseconds', 'cleanupMilliseconds', 'strokeLifecycleMilliseconds')
FLAGS = ('firstProfileUseInProcess', 'presetDefinitionReused', 'expandedLifecycleWorkload',
         'machineColdCacheClaimed', 'presetResourceCreationTimed', 'presetValidated',
         'constructorAttempted', 'constructorSucceeded', 'finishAttempted', 'finishSucceeded',
         'beforeFinishDabCheckpointRecorded', 'afterFinishDabCheckpointRecorded', 'cleanupDabCheckpointRecorded',
         'peakTrackedStrokeBytesIsSampled', 'constructorAllocationCheckpointRecorded',
         'finishAllocationCheckpointRecorded', 'resourceReleaseChecked', 'eventBudgetPassed',
         'addThroughputPassed', 'lifecycleThroughputPassed', 'cpuBudgetPassed',
         'sourceReferenceVerified', 'displayLatencyMeasured')
COUNTS = ('plannedEvents', 'attemptedEvents', 'events',
          'peakTrackedStrokeBytes',
          'backingRasterBytes', 'inputResourceObjects', 'inputResourcePixelBytes', 'presetCurveKnots')


def require(value, message):
    if not value:
        raise ValueError(message)


def finite(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def expected_profile(name):
    if name in ORIGINAL[:4]: return 'groundzero-native-1'
    if name == 'source-candidate-hard-wash': return 'krita-5.3.4.1-hard-pixel-candidate-1'
    if name == 'source-candidate-default-circle-aa': return 'krita-5.3.4.1-default-circle-candidate-1'
    if name in ORIGINAL[6:]: return 'krita-5.3.4.1-' + name + '-candidate-1'
    if name.startswith('source-generated-'): return 'krita-5.3.4.1-generated-mask-candidate-1'
    if name.startswith('source-scatter-'): return 'krita-5.3.4.1-scatter-candidate-1'
    if name.startswith('source-random-gih-'): return 'krita-5.3.4.1-random-bitmap-candidate-1'
    if name.startswith('source-new-dulling-radius-half-'): return 'krita-5.3.4.1-new-dulling-candidate-1'
    raise ValueError('Unknown CPU workload identity')


def validate(report):
    require(type(report) is dict and type(report.get('schema')) is int and report['schema'] == 2,
            'Missing lifecycle CPU report schema')
    require(report.get('displayLatencyMeasured') is False and report.get('sourceReferenceVerified') is False and
            type(report.get('scope')) is str and 'no GUI/display/tablet latency claim' in report['scope'],
            'Engine timing acquired an unrelated latency or fidelity claim')
    rows = report.get('records')
    require(type(rows) is list and len(rows) == len(NAMES) and all(type(r) is dict for r in rows) and
            [r.get('name') for r in rows] == list(NAMES),
            'CPU report omitted, reordered or replaced original/expanded workloads')
    seen = set()
    failed = 0
    for ordinal, row in enumerate(rows, 1):
        name = row['name']; context = name + ': '
        require(type(row) is dict and row.get('profile') == expected_profile(name), context + 'wrong production profile')
        require(all(type(row.get(key)) is bool for key in FLAGS), context + 'missing or untyped lifecycle truth')
        require(type(row.get('processStrokeOrdinal')) is int and row['processStrokeOrdinal'] == ordinal,
                context + 'missing actual process order')
        require(row['firstProfileUseInProcess'] is (row['profile'] not in seen), context + 'false first-profile-use claim')
        seen.add(row['profile'])
        expanded = name not in ORIGINAL
        repeated = expanded and name.endswith('-repeat')
        require(row['expandedLifecycleWorkload'] is expanded and row['presetDefinitionReused'] is repeated and
                row.get('setupMode') == ('repeat-workload-same-preset-and-resource-objects' if repeated else
                                         'first-use-of-workload-in-process') and
                row['machineColdCacheClaimed'] is False and type(row.get('unmeasuredWarmupStrokes')) is int and
                row['unmeasuredWarmupStrokes'] == 0 and
                row['presetResourceCreationTimed'] is False and
                row.get('rasterState') == 'fresh backing and BufferedSurface per measured stroke' and
                row.get('resourceOrigin') == 'original in-memory benchmark definitions',
                context + 'invalid cold/repeat/resource timing scope')
        heavy = name in ('heavy-texture-dual', 'heavy-smear')
        side, hz, seconds = (4096, 120, 60) if heavy else (2048, 240, 12 if expanded else 60)
        for key, value in (('canvasSide', side), ('inputHz', hz), ('traceSeconds', seconds)):
            require(type(row.get(key)) is int and row[key] == value, context + 'original workload changed: ' + key)
        require(all(type(row.get(key)) is int and row[key] >= 0 for key in COUNTS), context + 'invalid counts/resources')
        require(row['plannedEvents'] == seconds * hz + 1 and 0 <= row['events'] <= row['attemptedEvents'] <= row['plannedEvents'],
                context + 'event progress is not actual')
        for flag, key in (('beforeFinishDabCheckpointRecorded', 'dabsBeforeFinish'),
                          ('afterFinishDabCheckpointRecorded', 'dabsAfterFinish'),
                          ('cleanupDabCheckpointRecorded', 'dabsBeforeCleanup')):
            require((type(row.get(key)) is int and row[key] >= 0) if row[flag] else row.get(key) is None,
                    context + 'unobserved dab checkpoint was invented')
        require(type(row.get('dabs')) is type(row.get('dabsBeforeFinish')) and row['dabs'] == row['dabsBeforeFinish'],
                context + 'legacy dab alias differs from its actual pre-finish checkpoint')
        require(row['beforeFinishDabCheckpointRecorded'] is row['finishAttempted'] and
                row['afterFinishDabCheckpointRecorded'] is row['finishAttempted'],
                context + 'finish dab observations differ from actual finish attempt')
        require(not row['constructorSucceeded'] or row['cleanupDabCheckpointRecorded'],
                context + 'constructed stroke progress was not read before cleanup')
        require(row['constructorAttempted'] or not row['cleanupDabCheckpointRecorded'],
                context + 'an unconstructed stroke supplied cleanup dab progress')
        if row['afterFinishDabCheckpointRecorded']:
            require(row['cleanupDabCheckpointRecorded'] and row['dabsAfterFinish'] == row['dabsBeforeCleanup'],
                    context + 'post-finish progress differs from the retained stroke before cleanup')
            require(row['dabsAfterFinish'] >= row['dabsBeforeFinish'],
                    context + 'observed dab count regressed across finish attempt')
        pixel_bytes = row.get('enginePixelBytes')
        require(type(pixel_bytes) is int and 0 < pixel_bytes <= 128 and
                row['backingRasterBytes'] == side * side * pixel_bytes and
                row['peakTrackedStrokeBytesIsSampled'] is True and
                row.get('trackedAllocationSampling') == 'constructor, successful adds, finish, cleanup; transient peaks between checkpoints not observed' and
                row['peakTrackedStrokeBytes'] <= 256 * 1024 * 1024, context + 'incorrect allocation accounting')
        for flag, key in (('constructorAllocationCheckpointRecorded', 'trackedBytesAfterConstructor'),
                          ('finishAllocationCheckpointRecorded', 'trackedBytesAfterFinish')):
            require((type(row.get(key)) is int and 0 <= row[key] <= row['peakTrackedStrokeBytes'])
                    if row[flag] else row.get(key) is None, context + 'unobserved allocation checkpoint was invented')
        require(all(finite(row.get(key)) for key in ('sizeValue', 'spacing', 'smudgeRadius')),
                context + 'missing actual production settings')
        require(type(row.get('resolvedSeed')) is str and row['resolvedSeed'] == '1', context + 'resolved benchmark seed changed')
        if name in ORIGINAL:
            # These values come from the unchanged original C++ workloads.
            # Keep their full-length traces and existing timing gates as well.
            expected_size = 256 if heavy else 64
            expected_radius = 0 if name in ORIGINAL[6:] else 1
            expected_objects = 1 if heavy else 0
            expected_bytes = 4 * pixel_bytes if heavy else 0
            expected_knots = 2 if name in ORIGINAL[:4] else 0
            require(row['sizeValue'] == expected_size and row['spacing'] == .1 and
                    row['smudgeRadius'] == expected_radius,
                    context + 'original size/spacing/radius workload changed')
            require(row['inputResourceObjects'] == expected_objects and
                    row['inputResourcePixelBytes'] == expected_bytes and
                    row['presetCurveKnots'] == expected_knots,
                    context + 'original texture resource or pressure-curve inventory changed')
        if name.startswith('source-generated-soft65-'):
            require(row['presetCurveKnots'] == 65 and row['sizeValue'] == 64 and row['inputResourceObjects'] == 0,
                    context + 'nonlinear generated constructor workload changed')
        if name.startswith('source-random-gih-'):
            require(row['inputResourceObjects'] == 3 and row['inputResourcePixelBytes'] == 3 * 64 * pixel_bytes and row['sizeValue'] == 8,
                    context + 'random GIH resource workload missing')
        if name.startswith('source-scatter-'):
            require(row['sizeValue'] == 8 and row['spacing'] == 4, context + 'bounded scatter workload changed')
        if name.startswith('source-new-dulling-radius-half-'):
            require(row['smudgeRadius'] == .5 and row['sizeValue'] == 64,
                    context + 'adaptive/nonzero-radius Dulling workload missing')
        limit95, limit99 = (33, 66) if heavy else (16, 33)
        require(row.get('cpuP95LimitMilliseconds') == limit95 and row.get('cpuP99LimitMilliseconds') == limit99 and
                row.get('traceThroughputLimitMilliseconds') == seconds * 1000,
                context + 'original CPU thresholds were weakened')
        require(type(row.get('error')) is str and type(row.get('failurePhase')) is str and
                row['sourceReferenceVerified'] is False and row['displayLatencyMeasured'] is False,
                context + 'missing failure/scope information')
        if row.get('status') == 'complete':
            require(row['presetValidated'] and row['constructorAttempted'] and row['constructorSucceeded'] and
                    row['finishAttempted'] and row['finishSucceeded'] and row['resourceReleaseChecked'] and
                    row['constructorAllocationCheckpointRecorded'] and row['finishAllocationCheckpointRecorded'] and
                    row['beforeFinishDabCheckpointRecorded'] and row['afterFinishDabCheckpointRecorded'] and
                    row['cleanupDabCheckpointRecorded'] and
                    type(row.get('releasedStrokeBytes')) is int and row['releasedStrokeBytes'] == 0 and
                    row['events'] == row['attemptedEvents'] == row['plannedEvents'] and
                    row['dabsAfterFinish'] >= row['dabsBeforeFinish'] and
                    all(finite(row.get(key)) for key in TIMINGS), context + 'invalid/incomplete stroke was timed as success')
            require(row['eventP95Milliseconds'] <= row['eventP99Milliseconds'] <= row['eventMaxMilliseconds'] and
                    row['firstAddMilliseconds'] <= row['eventMaxMilliseconds'] and
                    row['strokeLifecycleMilliseconds'] + 1e-6 >= sum(row[k] for k in
                        ('strokeConstructorMilliseconds', 'renderMilliseconds', 'finishMilliseconds', 'cleanupMilliseconds')),
                    context + 'phase durations do not belong to the complete lifecycle')
            checks = (row['eventP95Milliseconds'] <= limit95 and row['eventP99Milliseconds'] <= limit99,
                      row['renderMilliseconds'] <= seconds * 1000,
                      row['strokeLifecycleMilliseconds'] <= seconds * 1000)
            require((row['eventBudgetPassed'], row['addThroughputPassed'], row['lifecycleThroughputPassed']) == checks and
                    row['cpuBudgetPassed'] is all(checks), context + 'CPU verdict differs from retained measurements')
            require((not row['error'] and not row['failurePhase']) if row['cpuBudgetPassed'] else
                    (bool(row['error']) and row['failurePhase'] == 'cpu-budget'), context + 'CPU failure was hidden')
        else:
            require(row.get('status') == 'failed' and row['error'] and row['failurePhase'] and
                    all(row.get(key) is None for key in TIMINGS) and
                    not any(row[key] for key in ('eventBudgetPassed', 'addThroughputPassed', 'lifecycleThroughputPassed', 'cpuBudgetPassed')),
                    context + 'failed/invalid stroke supplied successful timing metrics')
            require(not row['constructorSucceeded'] or row['constructorAttempted'], context + 'constructor success was inferred')
            require(not row['finishSucceeded'] or row['finishAttempted'], context + 'finish success was inferred')
            phase = row['failurePhase']
            require(phase in ('preset-validation', 'latency-storage-setup', 'backing-raster-setup',
                              'buffered-surface-constructor', 'stroke-constructor', 'stroke-add',
                              'stroke-finish', 'allocation-release'), context + 'unknown failed lifecycle phase')
            if phase == 'preset-validation':
                require(not row['presetValidated'] and not row['constructorAttempted'] and not row['finishAttempted'] and
                        row['events'] == row['attemptedEvents'] == 0, context + 'invalid preset was rendered')
            if phase in ('latency-storage-setup', 'backing-raster-setup', 'buffered-surface-constructor'):
                require(row['presetValidated'] and not row['constructorAttempted'] and not row['finishAttempted'] and
                        row['events'] == row['attemptedEvents'] == 0, context + 'setup failure supplied stroke results')
            if phase == 'stroke-constructor':
                require(row['presetValidated'] and row['constructorAttempted'] and not row['constructorSucceeded'] and
                        not row['finishAttempted'] and row['events'] == row['attemptedEvents'] == 0,
                        context + 'failed constructor supplied rendered results')
            if phase == 'stroke-add':
                require(row['constructorSucceeded'] and not row['finishAttempted'] and
                        row['attemptedEvents'] == row['events'] + 1, context + 'failed add was counted as successful')
            if phase == 'stroke-finish':
                require(row['constructorSucceeded'] and row['finishAttempted'] and not row['finishSucceeded'] and
                        row['events'] == row['plannedEvents'], context + 'failed finish was counted as successful')
            if phase == 'allocation-release':
                require(row['resourceReleaseChecked'] and type(row.get('releasedStrokeBytes')) is int and
                        row['releasedStrokeBytes'] > 0, context + 'resource release failure lacks retained allocation evidence')
            require((type(row.get('releasedStrokeBytes')) is int and row['releasedStrokeBytes'] >= 0)
                    if row['resourceReleaseChecked'] else row.get('releasedStrokeBytes') is None,
                    context + 'unobserved resource release was invented')
        failed += not row['cpuBudgetPassed']
    require(type(report.get('workloads')) is int and report['workloads'] == len(NAMES) and
            type(report.get('failedWorkloads')) is int and report['failedWorkloads'] == failed and
            report.get('allWorkloadsPassed') is (failed == 0), 'CPU report hid a failed workload')
    return failed == 0


def main():
    report = json.loads(Path(sys.argv[1]).read_text(), parse_constant=lambda text: (_ for _ in ()).throw(ValueError('Nonfinite CPU metric')))
    passed = validate(report)
    for row in report['records']:
        print(('::notice title=Engine CPU lifecycle::' if row['cpuBudgetPassed'] else '::error title=Engine CPU lifecycle::') +
              json.dumps(row, separators=(',', ':'), allow_nan=False).replace('%', '%25').replace('\n', '%0A').replace('\r', '%0D'))
    raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()
