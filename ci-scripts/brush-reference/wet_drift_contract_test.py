"""Pure bounded-prefix contract checks, executed only in reference CI."""
import unittest
from wet_drift_contract import CHECKPOINTS, WIDTH, HEIGHT, center, path, validate, capture


class DriftPathContract(unittest.TestCase):
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
                "sourcePath": path(1000), "diameter": 8, "spacing": 2}
        validate(case)
        case["sourcePath"][-1][0] += 1
        with self.assertRaises(ValueError):
            validate(case)

    def test_observable_count_rejects_duplicate_and_extra_deposits(self):
        case = {"driftProbe": {"nominalDabs": 1, "calibration": True},
                "sourcePath": path(1), "diameter": 8, "spacing": 2}
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

    def test_unknown_count_rejected(self):
        with self.assertRaises(ValueError):
            path(10001)


if __name__ == "__main__":
    unittest.main()
