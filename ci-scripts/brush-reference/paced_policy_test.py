"""CI-only deterministic timing-policy bounds; no source pixel retries."""
import importlib.util
from pathlib import Path
import unittest
import json
import tempfile
import timed_fixtures

spec = importlib.util.spec_from_file_location('paced_policy', Path(__file__).parent / 'gzbrush_reference' / 'paced_policy.py')
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


class TimingPolicy(unittest.TestCase):
    def test_exact_bounds(self):
        for elapsed in (98000, 100000, 120000):
            policy.check('contact', 1, 100000, elapsed, 20000)
        for elapsed in (97999, 120001):
            with self.assertRaises(policy.TimingFailure) as failure:
                policy.check('contact', 1, 100000, elapsed, 20000)
            self.assertEqual(failure.exception.detail['latenessUs'], elapsed - 100000)
            self.assertEqual(failure.exception.detail['index'], 1)

    def test_discard_must_be_timing_failure(self):
        case = {'events': [{'timeUs': 0}, {'timeUs': 100000}],
                'timedProbe': {'durationUs': 100000, 'maxLatenessUs': 20000}}
        entry = {'attempt': 1, 'reason': 'timing-only', 'cleanup': 'release-and-leave',
                 'detail': {'phase': 'contact', 'index': 1, 'authoredUs': 100000,
                            'elapsedUs': 120001, 'latenessUs': 20001, 'boundUs': 20000}}
        policy.validate_discarded([entry], case)
        with self.assertRaises(RuntimeError): policy.validate_discarded([entry, entry], case)
        for reason in ('pixel mismatch', 'empty stroke'):
            with self.assertRaises(RuntimeError): policy.validate_discarded([{**entry, 'reason': reason}], case)
        detail = {**entry['detail'], 'elapsedUs': 110000, 'latenessUs': 10000}
        with self.assertRaises(RuntimeError): policy.validate_discarded([{**entry, 'detail': detail}], case)
        with self.assertRaises(RuntimeError): policy.validate_discarded([{**entry, 'attempt': 2}], case)

    def test_causal_clock_controls_preserve_real_pacing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'output').mkdir()
            resources = root / 'resources'
            (resources / 'paintoppresets').mkdir(parents=True)
            timed_fixtures.prepare(root, resources, lambda *args: b'original', 'timedsensors')
            cases = json.loads((root / 'output' / 'fixtures.json').read_text())['cases']
        self.assertEqual(len(cases), 40)
        self.assertEqual(cases[27]['timedProbe']['target'], 'speed-cadence-65')
        for start in (28, 31, 34, 37):
            controls = cases[start:start + 3]
            self.assertEqual([(c['timedProbe']['deviceTimestampScale'],
                               c['timedProbe']['deviceTimestampOffsetMs']) for c in controls],
                             [(1, 0), (2, 0), (2, 1000000)])
            self.assertEqual(controls[0]['events'], controls[1]['events'])
            self.assertEqual(controls[0]['events'], controls[2]['events'])
            self.assertTrue(all(c['timedProbe']['maxLatenessUs'] == 20000 for c in controls))
            self.assertTrue(all(c['overrides'] == controls[0]['overrides'] for c in controls))


if __name__ == '__main__':
    unittest.main()
