"""CI-only benchmark contracts; no app build or performance thresholds."""

import argparse
import copy
import io
import json
import os
from pathlib import Path
import platform
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import paired_benchmark as benchmark


class BenchmarkContracts(unittest.TestCase):
    def setUp(self):
        self.storage = tempfile.TemporaryDirectory()
        self.addCleanup(self.storage.cleanup)
        self.root = Path(self.storage.name)
        self.output = self.root / "results"
        self.output.mkdir()
        self.provenance = self.root / "provenance.json"
        self.provenance.write_text(json.dumps({"baseline": {}, "candidate": {}}))
        self.suite = {"schema": 1, "id": "fixture",
                      "required_phases": {"save plain": "bytes"},
                      "cases": [{"id": "sample", "arguments": ["--repeat", "1"]}]}
        self.metadata = {"platform": "windows" if os.name == "nt" else "macos",
                         "suite": self.suite, "suite_sha256": "fixture",
                         "build": {"architecture": platform.machine()}}
        self.args = argparse.Namespace(baseline=self.root / "base.tar.gz",
                                       candidate=self.root / "candidate.tar.gz",
                                       provenance=self.provenance, output=self.output,
                                       samples=3, warmups=1, timeout=5)

    def bundled(self, archive, destination, expected):
        return self.root, Path(sys.executable), copy.deepcopy(self.metadata)

    def measured(self, command, directory, environment, log, timeout):
        log.write_text("NOTICE: save plain 2.0 ms (123 bytes)\nscenebench_test done\n")
        return {"exit_code": 0, "wall_ms": 4.0, "peak_rss_bytes": 1024}

    def run_with(self, measurement=None, bundles=None):
        report = {}
        with mock.patch.object(benchmark, "read_bundle", side_effect=bundles or self.bundled), \
             mock.patch.object(benchmark, "measured_process", side_effect=measurement or self.measured) as calls, \
             mock.patch.object(benchmark, "environment_for", return_value=os.environ.copy()), \
             mock.patch.object(benchmark.platform, "system", return_value="Windows" if os.name == "nt" else "Darwin"):
            benchmark.run(self.args, report)
            return report, calls.call_count

    def test_alternation_warmup_exclusion_and_raw_units(self):
        report, count = self.run_with()
        self.assertEqual(count, 8)
        self.assertEqual([sample["side"] for sample in report["samples"]],
                         ["baseline", "candidate", "candidate", "baseline",
                          "baseline", "candidate", "candidate", "baseline"])
        self.assertEqual(sum(sample["warmup"] for sample in report["samples"]), 2)
        phase = report["comparisons"]["sample"]["phase/save plain/ms"]
        self.assertEqual(len(phase["baseline"]["samples"]), 3)
        self.assertEqual(len(phase["candidate"]["samples"]), 3)
        self.assertEqual(report["samples"][0]["phases"]["save plain"],
                         {"ms": 2.0, "count": 123, "unit": "bytes"})
        self.assertEqual(report["samples"][0]["process"]["peak_rss_bytes"], 1024)

    def test_workload_mismatch_fails_before_execution(self):
        def bundles(archive, destination, expected):
            root, executable, metadata = self.bundled(archive, destination, expected)
            if archive == self.args.candidate:
                metadata["suite"]["cases"][0]["arguments"] += ["--rows", "999"]
            return root, executable, metadata
        with self.assertRaisesRegex(ValueError, "suite differ"):
            self.run_with(bundles=bundles)
        self.assertFalse((self.output / "logs").exists())

    def test_failed_subprocess_is_not_a_measurement(self):
        def failure(*arguments):
            result = self.measured(*arguments)
            result["exit_code"] = 1
            return result
        with self.assertRaisesRegex(RuntimeError, "failed; see"):
            self.run_with(measurement=failure)

    def test_changed_fixture_source_is_incompatible(self):
        def bundles(archive, destination, expected):
            root, executable, metadata = self.bundled(archive, destination, expected)
            metadata["build"]["fixture_source_sha256"] = archive.name
            return root, executable, metadata
        with self.assertRaisesRegex(ValueError, "build differ"):
            self.run_with(bundles=bundles)

    def test_missing_or_wrong_units_fail(self):
        log = self.root / "fixture.log"
        for text in ("scenebench_test done\n",
                     "NOTICE: save plain 2.0 ms (123 cells)\nscenebench_test done\n",
                     "ignored argument --trailing-columns\nscenebench_test done\n"):
            log.write_text(text)
            with self.assertRaises(ValueError):
                benchmark.phases_from_log(log, self.suite)

    def test_unsupported_bundle_schema_rejected(self):
        archive = self.root / "invalid.tar.gz"
        data = json.dumps({"schema": 99}).encode()
        with tarfile.open(archive, "w:gz") as stream:
            info = tarfile.TarInfo("bundle/benchmark.json")
            info.size = len(data)
            stream.addfile(info, io.BytesIO(data))
        with self.assertRaisesRegex(ValueError, "Unsupported benchmark bundle schema"):
            benchmark.read_bundle(archive, self.root / "extract", {})

    def test_native_process_peak_and_exit_status(self):
        # Real OS accounting/handle lifetime, without starting the application
        # or asserting a machine-dependent duration/memory threshold.
        result = benchmark.measured_process(
            [sys.executable, "-c", "import sys; sys.stdout.write('x' * (1024 * 1024))"],
            self.root, os.environ.copy(), self.root / "process.log", 30)
        self.assertEqual(result["exit_code"], 0)
        self.assertGreater(result["peak_rss_bytes"], 0)
        self.assertEqual((self.root / "process.log").stat().st_size, 1024 * 1024)
        failed = benchmark.measured_process([sys.executable, "-c", "raise SystemExit(7)"],
                                           self.root, os.environ.copy(), self.root / "failed.log", 30)
        self.assertEqual(failed["exit_code"], 7)

    def test_timeout_terminates_and_closes_output(self):
        log = self.root / "timeout.log"
        with self.assertRaisesRegex(RuntimeError, "exceeded"):
            benchmark.measured_process([sys.executable, "-c", "import time; time.sleep(30)"],
                                       self.root, os.environ.copy(), log, 0.1)
        log.unlink()
        self.assertFalse(log.exists())


if __name__ == "__main__":
    unittest.main()
