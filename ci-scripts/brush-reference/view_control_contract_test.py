"""CI-only source orientation guards, imported without the Krita package."""
from pathlib import Path
from runpy import run_path
import unittest

rotation_matches = run_path(str(Path(__file__).parent /
    "gzbrush_reference/view_control_contract.py"))["rotation_matches"]


class SourceViewRotation(unittest.TestCase):
    def test_wrapped_orientations_are_equivalent(self):
        for observed, requested in ((337, -23), (-23, 337), (0, 360),
                                    (360, 0), (-720, 0), (450, 90),
                                    (1097, 17), (-180, 180)):
            self.assertTrue(rotation_matches(observed, requested))
            self.assertTrue(rotation_matches(observed, requested, resource=True))

    def test_observed_bitmap_wrap_keeps_its_original_guard(self):
        # Completed source case067: requested -23, read back 337.0000241088671.
        observed = 337.0000241088671
        self.assertTrue(rotation_matches(observed, -23, resource=True))
        self.assertFalse(rotation_matches(observed, -23))
        self.assertFalse(rotation_matches(337.002, -23, resource=True))
        self.assertFalse(rotation_matches(338, -23, resource=True))

    def test_unchanged_tolerance_boundaries_and_small_signed_errors(self):
        self.assertTrue(rotation_matches(.001, 0, resource=True))
        self.assertFalse(rotation_matches(.001000001, 0, resource=True))
        self.assertTrue(rotation_matches(1e-5, 0))
        self.assertFalse(rotation_matches(1.000001e-5, 0))
        self.assertTrue(rotation_matches(359.9995, 0, resource=True))
        self.assertTrue(rotation_matches(-.0005, 360, resource=True))
        self.assertFalse(rotation_matches(359.998, 0, resource=True))

    def test_invalid_and_nonfinite_angles_are_rejected(self):
        for invalid in (True, False, None, "337", [], {}, float("nan"),
                        float("inf"), -float("inf"), 10**1000):
            for resource in (False, True):
                self.assertFalse(rotation_matches(invalid, 0, resource=resource))
                self.assertFalse(rotation_matches(0, invalid, resource=resource))
        self.assertFalse(rotation_matches(0, 0, resource="true"))
        self.assertFalse(rotation_matches(0, 0, resource=1))

    def test_large_finite_angles_cannot_overflow_before_reduction(self):
        self.assertTrue(rotation_matches(1e308, 1e308))
        self.assertFalse(rotation_matches(1e308, -1e308))


if __name__ == "__main__":
    unittest.main()
