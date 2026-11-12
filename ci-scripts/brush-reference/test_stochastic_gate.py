"""CI-only behavioral checks for statistics rejection and metric geometry."""
import base64
import unittest
import random
from unittest.mock import Mock, MagicMock
import zlib
from stochastic_gate import compare, decode, measure, frame_measure, read_report, REPORT_LIMIT, long_frame_measure


class StatisticsGate(unittest.TestCase):
    def test_mean_shift_rejected(self):
        self.assertFalse(compare([.2] * 100, [.24] * 100, 200)['passed'])
        self.assertTrue(compare([.2] * 100, [.2] * 100, 200)['passed'])

    def test_variance_mismatch_rejected(self):
        source = [0, 1] * 50
        candidate = [.4, .6] * 50
        self.assertFalse(compare(source, candidate, 200)['passed'])

    def test_empty_and_malformed_samples_rejected(self):
        with self.assertRaises(ValueError):
            compare([0] * 99, [0] * 100)
        with self.assertRaises(ValueError):
            compare([float('nan')] * 100, [0] * 100)
        raw = base64.b64encode(zlib.compress(b'\0' * (1041 * 48 + 1))).decode()
        with self.assertRaises(ValueError):
            decode({'rect': [232, 1000, 1041, 48], 'data': raw, 'encoding': 'zlib-base64-alpha8-row-major'}, 1)

    def test_independent_correct_rng_positive_and_broken_selector_negative(self):
        # Prospective fixed seeds, independent applications and strokes. Never
        # rebalance draws or retry/select seeds until a test passes.
        source_rng, candidate_rng = random.Random(421337), random.Random(719983)
        source = [long_frame_measure([source_rng.randrange(3) for _ in range(3072)]) for _ in range(100)]
        candidate = [long_frame_measure([candidate_rng.randrange(3) for _ in range(3072)]) for _ in range(100)]
        for key in source[0]:
            with self.subTest(statistic=key):
                self.assertTrue(compare([row[key] for row in source],
                                        [row[key] for row in candidate])['passed'])
        # Identical alpha but always-first color selection must be rejected.
        broken = long_frame_measure([0] * 3072)
        self.assertFalse(compare([row['0/frameFraction0'] for row in source],
                                 [broken['0/frameFraction0']] * 100)['passed'])
        self.assertFalse(compare([row['0/transitionJoint00'] for row in source],
                                 [broken['0/transitionJoint00']] * 100)['passed'])

    def test_invalid_frame_labels_rejected(self):
        for labels in ([0] * 31, [True] * 32, [3] * 32):
            with self.assertRaises(ValueError):
                frame_measure(labels, 1041, 8, 32, 232)

    def test_hostile_geometry_rejected_before_iteration(self):
        for width, height, diameter, slots, origin in (
                (1041, 48, 8, 10**12, 232), (3, 1, 8, 32, 232),
                (1041, 48, 0, 32, 232), (1041, 48, float('nan'), 32, 232),
                (1041, 48, 8, True, 232), (1041, 48, 8, 32, -10**12)):
            with self.assertRaises(ValueError):
                measure(b'', width, height, diameter, slots, origin)
            with self.assertRaises(ValueError):
                frame_measure([], width, diameter, slots, origin)

    def test_report_read_is_bounded(self):
        stream = Mock()
        stream.read.return_value = b' ' * (REPORT_LIMIT + 1)
        manager = MagicMock()
        manager.__enter__ = Mock(return_value=stream)
        manager.__exit__ = Mock(return_value=False)
        path = Mock()
        path.open.return_value = manager
        with self.assertRaises(ValueError):
            read_report(path)
        stream.read.assert_called_once_with(REPORT_LIMIT + 1)

    def test_known_occupied_cells_and_width(self):
        width, height = 1041, 48
        alpha = bytearray(width * height)
        for slot in range(32):
            cx = 24 + slot * 32
            for y in range(22, 26):
                for x in range(cx - 2, cx + 2):
                    alpha[y * width + x] = 255
        metrics = measure(alpha, width, height, 8, 32, 232)
        for band, count in enumerate((11, 10, 11)):
            self.assertEqual(metrics[str(band) + '/occupiedTips'], 1)
            self.assertEqual(metrics[str(band) + '/meanWidth'], .5)
            self.assertEqual(metrics[str(band) + '/coverage'], count * 16 / (347 * 48))
            self.assertGreater(metrics[str(band) + '/energy1'], 0)


if __name__ == '__main__':
    unittest.main()
