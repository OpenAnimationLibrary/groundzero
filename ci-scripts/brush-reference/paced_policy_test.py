"""CI-only deterministic timing-policy bounds; no source pixel retries."""
import importlib.util
from pathlib import Path
import unittest

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


if __name__ == '__main__':
    unittest.main()
