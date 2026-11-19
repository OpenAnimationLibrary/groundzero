"""Prove the cell interval operations of the property model against rows written by hand."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_property_cells as sheet

A, B, C, D, E = (("L", number, "") for number in (1, 2, 3, 4, 5))


def retime(rows: sheet.Rows, start: int, length: int, **operation: Any) -> sheet.Rows:
    return sheet.retime(rows, start, length, operation)


class IntervalTest(unittest.TestCase):
    def test_clear_blanks_without_moving_anything(self) -> None:
        self.assertEqual(sheet.clear([A, B, C, D], 1, 2), [A, None, None, D])

    def test_clear_beyond_the_last_cell_adds_nothing(self) -> None:
        self.assertEqual(sheet.clear([A, B], 5, 3), [A, B])

    def test_remove_moves_later_rows_up(self) -> None:
        self.assertEqual(sheet.remove([A, B, C, D], 1, 2), [A, D])
        self.assertEqual(sheet.remove([A, B, C, D], 2, 9), [A, B])

    def test_insert_moves_later_rows_down_unless_there_are_none(self) -> None:
        self.assertEqual(sheet.insert([A, B, C], 1, 2), [A, None, None, B, C])
        self.assertEqual(sheet.insert([A, B, C], 3, 2), [A, B, C])

    def test_a_blank_run_blanks_and_runs_do_not_shift_later_rows(self) -> None:
        runs = [
            {"level": "L", "number": 9, "suffix": "", "length": 2},
            {"empty": True, "length": 1},
        ]
        self.assertEqual(
            sheet.write_runs([A, B, C, D, E], 1, runs), [A, ("L", 9, ""), ("L", 9, ""), None, E]
        )

    def test_occupied_sees_only_the_target_rows(self) -> None:
        runs = [{"empty": True, "length": 2}]
        self.assertFalse(sheet.occupied([A, None, None, D], 1, runs))
        self.assertTrue(sheet.occupied([A, None, C, D], 1, runs))

    def test_copy_inserts_or_replaces(self) -> None:
        self.assertEqual(sheet.copy_block([A, B, C], [D, E], 1, "insert"), [A, D, E, B, C])
        self.assertEqual(sheet.copy_block([A, B, C], [D, E], 1, "replace"), [A, D, E])
        self.assertEqual(sheet.copy_block([A], [D], 3, "replace"), [A, None, None, D])

    def test_replace_changes_the_level_or_the_pair_within_the_interval(self) -> None:
        rows = [A, ("M", 1, ""), A, A]
        self.assertEqual(
            sheet.replace_level(rows, 0, 3, "L", "N"), [("N", 1, ""), ("M", 1, ""), ("N", 1, ""), A]
        )
        self.assertEqual(sheet.replace_pairs(rows, 1, 3, [(A, B)]), [A, ("M", 1, ""), B, B])


class RetimeTest(unittest.TestCase):
    def test_reverse_and_swing(self) -> None:
        self.assertEqual(retime([A, B, C, D], 0, 3, kind="reverse"), [C, B, A, D])
        self.assertEqual(retime([A, B, C, D], 0, 3, kind="swing"), [A, B, C, B, A, D])

    def test_step_and_each(self) -> None:
        self.assertEqual(retime([A, B, C], 0, 2, kind="step", factor=2), [A, A, B, B, C])
        self.assertEqual(retime([A, B, C, D, E], 0, 5, kind="each", factor=2), [A, C, E])
        self.assertEqual(retime([A, B, C, D], 0, 2, kind="each", factor=3), [A, B, C, D])

    def test_step_and_each_after_an_earlier_step_are_not_inverses_of_a_blank_tail(self) -> None:
        self.assertEqual(retime([A, B], 0, 4, kind="step", factor=2), [A, A, B, B])

    def test_reframe_shows_each_distinct_neighbour_for_the_step(self) -> None:
        operation = {"kind": "reframe", "step": 2}
        self.assertEqual(retime([A, A, A, B, B, C], 0, 6, **operation), [A, A, B, B, C, C])

    def test_the_step_kinds_work_on_holds_and_leave_blanks_alone(self) -> None:
        rows = [A, A, A, B, None, None, C, C]
        self.assertEqual(
            retime(rows, 0, 8, kind="increase_step"),
            [A] * 4 + [B] * 2 + [None, None] + [C] * 3,
        )
        self.assertEqual(retime(rows, 0, 8, kind="decrease_step"), [A, A, B, None, None, C])
        self.assertEqual(retime(rows, 0, 8, kind="reset_step"), [A, B, None, C])

    def test_roll_up_and_down(self) -> None:
        self.assertEqual(retime([A, B, C, D], 0, 3, kind="roll_up"), [B, C, A, D])
        self.assertEqual(retime([A, B, C, D], 0, 3, kind="roll_down"), [C, A, B, D])

    def test_repeat_fills_to_the_row_and_inserts(self) -> None:
        self.assertEqual(retime([A, B, C], 0, 2, kind="repeat", until_frame=4), [A, B, A, B, A, C])

    def test_reframe_extends_over_the_hold_the_interval_ends_in(self) -> None:
        rows = [A, A, B, B, B, C, C]
        self.assertEqual(
            retime(
                rows, 0, 4, kind="reframe", step=3, blank_entries="keep", extend_over_hold=False
            ),
            [A, A, A, B, B, B, B, C, C],
        )
        self.assertEqual(
            retime(rows, 0, 4, kind="reframe", step=3, blank_entries="keep", extend_over_hold=True),
            [A, A, A, B, B, B, C, C],
        )

    def test_the_arguments_that_cannot_work_are_refused(self) -> None:
        self.assertEqual(
            sheet.retime_refusal(1, 0, {"kind": "swing"}), "invalid_params/swing_length"
        )
        self.assertEqual(
            sheet.retime_refusal(4, 0, {"kind": "repeat", "until_frame": 2}),
            "invalid_params/until_frame",
        )
        self.assertIsNone(sheet.retime_refusal(4, 0, {"kind": "repeat", "until_frame": 4}))


if __name__ == "__main__":
    unittest.main()
