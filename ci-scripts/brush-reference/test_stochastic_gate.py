"""CI-only behavioral checks for statistics rejection and metric geometry."""
import base64
import unittest
import zlib
from stochastic_gate import compare, decode, measure


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
        raw = base64.b64encode(zlib.compress(b'\0' * 100)).decode()
        with self.assertRaises(ValueError):
            decode({'rect': [0, 0, 1, 1], 'data': raw, 'encoding': 'zlib-base64-alpha8-row-major'}, 1)

    def test_known_occupied_cells_and_width(self):
        # Three original4x4 tips centered in three separate bands.
        width, height = 384, 16
        alpha = bytearray(width * height)
        for cx in (24, 152, 280):
            for y in range(6, 10):
                for x in range(cx - 2, cx + 2):
                    alpha[y * width + x] = 255
        metrics = measure(alpha, width, height, 32, 3, 232)
        for band in range(3):
            self.assertEqual(metrics[str(band) + '/occupiedTips'], 1)
            self.assertEqual(metrics[str(band) + '/meanWidth'], 4 / 32)
            self.assertEqual(metrics[str(band) + '/coverage'], 16 / (128 * 16))
            self.assertGreater(metrics[str(band) + '/energy1'], 0)


if __name__ == '__main__':
    unittest.main()
