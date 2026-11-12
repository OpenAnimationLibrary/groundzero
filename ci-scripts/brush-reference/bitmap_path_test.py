"""Public-path geometry contract tests; no reference application required."""
import copy
from pathlib import Path
import runpy
import unittest

validate = runpy.run_path(str(Path(__file__).parent / "gzbrush_reference" / "bitmap_path.py"))["validate"]


class BitmapPathContract(unittest.TestCase):
    def setUp(self):
        self.case = {"bitmapPathProbe": True, "resourceProbe": {"kind": "filter-impulse"},
                     "pressure": [1, 1], "start": [256.25, 1024.25], "end": [257.25, 1024.25]}

    def test_fractional_vertices_are_preserved(self):
        self.assertEqual(validate(self.case), [[256.25, 1024.25], [257.25, 1024.25]])

    def test_wrong_or_unbounded_geometry_is_rejected(self):
        for key, value in (("bitmapPathProbe", "true"), ("pressure", [.5, 1]),
                           ("resourceProbe", {"kind": "random"}),
                           ("start", [float("nan"), 1024]), ("start", [False, 1024]),
                           ("end", [257.25, 1024]), ("end", [float("inf"), 1024]),
                           ("end", [257.25]), ("start", [-1, 1024])):
            with self.subTest(key=key, value=value):
                changed = copy.deepcopy(self.case)
                changed[key] = value
                with self.assertRaises(RuntimeError):
                    validate(changed)


if __name__ == "__main__":
    unittest.main()
