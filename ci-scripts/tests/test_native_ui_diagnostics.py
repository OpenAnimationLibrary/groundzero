"""CI-only contracts for isolated diagnostics; never launch the application."""

from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_native_ui_test as runner


SUITES = ("function-workflow", "cast-workflow", "combined-timing",
          "combined-cast-vector", "combined-fidelity")
MARKERS = dict(zip(SUITES, ("function-workflow-native: PASS", "cast-native-workflow: PASS",
                          "function-timing-native: PASS",
                          "cast-vector-native: PASS",
                          "combined-fidelity-native: PASS")))
FULL_OUTPUT = "\n".join(("native-ui-integration: PASS", "vector-workflows: PASS",
                          "offline-gl-workflow: PASS", "function-workflow-native: PASS",
                          *MARKERS.values())) + "\n"


class NativeDiagnosticContracts(unittest.TestCase):
    def setUp(self):
        self.storage = tempfile.TemporaryDirectory()
        self.addCleanup(self.storage.cleanup)
        self.root = Path(self.storage.name)
        self.executable = self.root / "fixture-app"
        self.executable.write_bytes(b"mocked application; never executed\n")
        self.source = self.root / "stuff"
        self.source.mkdir()
        (self.source / "pristine.txt").write_text("source", encoding="utf-8")
        self.report = self.root / "reports" / "ui-integration-output.txt"

    def invoke(self, response, benchmark=False, extra_arguments=()):
        calls = []

        def launch(command, **options):
            environment, directory = options["env"], options["cwd"]
            suite = environment.get("GROUNDZERO_NATIVE_UI_SUITE")
            calls.append((suite, directory, options["timeout"],
                          environment.get("GROUNDZERO_ARTIST_BENCHMARK")))
            self.assertEqual(command, [str(self.executable.resolve()), "--test-native-ui"])
            self.assertEqual((directory / "portablestuff" / "pristine.txt").read_text(), "source")
            self.assertFalse((directory / "changed-by-process").exists())
            (directory / "changed-by-process").write_text("isolated", encoding="utf-8")
            return response(suite, command, options)

        arguments = ["native-runner", str(self.executable), str(self.source),
                     "--output", str(self.report)]
        arguments += (["--artist-benchmark"] if benchmark else
                      ["--require-function-workflow", "--require-vector-workflows",
                       "--require-combined-workflows"])
        arguments += list(extra_arguments)
        inherited = {"GROUNDZERO_NATIVE_UI_SUITE": "untrusted-inherited-selector",
                     "GROUNDZERO_ARTIST_BENCHMARK": "1"}
        with mock.patch.object(sys, "argv", arguments), \
             mock.patch.dict(os.environ, inherited), \
             mock.patch.object(runner.subprocess, "run", side_effect=launch), \
             redirect_stdout(io.StringIO()):
            status = runner.main()
        self.assertEqual(len({call[1] for call in calls}), len(calls))
        self.assertTrue(all(not call[1].exists() for call in calls))
        return status, calls, self.report.read_text(encoding="utf-8")

    @staticmethod
    def passed(suite, command):
        output = MARKERS[suite] + f"\nnative-ui-diagnostic: PASS {suite}\n"
        return subprocess.CompletedProcess(command, 0, output)

    def test_original_failure_cannot_be_replaced_by_diagnostic_success(self):
        for original_failure in ("nonzero", "timeout", "missing-marker", "missing-cast-marker"):
            with self.subTest(original_failure=original_failure):
                def response(suite, command, options):
                    if suite is not None:
                        return self.passed(suite, command)
                    if original_failure == "timeout":
                        raise subprocess.TimeoutExpired(command, options["timeout"], output=b"original timed out\n")
                    if original_failure == "missing-marker":
                        return subprocess.CompletedProcess(command, 0,
                                                           FULL_OUTPUT.replace(MARKERS["combined-fidelity"], ""))
                    if original_failure == "missing-cast-marker":
                        return subprocess.CompletedProcess(command, 0,
                                                           FULL_OUTPUT.replace(MARKERS["cast-workflow"], ""))
                    return subprocess.CompletedProcess(command, 17, "native-ui-integration: FAIL: original failure\n")

                status, calls, output = self.invoke(response)
                self.assertEqual(status, 1)
                self.assertEqual([call[0] for call in calls], [None, *SUITES])
                self.assertEqual([call[2] for call in calls], [600, 120, 120, 120, 120, 120])
                self.assertTrue(all(call[3] is None for call in calls))
                for suite in SUITES:
                    self.assertIn(f"{suite}=PASS (status 0)", output)
                    report = self.report.with_name(self.report.stem + f"-{suite}.txt")
                    self.assertIn("original all-workflows failure remains authoritative", report.read_text())

    def test_each_diagnostic_runs_after_another_fails_or_times_out(self):
        for first_result in ("failure", "timeout"):
            with self.subTest(first_result=first_result):
                def response(suite, command, options):
                    if suite is None:
                        return subprocess.CompletedProcess(command, 17, "original failure\n")
                    if suite == "combined-timing":
                        if first_result == "timeout":
                            raise subprocess.TimeoutExpired(command, 120, output=b"partial diagnostic output\n")
                        return subprocess.CompletedProcess(command, 23, "independent timing failure\n")
                    return self.passed(suite, command)

                status, calls, output = self.invoke(response)
                self.assertEqual(status, 1)
                self.assertEqual([call[0] for call in calls], [None, *SUITES])
                self.assertIn("combined-timing=FAIL", output)
                self.assertIn("combined-cast-vector=PASS", output)
                self.assertIn("combined-fidelity=PASS", output)
                if first_result == "timeout":
                    self.assertIn("partial diagnostic output", output)
                    self.assertIn("timed out after 120 seconds", output)

    def test_unselected_binary_cannot_supply_diagnostic_completion(self):
        def response(suite, command, options):
            return subprocess.CompletedProcess(command, 17 if suite is None else 0,
                                               "original failure\n" if suite is None else FULL_OUTPUT)

        status, calls, output = self.invoke(response)
        self.assertEqual(status, 1)
        self.assertEqual(len(calls), 6)
        for suite in SUITES:
            self.assertIn(f"{suite}=FAIL", output)
        self.assertIn("Missing isolated diagnostic checkpoints", output)

    def test_component_failure_does_not_hide_later_workflows(self):
        for failed_suite in ("function-workflow", "cast-workflow"):
            with self.subTest(failed_suite=failed_suite):
                def response(suite, command, options):
                    if suite is None or suite == failed_suite:
                        return subprocess.CompletedProcess(command, 19, "component failure\n")
                    return self.passed(suite, command)

                status, calls, output = self.invoke(response)
                self.assertEqual(status, 1)
                self.assertEqual([call[0] for call in calls], [None, *SUITES])
                self.assertIn(f"{failed_suite}=FAIL (status 19)", output)
                for suite in set(SUITES) - {failed_suite}:
                    self.assertIn(f"{suite}=PASS (status 0)", output)

    def test_successful_monolithic_pass_strips_inherited_selector_and_stays_single(self):
        status, calls, output = self.invoke(
            lambda suite, command, options: subprocess.CompletedProcess(command, 0, FULL_OUTPUT))
        self.assertEqual(status, 0)
        self.assertEqual([(call[0], call[3]) for call in calls], [(None, None)])
        self.assertNotIn("Independent diagnostics", output)

    def test_benchmark_strips_selector_and_never_launches_diagnostics(self):
        payload = {"schema": 1, "fixture": "native-artist-workload-v1"}
        benchmark_output = "artist-workload-json: " + json.dumps(payload) + "\nartist-workload: PASS\n"
        status, calls, _ = self.invoke(
            lambda suite, command, options: subprocess.CompletedProcess(command, 0, benchmark_output),
            benchmark=True)
        self.assertEqual(status, 0)
        self.assertEqual([(call[0], call[3]) for call in calls], [(None, "1")])

    def test_debugger_fixture_also_strips_inherited_selection(self):
        with mock.patch.dict(os.environ, {"GROUNDZERO_NATIVE_UI_SUITE": "combined-fidelity",
                                          "GROUNDZERO_ARTIST_BENCHMARK": "1"}):
            # Both debugger helpers use this exact fresh-process boundary.
            with runner.isolated_fixture(self.source, self.executable) as (_, environment):
                self.assertNotIn("GROUNDZERO_NATIVE_UI_SUITE", environment)
                self.assertNotIn("GROUNDZERO_ARTIST_BENCHMARK", environment)

    def test_isolated_crash_debugging_preserves_failure_and_continues(self):
        for debugger_fails in (False, True):
            with self.subTest(debugger_fails=debugger_fails):
                def response(suite, command, options):
                    if suite is None:
                        return subprocess.CompletedProcess(command, 17, "original failure\n")
                    if suite == "combined-cast-vector":
                        return subprocess.CompletedProcess(command, -11, "crashed before completion\n")
                    return self.passed(suite, command)

                # sys.platform controls only debugger selection here; leave
                # os.name/pathlib untouched on Windows and macOS test hosts.
                with mock.patch.object(runner.sys, "platform", "linux"), \
                     mock.patch.object(runner, "linux_crash_diagnostics") as debugger:
                    if debugger_fails:
                        debugger.side_effect = OSError("debugger unavailable")
                    else:
                        debugger.return_value = ("native-ui-diagnostic: PASS combined-cast-vector\n"
                                                 "diagnostic stack only\n")
                    status, calls, output = self.invoke(
                        response, extra_arguments=("--linux-crash-debugger",))
                    debugger.assert_called_once_with(self.executable.resolve(), self.source.resolve(),
                                                     "combined-cast-vector")
                self.assertEqual(status, 1)
                self.assertEqual([call[0] for call in calls], [None, *SUITES])
                self.assertIn("combined-cast-vector=FAIL (status -11)", output)
                self.assertIn("combined-fidelity=PASS", output)
                report = self.report.with_name(self.report.stem + "-combined-cast-vector-debugger.txt")
                self.assertIn("debugger unavailable" if debugger_fails else "diagnostic stack only",
                              report.read_text(encoding="utf-8"))

    def test_both_debuggers_launch_the_exact_selected_suite(self):
        kits = self.root / "sdk"
        cdb = kits / "Windows Kits" / "10" / "Debuggers" / "x64" / "cdb.exe"
        cdb.parent.mkdir(parents=True)
        cdb.write_bytes(b"mocked debugger; never executed\n")
        process = mock.Mock(returncode=0)
        process.communicate.return_value = (b"bounded diagnostic stack\n", None)
        inherited = {"ProgramFiles(x86)": str(kits),
                     "GROUNDZERO_NATIVE_UI_SUITE": "untrusted-inherited-selector",
                     "GROUNDZERO_ARTIST_BENCHMARK": "1"}
        for selected in (None, "function-workflow", "cast-workflow", "combined-cast-vector"):
            for debugger_kind in ("linux", "windows"):
                with self.subTest(selected=selected, debugger=debugger_kind), \
                     mock.patch.dict(os.environ, inherited), \
                     mock.patch.object(runner.shutil, "which", return_value="mock-gdb"), \
                     mock.patch.object(runner.subprocess, "Popen", return_value=process) as launch:
                    if debugger_kind == "linux":
                        runner.linux_crash_diagnostics(self.executable, self.source, selected)
                    else:
                        runner.windows_crash_diagnostics(self.executable, self.source, 0xc0000005,
                                                         (), native_suite=selected)
                    options = launch.call_args.kwargs
                    self.assertEqual(options["env"].get("GROUNDZERO_NATIVE_UI_SUITE"), selected)
                    self.assertNotIn("GROUNDZERO_ARTIST_BENCHMARK", options["env"])
                    self.assertFalse(options["cwd"].exists())

    def test_unknown_diagnostic_selector_is_rejected(self):
        environment = {}
        with self.assertRaises(ValueError):
            runner.select_diagnostic_suite(environment, "unknown-fixture")
        self.assertNotIn("GROUNDZERO_NATIVE_UI_SUITE", environment)


if __name__ == "__main__":
    unittest.main()
