"""Prove the curve semantics of the property model against values computed by hand."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_property_curves as curves
from mcp_property_curves import Key


def rich() -> dict[int, Key]:
    """Speed, ease, linear, constant and linear segments of the animation cases."""
    keys = [
        Key(0, 0, "speed_in_out", 1, ("none",), ("speed", 4, 0)),
        Key(12, 120, "ease_in_out", 1, ("speed", -4, -24), ("ease", 3), linked=False),
        Key(24, 240, "linear", 1, ("ease", 3), None, linked=False),
        Key(36, 360, "constant"),
        Key(48, 100, "linear"),
    ]
    keys[0].linked = False
    return {key.frame: key for key in keys}


class SampleTest(unittest.TestCase):
    def values(self, frames: list[float], cycle: bool = False) -> list[float]:
        items = curves.ordered(rich())
        return [round(curves.sample(items, frame, cycle), 6) for frame in frames]

    def test_a_speed_segment_is_the_bezier_through_its_handles(self) -> None:
        # Control points (0,0) (4,0) (8,96) (12,120): at t = 1/2 the frame is 6 and the value 51.
        self.assertEqual(self.values([0, 6, 12]), [0, 51, 120])

    def test_an_ease_segment_rises_holds_and_falls_in_speed(self) -> None:
        # 120 over 12 frames with 3 frames of ease at each end: peak speed 120 / 9 per frame, so
        # the value at frame 15 is 120 + 20, the middle is 180, and frame 21 is 220.
        self.assertEqual(self.values([15, 18, 21]), [140, 180, 220])

    def test_a_one_sided_ease_ramps_only_that_side(self) -> None:
        first = Key(0, 0, "ease_in_out", 1, ("none",), ("ease", 2), linked=False)
        last = Key(6, 22, "linear", 1, ("ease", 0), None, linked=False)
        # Speed rises over 2 frames to v, then holds: area v * (1 + 4) = 22, so v = 4.4.
        self.assertAlmostEqual(curves.sample([first, last], 1), 4.4 / 4)
        self.assertAlmostEqual(curves.sample([first, last], 4), 4.4 * 3)

    def test_a_step_above_one_holds_the_value_between_steps(self) -> None:
        keys = [Key(0, 0, "linear", 3), Key(12, 12, "linear")]
        values = [curves.sample(keys, frame) for frame in (2, 3, 4, 5, 6)]
        self.assertEqual(values, [0, 3, 3, 3, 6])

    def test_constant_and_linear_follow_their_first_key(self) -> None:
        # Linear from 240 at frame 24 to 360 at 36, then constant until the last key.
        self.assertEqual(self.values([30, 36, 42, 48]), [300, 360, 360, 100])

    def test_a_cyclic_curve_repeats_its_span_and_drifts(self) -> None:
        # The span is 48 frames and the last value is 100 above the first.
        self.assertEqual(self.values([48, 54, 60, 96, 100], True), [100, 151, 220, 200, 225.777778])

    def test_a_cycle_needs_two_keys(self) -> None:
        self.assertEqual(curves.sample([Key(6, 5, "linear")], 100, True), 5)


class EditTest(unittest.TestCase):
    def test_move_shifts_keys_and_keeps_their_controls(self) -> None:
        error, moved = curves.move_keys(rich(), [36, 48], 6, 10)
        self.assertIsNone(error)
        self.assertEqual(sorted(moved), [0, 12, 24, 42, 54])
        self.assertEqual(moved[42].value, 370)

    def test_move_names_what_it_refuses(self) -> None:
        keys = rich()
        self.assertEqual(curves.move_keys(keys, [36], 12, 0)[0], "invalid_argument/key_exists")
        self.assertEqual(curves.move_keys(keys, [36], 18, 0)[0], "invalid_argument/order_changed")
        self.assertEqual(
            curves.move_keys(keys, [0], -1, 0)[0], "invalid_argument/frame_out_of_range"
        )
        self.assertEqual(
            curves.move_keys(keys, [0], 0, 2000000)[0], "invalid_argument/value_out_of_bounds"
        )
        self.assertEqual(curves.move_keys(keys, [7], 6, 0)[0], "missing_key")
        self.assertEqual(curves.move_keys(keys, [12], 20, 0)[0], "invalid_argument/order_changed")

    def test_move_refuses_a_handle_that_no_longer_fits(self) -> None:
        # Moving the key at 24 back to 13 leaves 1 frame for an ease of 3.
        self.assertEqual(curves.move_keys(rich(), [24], -11, 0)[0], "invalid_argument/controls")

    def test_a_move_by_nothing_changes_nothing(self) -> None:
        keys = rich()
        self.assertEqual(curves.move_keys(keys, [0], 0, 0), (None, keys))

    def test_duplicate_copies_a_key_verbatim(self) -> None:
        keys = {0: Key(0, 1, "linear"), 6: Key(6, 2, "linear", 3)}
        error, result = curves.duplicate_keys(keys, [6], 12, 5)
        self.assertIsNone(error)
        self.assertEqual((result[18].value, result[18].step), (7, 3))

    def test_duplicate_refuses_a_landing_on_a_key_and_a_change_of_incoming_kind(self) -> None:
        keys = rich()
        self.assertEqual(curves.duplicate_keys(keys, [36], 12, 0)[0], "invalid_argument/key_exists")
        # A copy of the linear key at 24 inside the ease segment, after a linked copy of the
        # constant key, would change the next key's incoming kind.
        self.assertIn(
            curves.duplicate_keys(keys, [36], -20, 0)[0],
            {"unsupported_interpolation", "invalid_argument/incoming_kind_changed"},
        )

    def test_a_copy_after_its_source_gets_the_sources_kind_of_segment_not_the_original(
        self,
    ) -> None:
        keys = {
            0: Key(0, 0, "speed_in_out", 1, ("none",), ("speed", 2, 0), linked=False),
            6: Key(6, 10, "linear", 1, ("speed", -2, 0), None, linked=False),
        }
        error, _ = curves.duplicate_keys(keys, [6], 14, 0)
        self.assertEqual(error, "invalid_argument/incoming_kind_changed")

    def test_a_linked_copy_after_a_rich_segment_is_unsupported(self) -> None:
        keys = {
            0: Key(0, 0, "ease_in_out", 1, ("none",), ("ease", 1), linked=False),
            12: Key(12, 5, "linear", 1, ("ease", 1), None, linked=False),
            24: Key(24, 9, "linear"),
        }
        error, _ = curves.duplicate_keys(keys, [24], -18, 0)
        self.assertEqual(error, "unsupported_interpolation")

    def test_breakdown_in_a_linear_segment_takes_the_segments_type_and_step(self) -> None:
        keys = {0: Key(0, 0, "linear", 3), 12: Key(12, 12, "linear")}
        error, result = curves.insert_breakdown(keys, 6, 50, None)
        self.assertIsNone(error)
        self.assertEqual(
            (result[6].value, result[6].interpolation, result[6].step), (6, "linear", 3)
        )

    def test_breakdown_by_value_and_by_favour(self) -> None:
        keys = {0: Key(0, 10, "linear"), 10: Key(10, 20, "linear")}
        self.assertEqual(curves.insert_breakdown(keys, 4, 150, None)[1][4].value, 25)
        self.assertEqual(curves.insert_breakdown(keys, 4, None, -3)[1][4].value, -3)

    def test_breakdown_refuses_a_key_a_frame_outside_and_a_value_out_of_bounds(self) -> None:
        keys = {0: Key(0, 10, "linear"), 10: Key(10, 20, "linear")}
        self.assertEqual(
            curves.insert_breakdown(keys, 10, 0, None)[0], "invalid_argument/key_exists"
        )
        self.assertEqual(
            curves.insert_breakdown(keys, 11, 0, None)[0], "invalid_argument/outside_segment"
        )
        self.assertEqual(
            curves.insert_breakdown(keys, 5, None, 2e6)[0], "invalid_argument/value_out_of_bounds"
        )

    def test_breakdown_in_an_ease_segment_gives_the_new_key_no_ease_and_clamps_the_ends(
        self,
    ) -> None:
        keys = rich()
        _, result = curves.insert_breakdown(keys, 14, 50, None)
        self.assertEqual((result[14].incoming, result[14].outgoing), (("ease", 0), ("ease", 0)))
        self.assertEqual(result[12].outgoing, ("ease", 2))
        self.assertEqual(result[24].incoming, ("ease", 3))

    def test_breakdown_on_the_curve_keeps_a_speed_segments_shape(self) -> None:
        keys = rich()
        on_curve = curves.sample(curves.ordered(keys), 6)
        _, result = curves.insert_breakdown(keys, 6, None, on_curve)
        for frame in (0, 1.5, 3, 4.5, 6, 7.5, 9, 10.5, 12):
            self.assertAlmostEqual(
                curves.sample(curves.ordered(result), frame),
                curves.sample(curves.ordered(keys), frame),
                places=6,
            )


class GeneratorTest(unittest.TestCase):
    def test_random_curves_fit_their_handles_and_keep_a_defined_shape(self) -> None:
        import random

        for seed in range(300):
            keys = {key.frame: key for key in curves.random_keys(random.Random(seed))}
            self.assertIsNone(curves.native_refusal(keys), seed)
            self.assertTrue(curves.shape_is_defined(keys), seed)

    def test_a_speed_segment_has_a_defined_shape_unless_its_handles_cross(self) -> None:
        def shaped(outgoing: float, incoming: float) -> bool:
            keys = {
                0: Key(0, 0, "speed_in_out", 1, ("none",), ("speed", outgoing, 1), linked=False),
                6: Key(6, 5, "linear", 1, ("speed", incoming, 1), None, linked=False),
            }
            return curves.shape_is_defined(keys)

        self.assertTrue(shaped(4, 0))
        self.assertTrue(shaped(0, -4))
        self.assertTrue(shaped(4, -0.001))
        self.assertTrue(shaped(0, -3))
        self.assertFalse(shaped(5, -2))


if __name__ == "__main__":
    unittest.main()
