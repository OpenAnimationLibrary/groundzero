"""Authored CPU report truth regressions; synthetic timings are not measurements."""
import copy
import unittest
import benchmark_report as contract


def report():
    records = []
    seen = set()
    for ordinal, name in enumerate(contract.NAMES, 1):
        profile = contract.expected_profile(name)
        expanded = name not in contract.ORIGINAL
        repeated = expanded and name.endswith('-repeat')
        heavy = name in ('heavy-texture-dual', 'heavy-smear')
        side, hz, seconds = (4096, 120, 60) if heavy else (2048, 240, 12 if expanded else 60)
        row = dict.fromkeys(contract.FLAGS, False)
        row.update(name=name, profile=profile, status='complete', error='', failurePhase='',
                   processStrokeOrdinal=ordinal, firstProfileUseInProcess=profile not in seen,
                   setupMode='repeat-workload-same-preset-and-resource-objects' if repeated else 'first-use-of-workload-in-process',
                   presetDefinitionReused=repeated, expandedLifecycleWorkload=expanded,
                   unmeasuredWarmupStrokes=0, rasterState='fresh backing and BufferedSurface per measured stroke',
                   resourceOrigin='original in-memory benchmark definitions', resolvedSeed='1',
                   canvasSide=side, inputHz=hz, traceSeconds=seconds, plannedEvents=seconds * hz + 1,
                   attemptedEvents=seconds * hz + 1, events=seconds * hz + 1,
                   dabs=100, dabsBeforeFinish=100, dabsAfterFinish=101, dabsBeforeCleanup=101,
                   beforeFinishDabCheckpointRecorded=True, afterFinishDabCheckpointRecorded=True,
                   cleanupDabCheckpointRecorded=True,
                   presetValidated=True, constructorAttempted=True, constructorSucceeded=True,
                   finishAttempted=True, finishSucceeded=True,
                   peakTrackedStrokeBytes=4096, peakTrackedStrokeBytesIsSampled=True,
                   trackedAllocationSampling='constructor, successful adds, finish, cleanup; transient peaks between checkpoints not observed',
                   constructorAllocationCheckpointRecorded=True, trackedBytesAfterConstructor=1024,
                   finishAllocationCheckpointRecorded=True, trackedBytesAfterFinish=2048,
                   enginePixelBytes=32, backingRasterBytes=side * side * 32,
                   inputResourceObjects=1 if heavy else 3 if name.startswith('source-random-gih-') else 0,
                   inputResourcePixelBytes=4 * 32 if heavy else 3 * 64 * 32 if name.startswith('source-random-gih-') else 0,
                   presetCurveKnots=65 if name.startswith('source-generated-soft65-') else 2 if name in contract.ORIGINAL[:4] else 0,
                   sizeValue=256 if heavy else 8 if name.startswith(('source-scatter-', 'source-random-gih-')) else 64,
                   spacing=4 if name.startswith(('source-scatter-', 'source-random-gih-')) else 1 if name.startswith('source-generated-') else .1,
                   smudgeRadius=0 if name in contract.ORIGINAL[6:] else .5 if name.startswith('source-new-dulling-radius-half-') else 1,
                   resourceReleaseChecked=True, releasedStrokeBytes=0,
                   cpuP95LimitMilliseconds=33 if heavy else 16,
                   cpuP99LimitMilliseconds=66 if heavy else 33,
                   traceThroughputLimitMilliseconds=seconds * 1000,
                   eventBudgetPassed=True, addThroughputPassed=True,
                   lifecycleThroughputPassed=True, cpuBudgetPassed=True)
        row.update(dict.fromkeys(contract.TIMINGS, 1.0))
        row.update(firstAddMilliseconds=.5, eventP95Milliseconds=1., eventP99Milliseconds=2.,
                   eventMaxMilliseconds=3., renderMilliseconds=200., finishMilliseconds=4.,
                   cleanupMilliseconds=2., strokeLifecycleMilliseconds=207.1)
        records.append(row)
        seen.add(profile)
    return {'schema': 2, 'scope': 'engine CPU only; no GUI/display/tablet latency claim',
            'displayLatencyMeasured': False, 'sourceReferenceVerified': False,
            'records': records, 'workloads': len(records), 'failedWorkloads': 0, 'allWorkloadsPassed': True}


def fail(row, phase='stroke-finish'):
    row.update(status='failed', error='Actual benchmark operation failed', failurePhase=phase,
               finishSucceeded=False, dabsAfterFinish=103, dabsBeforeCleanup=103,
               beforeFinishDabCheckpointRecorded=True, afterFinishDabCheckpointRecorded=True,
               cleanupDabCheckpointRecorded=True,
               finishAllocationCheckpointRecorded=False, trackedBytesAfterFinish=None,
               eventBudgetPassed=False, addThroughputPassed=False,
               lifecycleThroughputPassed=False, cpuBudgetPassed=False)
    row.update(dict.fromkeys(contract.TIMINGS, None))


class BenchmarkReportContract(unittest.TestCase):
    def test_complete_report_retains_originals_and_expanded_first_repeat_contexts(self):
        data = report()
        self.assertTrue(contract.validate(data))
        self.assertEqual(len(data['records']), 20)
        self.assertEqual([r['name'] for r in data['records'][:10]], list(contract.ORIGINAL))
        self.assertEqual(sum(r['presetDefinitionReused'] for r in data['records']), 5)
        self.assertFalse(next(r for r in data['records'] if r['name'] == 'source-new-dulling-radius-half-first')['firstProfileUseInProcess'])

    def test_failed_finish_keeps_failure_and_null_timings(self):
        data = report(); row = data['records'][-1]
        fail(row)
        data.update(failedWorkloads=1, allWorkloadsPassed=False)
        self.assertFalse(contract.validate(data))
        self.assertEqual(row['dabsBeforeFinish'], 100)
        self.assertEqual(row['dabsAfterFinish'], 103)
        self.assertEqual(row['dabsBeforeCleanup'], 103)
        for key, value in (('status', 'complete'), ('finishSucceeded', True),
                           ('renderMilliseconds', 0), ('cpuBudgetPassed', True),
                           ('trackedBytesAfterFinish', 0), ('dabsAfterFinish', 0),
                           ('dabsBeforeCleanup', None), ('afterFinishDabCheckpointRecorded', False)):
            altered = copy.deepcopy(data); altered['records'][-1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): contract.validate(altered)
        altered = copy.deepcopy(data)
        altered['records'][-1].update(dabsAfterFinish=0, dabsBeforeCleanup=0)
        with self.assertRaisesRegex(ValueError, 'observed dab count regressed'):
            contract.validate(altered)

    def test_failed_add_after_dabs_keeps_observed_progress_without_finish_checkpoints(self):
        data = report(); row = data['records'][-1]
        fail(row, 'stroke-add')
        row.update(finishAttempted=False, events=17, attemptedEvents=18,
                   beforeFinishDabCheckpointRecorded=False, dabsBeforeFinish=None, dabs=None,
                   afterFinishDabCheckpointRecorded=False, dabsAfterFinish=None,
                   cleanupDabCheckpointRecorded=True, dabsBeforeCleanup=9)
        data.update(failedWorkloads=1, allWorkloadsPassed=False)
        self.assertFalse(contract.validate(data))
        self.assertEqual(row['dabsBeforeCleanup'], 9)
        for key, value in (('dabs', 0), ('dabsBeforeFinish', 0), ('dabsAfterFinish', 0),
                           ('dabsBeforeCleanup', None), ('cleanupDabCheckpointRecorded', False)):
            altered = copy.deepcopy(data); altered['records'][-1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): contract.validate(altered)

    def test_invalid_preset_never_claims_constructed_finished_or_released_stroke(self):
        data = report(); row = data['records'][0]
        fail(row, 'preset-validation')
        row.update(presetValidated=False, constructorAttempted=False, constructorSucceeded=False,
                   finishAttempted=False, events=0, attemptedEvents=0, dabs=None, dabsBeforeFinish=None,
                   beforeFinishDabCheckpointRecorded=False, afterFinishDabCheckpointRecorded=False,
                   cleanupDabCheckpointRecorded=False, dabsAfterFinish=None, dabsBeforeCleanup=None,
                   constructorAllocationCheckpointRecorded=False, trackedBytesAfterConstructor=None,
                   resourceReleaseChecked=False, releasedStrokeBytes=None, peakTrackedStrokeBytes=0)
        data.update(failedWorkloads=1, allWorkloadsPassed=False)
        self.assertFalse(contract.validate(data))
        row['releasedStrokeBytes'] = 0
        with self.assertRaises(ValueError): contract.validate(data)

    def test_original_workload_settings_resources_and_pressure_knots_are_pinned(self):
        data = report()
        self.assertTrue(contract.validate(data))
        original = {row['name']: row for row in data['records'][:10]}
        for name in ('heavy-texture-dual', 'heavy-smear'):
            self.assertEqual(original[name]['sizeValue'], 256)
            self.assertEqual(original[name]['inputResourceObjects'], 1)
            self.assertEqual(original[name]['inputResourcePixelBytes'], 4 * original[name]['enginePixelBytes'])
        self.assertTrue(all(row['spacing'] == .1 for row in original.values()))
        self.assertTrue(all(original[name]['presetCurveKnots'] == 2 for name in contract.ORIGINAL[:4]))
        for name, key, changed in (
                ('heavy-texture-dual', 'sizeValue', 64), ('heavy-smear', 'sizeValue', 64),
                ('standard-dry', 'sizeValue', 256), ('standard-dry', 'spacing', 1),
                ('source-candidate-default-circle-aa', 'spacing', 1),
                ('heavy-texture-dual', 'inputResourceObjects', 0),
                ('heavy-smear', 'inputResourcePixelBytes', 0),
                ('heavy-texture-dual', 'inputResourcePixelBytes', 3 * 32),
                ('standard-dry', 'presetCurveKnots', 0), ('heavy-smear', 'presetCurveKnots', 3),
                ('new-dulling', 'presetCurveKnots', 2), ('legacy-smear', 'smudgeRadius', 1),
                ('source-candidate-hard-wash', 'smudgeRadius', 0)):
            altered = copy.deepcopy(data)
            next(row for row in altered['records'] if row['name'] == name)[key] = changed
            with self.subTest(name=name, key=key), self.assertRaises(ValueError): contract.validate(altered)

    def test_constructor_and_finish_cost_cannot_disappear_from_lifecycle_throughput(self):
        data = report(); row = data['records'][-1]
        row.update(strokeConstructorMilliseconds=11000., finishMilliseconds=900., strokeLifecycleMilliseconds=12102.,
                   lifecycleThroughputPassed=False, cpuBudgetPassed=False,
                   failurePhase='cpu-budget', error='Actual complete lifecycle exceeds its real-time trace')
        data.update(failedWorkloads=1, allWorkloadsPassed=False)
        self.assertFalse(contract.validate(data))
        row['cpuBudgetPassed'] = True
        with self.assertRaises(ValueError): contract.validate(data)

    def test_schema_rejects_removed_workloads_changed_limits_and_false_measurement_claims(self):
        for defect in ('omit-original', 'omit-expanded', 'reorder', 'threshold', 'duration', 'profile',
                       'first-profile', 'reuse', 'machine-cold', 'source-parity', 'display', 'resource',
                       'radius', 'finish', 'release', 'checkpoint', 'nonfinite', 'phase-sum', 'count-type'):
            data = report(); row = data['records'][-1]
            if defect == 'omit-original': data['records'].pop(0)
            elif defect == 'omit-expanded': data['records'].pop()
            elif defect == 'reorder': data['records'].reverse()
            elif defect == 'threshold': row['cpuP95LimitMilliseconds'] = 17
            elif defect == 'duration': data['records'][0]['traceSeconds'] = 12
            elif defect == 'profile': row['profile'] = 'groundzero-native-1'
            elif defect == 'first-profile': row['firstProfileUseInProcess'] = True
            elif defect == 'reuse': row['presetDefinitionReused'] = False
            elif defect == 'machine-cold': row['machineColdCacheClaimed'] = True
            elif defect == 'source-parity': row['sourceReferenceVerified'] = True
            elif defect == 'display': data['displayLatencyMeasured'] = True
            elif defect == 'resource': data['records'][-3]['inputResourceObjects'] = 0
            elif defect == 'radius': row['smudgeRadius'] = 0
            elif defect == 'finish': row['finishSucceeded'] = False
            elif defect == 'release': row['releasedStrokeBytes'] = 1
            elif defect == 'checkpoint': row['trackedBytesAfterConstructor'] = None
            elif defect == 'nonfinite': row['finishMilliseconds'] = float('nan')
            elif defect == 'phase-sum': row['strokeLifecycleMilliseconds'] = 1
            else: row['events'] = True
            with self.subTest(defect=defect), self.assertRaises(ValueError): contract.validate(data)


if __name__ == '__main__':
    unittest.main()
