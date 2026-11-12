"""CI-only bounds/completeness regressions for source frame-advancement probes."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from gih_cadence_fixtures import plans
from resource_fixtures import prepare

ROOT = Path(__file__).parent


def helper(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'gzbrush_reference' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Cadence(unittest.TestCase):
    def test_complete_and_exact_delivery(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'output').mkdir()
            (root / 'paintoppresets').mkdir()
            prepare(root, root, lambda *args: b'original synthetic fixture', 'gihcadence')
            manifest = json.loads((root / 'output' / 'fixtures.json').read_text())
            self.assertEqual(len(manifest['cases']), 168)
            self.assertEqual(len(set(plans())), 168)
            validate = helper('cadence_probe').validate
            for case in manifest['cases']:
                paths = validate(case)
                self.assertEqual(len(case['cadenceCenters']), 32)
                self.assertLessEqual(sum(len(p) for p in paths), 120)
                self.assertTrue(all(0 <= v < 2048 for p in paths for point in p for v in point))
                bad = copy.deepcopy(case)
                bad['cadencePaths'][0][0][0] += 1
                with self.assertRaises(RuntimeError): validate(bad)
            bad = copy.deepcopy(manifest['cases'][0])
            bad['cadenceProbe']['slots'] = 10**12
            with self.assertRaises(RuntimeError): validate(bad)

    def test_sequence_diagnostics(self):
        summarize = helper('sequence_evidence').summarize
        sequence = [i % 3 for i in range(3072)]
        report = summarize(sequence, 64)
        self.assertEqual(report['lags']['1'], {'equal': 0, 'pairs': 3071})
        self.assertEqual(report['lags']['3'], {'equal': 3069, 'pairs': 3069})
        self.assertEqual(report['rowBoundaryEqualPairs'], [0, 47])
        self.assertEqual(report['interiorEqualPairs'], [0, 3024])
        for invalid in ([0] * 3073, [False] * 32, [4] * 32, [-1] * 32):
            with self.assertRaises(ValueError): summarize(invalid)


if __name__ == '__main__':
    unittest.main()
