"""Pure bounded-prefix contract checks, executed only in reference CI."""
import unittest
from wet_drift_contract import CHECKPOINTS, WIDTH, HEIGHT, center, path, validate, capture, geometry_windows


class DriftPathContract(unittest.TestCase):
    def test_geometry_windows_preserve_raw_channels_without_row_wrap(self):
        import base64
        import zlib
        raw = bytearray(WIDTH * HEIGHT * 4)
        raw[:4] = bytes((1, 2, 3, 4))
        raw[(WIDTH - 64) * 4:(WIDTH - 64) * 4 + 4] = bytes((5, 6, 7, 8))
        raw[(WIDTH - 1) * 4:WIDTH * 4] = bytes((9, 10, 11, 12))
        windows = geometry_windows(raw)
        self.assertEqual([w['rect'] for w in windows], [[0, 0, 64, 48], [1984, 0, 64, 48]])
        left, right = [zlib.decompress(base64.b64decode(w['data'])) for w in windows]
        self.assertEqual(len(left), 64 * 48 * 4)
        self.assertEqual(left[:4], bytes((1, 2, 3, 4)))
        self.assertEqual(left[64 * 4:64 * 4 + 4], bytes(4))
        self.assertEqual(right[:4], bytes((5, 6, 7, 8)))
        self.assertEqual(right[63 * 4:64 * 4], bytes((9, 10, 11, 12)))
        with self.assertRaises(ValueError):
            geometry_windows(raw[:-1])

    def test_exact_arc_length_and_bounds(self):
        for count in CHECKPOINTS:
            points = path(count)
            length = sum(abs(b[0] - a[0]) + abs(b[1] - a[1]) for a, b in zip(points, points[1:]))
            self.assertEqual(length, (count - 1) * 16 + 1)
            self.assertTrue(all(4 <= x < 2044 and 4 <= y < 2044 for x, y in points))
            self.assertLessEqual(len(points), 160)
            self.assertEqual(points[0], center(0))

    def test_unique_nominal_cells(self):
        self.assertEqual(len({tuple(center(i)) for i in range(10000)}), 10000)
        self.assertTrue(all(abs(center(i)[0] - center(i - 1)[0]) +
                            abs(center(i)[1] - center(i - 1)[1]) == 16
                            for i in range(1, 10000)))

    def test_changed_endpoint_rejected(self):
        case = {"driftProbe": {"nominalDabs": 1000, "calibration": False},
                "sourcePath": path(1000), "diameter": 8, "spacing": 2, "sourceEngine": "colorsmudge"}
        validate(case)
        case["sourcePath"][-1][0] += 1
        with self.assertRaises(ValueError):
            validate(case)

    def test_observable_count_rejects_duplicate_and_extra_deposits(self):
        case = {"driftProbe": {"nominalDabs": 1, "calibration": True},
                "sourcePath": path(1), "diameter": 8, "spacing": 2, "sourceEngine": "paintbrush"}
        before = bytes(WIDTH * HEIGHT * 4)
        after = bytearray(before)
        pixel = (8 * WIDTH + 8) * 4
        after[pixel:pixel + 4] = bytes((0, 255, 0, 128))
        self.assertEqual(capture(before, after, case)["observedDepositCells"], 1)
        after[pixel + 3] = 192
        with self.assertRaises(ValueError):
            capture(before, after, case)
        after[pixel + 3] = 128
        other = (8 * WIDTH + 40) * 4
        after[other:other + 4] = bytes((0, 255, 0, 128))
        with self.assertRaises(ValueError):
            capture(before, after, case)

    def test_calibration_engine_is_explicit(self):
        case = {"driftProbe": {"nominalDabs": 1, "calibration": True},
                "sourcePath": path(1), "diameter": 8, "spacing": 2, "sourceEngine": "paintbrush"}
        validate(case)
        case["sourceEngine"] = "colorsmudge"
        with self.assertRaises(ValueError):
            validate(case)

    def test_unknown_count_rejected(self):
        with self.assertRaises(ValueError):
            path(10001)


if __name__ == "__main__":
    unittest.main()
