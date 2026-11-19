"""Prove a known violation passes only its recorded answer and goes stale when fixed."""

from __future__ import annotations

import dataclasses
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_known_violations as known


class FakeCase:
    def __init__(self, directory: Path) -> None:
        self.directory = directory

    def id(self) -> str:
        return "suite.test_fake"

    def fail(self, message: str) -> None:
        raise AssertionError(message)


# The committed table may be empty, so the checks run on an entry of their own.
SAMPLE = known.Violation(
    name="A tool answers with a code it does not declare",
    tool="fixture.tool",
    call="fixture.tool {name: 129 x 'm'}",
    expected="a code the tool declares",
    observed='error code "invalid_argument"',
    answers=frozenset({"invalid_argument"}),
    row="TEST",
)


def table(**entries: known.Violation) -> Any:
    return mock.patch.object(known, "KNOWN_VIOLATIONS", entries)


def settled(*names: str) -> Any:
    """Pretend one case module passes these names to settle()."""
    return mock.patch.object(known, "_settle_names", side_effect=[set(names)] + [set()] * 999)


class SettleTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.case: Any = FakeCase(Path(self.directory.name))
        patched = table(KV_900=SAMPLE)
        patched.start()
        self.addCleanup(patched.stop)

    def settle(self, observed: object, expected: object) -> None:
        known.settle(self.case, "KV_900", observed, expected)

    def test_the_recorded_answer_passes_and_is_logged(self) -> None:
        self.settle("invalid_argument", {"invalid_params"})
        log = (self.case.directory / known.LOG_NAME).read_text()
        self.assertIn('"violation": "KV_900"', log)

    def test_the_expected_answer_means_the_entry_is_stale(self) -> None:
        with self.assertRaisesRegex(AssertionError, "delete its entry"):
            self.settle("invalid_params", {"invalid_params"})

    def test_a_different_answer_is_a_new_defect(self) -> None:
        with self.assertRaisesRegex(AssertionError, "neither the expected"):
            self.settle("native_error", {"invalid_params"})

    def test_a_single_expected_value_is_accepted(self) -> None:
        with self.assertRaises(AssertionError):
            self.settle("invalid_params", "invalid_params")


class TableTest(unittest.TestCase):
    def test_the_committed_table_is_complete_and_used(self) -> None:
        self.assertEqual(known.problems(), [])

    def test_an_entry_with_a_case_passes(self) -> None:
        with table(KV_900=SAMPLE), settled("KV_900"):
            self.assertEqual(known.problems(), [])

    def test_an_entry_without_a_case_is_reported(self) -> None:
        with table(KV_900=SAMPLE), settled():
            self.assertEqual(len(known.problems()), 1)
            self.assertIn("KV_900", known.problems()[0])

    def test_an_empty_field_is_reported(self) -> None:
        with table(KV_900=dataclasses.replace(SAMPLE, call=" ")), settled("KV_900"):
            self.assertEqual(len(known.problems()), 1)

    def test_a_settled_name_without_an_entry_is_reported(self) -> None:
        with table(), settled("KV_900"):
            self.assertTrue(any("KV_900" in problem for problem in known.problems()))


if __name__ == "__main__":
    unittest.main()
