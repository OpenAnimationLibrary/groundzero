"""Prove the shard plan keeps every test once and the shard judge catches what a shard hid."""

from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_shards as shards


class Plan(unittest.TestCase):
    def test_every_test_lands_in_exactly_one_shard(self) -> None:
        names = [f"test_{index}" for index in range(23)]
        plan = shards.plan(names, {"test_3": 50.0, "test_7": 20.0}, 4)
        self.assertEqual(len(plan), 4)
        self.assertEqual(sorted(name for shard in plan for name in shard), sorted(names))

    def test_the_longest_tests_are_spread_first(self) -> None:
        durations = {"test_a": 10.0, "test_b": 10.0, "test_c": 1.0, "test_d": 1.0}
        plan = shards.plan(sorted(durations), durations, 2)
        self.assertEqual(
            [sorted(shard) for shard in plan], [["test_a", "test_c"], ["test_b", "test_d"]]
        )

    def test_serial_tests_share_the_first_shard_and_count_as_its_load(self) -> None:
        serial = sorted(shards.SERIAL_TESTS)
        names = [*serial, "test_x", "test_y"]
        durations = {name: 30.0 for name in serial} | {"test_x": 5.0, "test_y": 5.0}
        plan = shards.plan(names, durations, 3)
        self.assertEqual(plan[0], serial)
        self.assertEqual(sorted(plan[1] + plan[2]), ["test_x", "test_y"])

    def test_fewer_tests_than_shards_leaves_no_empty_shard(self) -> None:
        self.assertEqual(shards.plan(["test_a"], {}, 8), [["test_a"]])

    def test_a_shard_count_below_one_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            shards.shard_count("0")


class Judge(unittest.TestCase):
    def setUp(self) -> None:
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.output = Path(folder.name)
        self.shard = shards.Shard(0, ["test_a", "test_b"], self.output / "shard-0")
        self.shard.output.mkdir()
        self.shard.process = subprocess.Popen([sys.executable, "-c", "pass"])
        self.shard.process.wait()

    def report(self, *names: str) -> None:
        durations = {name: 1.0 for name in names}
        (self.shard.output / shards.DURATIONS_NAME).write_text(json.dumps(durations))

    def test_a_shard_that_reported_every_test_has_no_problem(self) -> None:
        self.report("test_a", "test_b")
        self.assertEqual(self.shard.problems(), [])

    def test_a_shard_that_ended_early_is_a_failure(self) -> None:
        self.report("test_a")
        self.assertIn("without reporting 1 test(s), first test_b", self.shard.problems()[0])

    def test_a_shard_that_reported_nothing_is_a_failure(self) -> None:
        self.assertIn("without reporting 2 test(s)", self.shard.problems()[0])

    def test_a_worker_that_missed_its_exit_deadline_is_a_failure(self) -> None:
        self.report("test_a", "test_b")
        case = self.shard.output / "test_a-x"
        case.mkdir()
        (case / "worker.stderr.stalled-exit.txt").write_text("no sampler")
        self.assertIn("missed the exit deadline", self.shard.problems()[0])

    def test_the_judged_run_fails_and_records_durations(self) -> None:
        self.report("test_a")
        with mock.patch.object(shards, "annotation") as annotate:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(shards._judge([self.shard], self.output), 1)
        annotate.assert_called_once()
        recorded = json.loads((self.output / shards.DURATIONS_NAME).read_text())
        self.assertEqual(recorded, {"test_a": 1.0})


if __name__ == "__main__":
    unittest.main()
