"""Prove the break-check runner kills a guarded test, reports a survivor and restores the tree."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_break_checks as breaks

GUARDED = "def limit(value):\n    if value > 10:\n        return 10\n    return value\n"
LIGHTLY_TESTED = "def label(value):\n    return 'big' if value > 100 else 'small'\n"
TEST = (
    "import sys, unittest\nsys.path.insert(0, 'ci-scripts')\n"
    "import guarded\n\n\nclass Guarded(unittest.TestCase):\n"
    "    def test_limit(self):\n        self.assertEqual(guarded.limit(50), 10)\n"
    "    def test_label(self):\n        self.assertEqual(guarded.label(1), 'small')\n"
)


def git(root: Path, *arguments: str) -> None:
    subprocess.run(["git", "-C", str(root), *arguments], check=True, capture_output=True)


def make_repository(root: Path) -> None:
    (root / "ci-scripts" / "tests").mkdir(parents=True)
    (root / "ci-scripts" / "guarded.py").write_text(GUARDED + "\n\n" + LIGHTLY_TESTED)
    (root / "ci-scripts" / "tests" / "test_guarded.py").write_text(TEST)
    git(root, "init", "-q")
    git(root, "config", "user.name", "test")
    git(root, "config", "user.email", "test@users.noreply.github.com")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "fixture")


def guard(name: str, find: str, replace: str) -> dict[str, Any]:
    return {
        "id": name,
        "file": "ci-scripts/guarded.py",
        "find": find,
        "replace": replace,
        "why": "fixture",
        "tests": [{"kind": "unit", "pattern": "test_guarded.py"}],
    }


class PatchTest(unittest.TestCase):
    def test_the_text_to_break_must_occur_exactly_once(self) -> None:
        text = "a\nb\na\n"
        self.assertEqual(breaks.apply_patch(text, guard("g", "b", "c")), "a\nc\na\n")
        with self.assertRaises(ValueError):
            breaks.apply_patch(text, guard("g", "a", "c"))
        with self.assertRaises(ValueError):
            breaks.apply_patch(text, guard("g", "z", "c"))


class RunTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="gzq-bt-", dir="/tmp"))
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        self.repo = self.directory / "repo"
        self.repo.mkdir()
        make_repository(self.repo)

    def options(self) -> argparse.Namespace:
        return argparse.Namespace(
            repo=self.repo,
            output=self.directory / "out",
            app=None,
            gate=None,
            env=[],
            media_tools=None,
            native_build=None,
            native_app=None,
        )

    def test_a_guard_that_a_test_notices_is_killed_and_one_that_none_does_is_reported(self) -> None:
        guards = [
            guard("limit", "if value > 10:", "if value > 1000:"),
            guard("label", "'big' if value > 100", "'big' if value > 1000"),
        ]
        outcomes = breaks.break_all(guards, self.options())
        self.assertEqual(
            {outcome.guard: outcome.status for outcome in outcomes},
            {"limit": "killed", "label": "survived"},
        )
        status = (self.repo / "ci-scripts" / "guarded.py").read_text()
        self.assertEqual(status, GUARDED + "\n\n" + LIGHTLY_TESTED)

    def test_the_report_names_survivors_and_fails(self) -> None:
        options = self.options()
        outcomes = breaks.break_all([guard("label", "'big' if value > 100", "'big'")], options)
        self.assertEqual(outcomes[0].status, "killed")
        survivor = breaks.Outcome("label", "survived", "every named test still passed")
        self.assertEqual(breaks.write_report([survivor], options), 1)
        gaps = (options.output / "break-gaps.md").read_text()
        self.assertIn("- label (survived)", gaps)
        results = json.loads((options.output / "break-results.json").read_text())
        self.assertEqual(results["guards"][0]["guard"], "label")
        self.assertEqual(breaks.write_report([breaks.Outcome("a", "killed")], options), 0)

    def test_a_test_that_fails_before_anything_is_broken_stops_the_run(self) -> None:
        (self.repo / "ci-scripts" / "guarded.py").write_text("def limit(v):\n    return v\n")
        git(self.repo, "commit", "-q", "-am", "break the fixture")
        with self.assertRaises(SystemExit):
            breaks.break_all([guard("limit", "return v", "return 0")], self.options())

    def test_an_acceptance_guard_without_an_app_is_not_run(self) -> None:
        acceptance = guard("limit", "x", "y")
        acceptance["tests"] = [{"kind": "acceptance", "names": ["anything"]}]
        outcomes = breaks.break_all([acceptance], self.options())
        self.assertEqual(outcomes[0].status, "not run")

    def test_anchors_are_checked_against_the_committed_text(self) -> None:
        good = guard("limit", "if value > 10:", "if value > 1000:")
        stale = guard("old", "text that is gone", "x")
        outcomes = breaks.check_anchors(self.repo, [good, stale])
        self.assertEqual([o.status for o in outcomes], ["applies", "invalid"])


class TableTest(unittest.TestCase):
    def test_every_guard_of_the_table_names_a_file_a_unique_id_and_a_test(self) -> None:
        table = json.loads(breaks.GUARDS.read_text())
        root = Path(__file__).resolve().parents[2]
        self.assertEqual(len({g["id"] for g in table}), len(table))
        for entry in table:
            self.assertTrue((root / entry["file"]).is_file(), entry["id"])
            self.assertTrue(entry["tests"], entry["id"])
            self.assertNotEqual(entry["find"], entry["replace"], entry["id"])

    def test_every_patch_applies_once_to_the_text_of_this_checkout(self) -> None:
        table = json.loads(breaks.GUARDS.read_text())
        root = Path(__file__).resolve().parents[2]
        for entry in table:
            text = (root / entry["file"]).read_text(encoding="utf-8")
            breaks.apply_patch(text, entry)


if __name__ == "__main__":
    unittest.main()
