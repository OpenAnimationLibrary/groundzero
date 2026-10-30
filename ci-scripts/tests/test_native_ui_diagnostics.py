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
          "combined-cast-vector", "combined-fidelity", "vector-renderer")
MARKERS = dict(zip(SUITES, ("function-workflow-native: PASS", "cast-native-workflow: PASS",
                          "function-timing-native: PASS",
                          "cast-vector-native: PASS",
                          "combined-fidelity-native: PASS",
                          "vector-renderer-native: PASS")))
ICON_LIFECYCLE_LINES = (
    "icon-lifecycle-native: PASS (actual workers and Qt TLS retired)",
    "native-ui-integration: checkpoint: native icon workers drained",
)
FULL_OUTPUT = "\n".join(("native-ui-integration: PASS", "vector-workflows: PASS",
                          "offline-gl-workflow: PASS", "function-workflow-native: PASS",
                          *MARKERS.values(), *ICON_LIFECYCLE_LINES)) + "\n"
TIMING_LIFECYCLE_LINES = (
    "function-timing-native: PASS (native locks, undo and scene roundtrip)",
    "native-ui-diagnostic: PASS combined-timing",
    *ICON_LIFECYCLE_LINES,
    "native-ui-integration: checkpoint: native application cleanup completed",
)
TIMING_LIFECYCLE_OUTPUT = "\n".join(TIMING_LIFECYCLE_LINES) + "\n"


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

    def invoke(self, response, benchmark=False, extra_arguments=(), retained_count=0,
               timing_lifecycle=False):
        calls = []

        def launch(command, **options):
            environment, directory = options["environment"], options["cwd"]
            suite = environment.get("GROUNDZERO_NATIVE_UI_SUITE")
            calls.append((suite, directory, options["timeout"],
                          environment.get("GROUNDZERO_ARTIST_BENCHMARK")))
            self.assertEqual(command, [str(self.executable.resolve()), "--test-native-ui"])
            self.assertEqual((directory / "portablestuff" / "pristine.txt").read_text(), "source")
            self.assertFalse((directory / "changed-by-process").exists())
            (directory / "changed-by-process").write_text("isolated", encoding="utf-8")
            result = response(suite, command, options)
            # The shared captured-process boundary returns bytes on every OS;
            # the native runner owns UTF-8 decoding and report formatting.
            if isinstance(result.stdout, str):
                result.stdout = result.stdout.encode("utf-8")
            return result

        arguments = ["native-runner", str(self.executable), str(self.source),
                     "--output", str(self.report)]
        arguments += (["--isolated-timing-lifecycle"] if timing_lifecycle else
                      ["--artist-benchmark"] if benchmark else
                      ["--require-function-workflow", "--require-vector-workflows",
                       "--require-combined-workflows"])
        arguments += list(extra_arguments)
        inherited = {"GROUNDZERO_NATIVE_UI_SUITE": "untrusted-inherited-selector",
                     "GROUNDZERO_ARTIST_BENCHMARK": "1"}
        with mock.patch.object(sys, "argv", arguments), \
             mock.patch.dict(os.environ, inherited), \
             mock.patch.object(runner, "run_captured_process", side_effect=launch), \
             redirect_stdout(io.StringIO()):
            status = runner.main()
        self.assertEqual(len({call[1] for call in calls}), len(calls))
        retained = [call[1] for call in calls if call[1].exists()]
        for directory in retained:
            # The fake process owns no handles; clean up only after observing
            # whether production respected the unsafe retention boundary.
            self.addCleanup(runner.shutil.rmtree, directory)
        self.assertEqual(len(retained), retained_count)
        return status, calls, self.report.read_text(encoding="utf-8")

    @staticmethod
    def passed(suite, command):
        output = (MARKERS[suite] + f"\nnative-ui-diagnostic: PASS {suite}\n" +
                  "\n".join(ICON_LIFECYCLE_LINES) + "\n")
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
                self.assertEqual([call[2] for call in calls], [600] + [120] * len(SUITES))
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
        self.assertEqual(len(calls), 1 + len(SUITES))
        for suite in SUITES:
            self.assertIn(f"{suite}=FAIL", output)
        self.assertIn("Missing isolated diagnostic checkpoints", output)

    def test_component_failure_does_not_hide_later_workflows(self):
        for failed_suite in ("function-workflow", "cast-workflow", "vector-renderer"):
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

    def test_required_timing_lifecycle_uses_one_clean_selected_process(self):
        with mock.patch.object(runner, "macos_crash_diagnostics") as debugger:
            status, calls, output = self.invoke(
                lambda suite, command, options: subprocess.CompletedProcess(
                    command, 0, TIMING_LIFECYCLE_OUTPUT), timing_lifecycle=True,
                extra_arguments=("--macos-crash-debugger",))
        self.assertEqual(status, 0)
        self.assertEqual([(call[0], call[2], call[3]) for call in calls],
                         [("combined-timing", 120, None)])
        self.assertIn("Required isolated combined-timing lifecycle: PASS", output)
        self.assertNotIn("Independent diagnostics", output)
        debugger.assert_not_called()

    def test_full_pass_requires_actual_icon_lifecycle_and_drain_lines(self):
        for missing in ICON_LIFECYCLE_LINES:
            for replacement in ("", "echo: " + missing, "diagnostic-prefix " + missing):
                with self.subTest(missing=missing, replacement=replacement):
                    def response(suite, command, options):
                        if suite is not None:
                            return self.passed(suite, command)
                        return subprocess.CompletedProcess(command, 0, FULL_OUTPUT.replace(missing, replacement))
                    status, calls, output = self.invoke(response)
                    self.assertEqual(status, 1)
                    self.assertIn("Missing native icon lifecycle checkpoints", output)
                    self.assertEqual([call[0] for call in calls], [None, *SUITES])

    def test_isolated_fixture_pass_cannot_replace_missing_icon_retirement(self):
        for missing in ICON_LIFECYCLE_LINES:
            with self.subTest(missing=missing):
                def response(suite, command, options):
                    if suite is None:
                        return subprocess.CompletedProcess(command, 17, "original failure\n")
                    result = self.passed(suite, command)
                    if suite == "combined-timing":
                        result.stdout = result.stdout.replace(missing, "echo: " + missing)
                    return result
                status, calls, output = self.invoke(response)
                self.assertEqual(status, 1)
                self.assertIn("combined-timing=FAIL (status 1)", output)
                self.assertIn("Missing isolated diagnostic checkpoints", output)
                self.assertIn("combined-fidelity=PASS (status 0)", output)

    def test_required_timing_lifecycle_rejects_missing_or_only_echoed_checkpoints(self):
        incomplete = [TIMING_LIFECYCLE_OUTPUT.replace(line + "\n", "")
                      for line in TIMING_LIFECYCLE_LINES]
        incomplete += ["\n".join("echo: " + line for line in TIMING_LIFECYCLE_LINES), FULL_OUTPUT]
        for native_output in incomplete:
            with self.subTest(native_output=native_output):
                status, calls, output = self.invoke(
                    lambda suite, command, options: subprocess.CompletedProcess(command, 0, native_output),
                    timing_lifecycle=True)
                self.assertEqual(status, 1)
                self.assertEqual(len(calls), 1)
                self.assertIn("Missing required isolated timing lifecycle checkpoints", output)
                self.assertNotIn("Required isolated combined-timing lifecycle: PASS", output)

    def test_required_timing_lifecycle_nonzero_after_pass_stays_failed(self):
        for native_status in (17, -11):
            with self.subTest(native_status=native_status), \
                    mock.patch.object(runner.sys, "platform", "darwin"), \
                    mock.patch.object(runner, "macos_crash_diagnostics",
                        return_value=TIMING_LIFECYCLE_OUTPUT + "LLDB stack only\n") as debugger:
                status, calls, output = self.invoke(
                    lambda suite, command, options: subprocess.CompletedProcess(
                        command, native_status, TIMING_LIFECYCLE_OUTPUT),
                    timing_lifecycle=True, extra_arguments=("--macos-crash-debugger",))
                self.assertEqual(status, 1)
                self.assertEqual(len(calls), 1)
                self.assertIn(f"Native application exited with status {native_status}", output)
                self.assertNotIn("Required isolated combined-timing lifecycle: PASS", output)
                if native_status < 0:
                    debugger.assert_called_once_with(self.executable.resolve(), self.source.resolve(),
                        runtime_state=mock.ANY, native_suite="combined-timing")
                    self.assertIn("LLDB stack only", self.report.with_name(
                        self.report.stem + "-debugger.txt").read_text(encoding="utf-8"))
                else:
                    debugger.assert_not_called()

    def test_required_timing_lifecycle_timeout_keeps_pass_output_but_fails(self):
        def response(suite, command, options):
            raise subprocess.TimeoutExpired(command, options["timeout"],
                                             output=TIMING_LIFECYCLE_OUTPUT.encode("utf-8"))
        with mock.patch.object(runner.sys, "platform", "darwin"), \
                mock.patch.object(runner, "macos_crash_diagnostics") as debugger:
            status, calls, output = self.invoke(response, timing_lifecycle=True,
                                               extra_arguments=("--macos-crash-debugger",))
        self.assertEqual(status, 1)
        self.assertEqual(len(calls), 1)
        self.assertIn(TIMING_LIFECYCLE_OUTPUT, output)
        self.assertIn("timed out after 120 seconds", output)
        self.assertNotIn("Required isolated combined-timing lifecycle: PASS", output)
        debugger.assert_not_called()

    def test_required_timing_lifecycle_unsafe_crash_retains_fixture_without_debugger(self):
        def response(suite, command, options):
            options["runtime_state"]["safe_to_release"] = False
            return subprocess.CompletedProcess(command, -11, TIMING_LIFECYCLE_OUTPUT)
        with mock.patch.object(runner.sys, "platform", "darwin"), \
                mock.patch.object(runner, "macos_crash_diagnostics") as debugger:
            status, calls, output = self.invoke(response, timing_lifecycle=True, retained_count=1,
                                               extra_arguments=("--macos-crash-debugger",))
        self.assertEqual(status, 1)
        self.assertEqual(len(calls), 1)
        self.assertIn("status -11", output)
        self.assertIn("retained", output)
        self.assertNotIn("Required isolated combined-timing lifecycle: PASS", output)
        debugger.assert_not_called()

    def test_required_timing_lifecycle_cannot_substitute_for_full_or_benchmark_checks(self):
        for incompatible in ("--artist-benchmark", "--require-function-workflow",
                             "--require-offline-gl-workflow", "--require-vector-workflows",
                             "--require-combined-workflows"):
            arguments = ["native-runner", str(self.executable), str(self.source),
                         "--output", str(self.report), "--isolated-timing-lifecycle", incompatible]
            with self.subTest(incompatible=incompatible), mock.patch.object(sys, "argv", arguments), \
                    mock.patch.object(runner, "run_captured_process") as launch, \
                    mock.patch.object(sys, "stderr", io.StringIO()), self.assertRaises(SystemExit) as error:
                runner.main()
            self.assertEqual(error.exception.code, 2)
            launch.assert_not_called()

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
                                                     "combined-cast-vector", runtime_state=mock.ANY)
                self.assertEqual(status, 1)
                self.assertEqual([call[0] for call in calls], [None, *SUITES])
                self.assertIn("combined-cast-vector=FAIL (status -11)", output)
                self.assertIn("combined-fidelity=PASS", output)
                report = self.report.with_name(self.report.stem + "-combined-cast-vector-debugger.txt")
                self.assertIn("debugger unavailable" if debugger_fails else "diagnostic stack only",
                              report.read_text(encoding="utf-8"))

    def test_unsafe_original_crash_retains_fixture_and_prevents_all_reruns(self):
        def response(suite, command, options):
            options["runtime_state"].update(safe_to_release=False, output=b"original crash checkpoint\n")
            return subprocess.CompletedProcess(command, -11, b"original crash checkpoint\n")
        with mock.patch.object(runner.sys, "platform", "linux"), \
                mock.patch.object(runner, "linux_crash_diagnostics") as debugger:
            status, calls, output = self.invoke(response,
                extra_arguments=("--linux-crash-debugger",), retained_count=1)
        self.assertEqual(status, 1)
        self.assertEqual([call[0] for call in calls], [None])
        self.assertIn("original crash checkpoint", output)
        self.assertIn("status -11", output)
        self.assertIn("retained", output)
        debugger.assert_not_called()

    def test_unsafe_isolated_failure_prevents_remaining_suites_and_final_debugger(self):
        def response(suite, command, options):
            if suite is None:
                return subprocess.CompletedProcess(command, -11, b"original crash\n")
            options["runtime_state"].update(safe_to_release=False, output=b"isolated failure checkpoint\n")
            return subprocess.CompletedProcess(command, 23, b"isolated failure checkpoint\n")
        with mock.patch.object(runner.sys, "platform", "linux"), \
                mock.patch.object(runner, "linux_crash_diagnostics") as debugger:
            status, calls, output = self.invoke(response,
                extra_arguments=("--linux-crash-debugger",), retained_count=1)
        self.assertEqual(status, 1)
        self.assertEqual([call[0] for call in calls], [None, "function-workflow"])
        self.assertIn("original crash", output)
        self.assertIn("isolated failure checkpoint", output)
        self.assertIn("retained", output)
        self.assertNotIn("cast-workflow=PASS", output)
        debugger.assert_not_called()

    def test_unsafe_selected_debugger_prevents_remaining_suites_and_final_debugger(self):
        def response(suite, command, options):
            return subprocess.CompletedProcess(command, -11,
                b"original crash\n" if suite is None else b"isolated crash\n")
        def debug(*args, **kwargs):
            kwargs["runtime_state"]["safe_to_release"] = False
            return "isolated debugger checkpoint; fixture retained\n"
        with mock.patch.object(runner.sys, "platform", "linux"), \
                mock.patch.object(runner, "linux_crash_diagnostics", side_effect=debug) as debugger:
            status, calls, output = self.invoke(response, extra_arguments=("--linux-crash-debugger",))
        self.assertEqual(status, 1)
        self.assertEqual([call[0] for call in calls], [None, "function-workflow"])
        debugger.assert_called_once_with(self.executable.resolve(), self.source.resolve(),
                                         "function-workflow", runtime_state=mock.ANY)
        self.assertFalse(debugger.call_args.kwargs["runtime_state"]["safe_to_release"])
        self.assertIn("isolated debugger checkpoint", output)
        self.assertIn("function-workflow=FAIL (status -11)", output)
        self.assertNotIn("cast-workflow=PASS", output)

    def test_all_debuggers_launch_the_exact_selected_suite(self):
        kits = self.root / "sdk"
        cdb = kits / "Windows Kits" / "10" / "Debuggers" / "x64" / "cdb.exe"
        cdb.parent.mkdir(parents=True)
        cdb.write_bytes(b"mocked debugger; never executed\n")
        result = subprocess.CompletedProcess(["mock-debugger"], 0,
                                             b"bounded diagnostic stack\nnative-lldb-target-cleanup: CONFIRMED\n")
        inherited = {"ProgramFiles(x86)": str(kits),
                     "GROUNDZERO_NATIVE_UI_SUITE": "untrusted-inherited-selector",
                     "GROUNDZERO_ARTIST_BENCHMARK": "1"}
        for selected in (None, *SUITES):
            for debugger_kind in ("linux", "windows", "macos"):
                with self.subTest(selected=selected, debugger=debugger_kind), \
                     mock.patch.dict(os.environ, inherited), \
                     mock.patch.object(runner.shutil, "which", return_value="mock-gdb"), \
                     mock.patch.object(runner, "run_captured_process", return_value=result) as launch:
                    if debugger_kind == "linux":
                        runner.linux_crash_diagnostics(self.executable, self.source, selected)
                    elif debugger_kind == "windows":
                        runner.windows_crash_diagnostics(self.executable, self.source, 0xc0000005,
                                                         (), native_suite=selected)
                    else:
                        runner.macos_crash_diagnostics(self.executable, self.source, selected)
                        command = launch.call_args.args[0]
                        self.assertIn("--batch", command)
                        self.assertIn("--no-lldbinit", command)
                        self.assertIn("--one-line-on-crash", command)
                        self.assertIn("thread backtrace all -c 40", command)
                        self.assertEqual(command[-3:], ["--", str(self.executable), "--test-native-ui"])
                    options = launch.call_args.kwargs
                    self.assertEqual(options["environment"].get("GROUNDZERO_NATIVE_UI_SUITE"), selected)
                    self.assertNotIn("GROUNDZERO_ARTIST_BENCHMARK", options["environment"])
                    self.assertEqual(options["timeout"], 120)
                    self.assertFalse(options["cwd"].exists())

    def test_macos_shutdown_crash_after_pass_markers_stays_failed(self):
        for unsafe_debugger in (False, True):
            with self.subTest(unsafe_debugger=unsafe_debugger):
                def response(suite, command, options):
                    if suite is None:
                        return subprocess.CompletedProcess(command, 17, "original failure\n")
                    result = self.passed(suite, command)
                    if suite == "combined-timing":
                        result.returncode = -11
                        result.stdout += "native application cleanup completed\n"
                    return result
                def debug(*args, **kwargs):
                    if unsafe_debugger:
                        kwargs["runtime_state"]["safe_to_release"] = False
                    return "LLDB stack; original crash remains failure\n"
                with mock.patch.object(runner.sys, "platform", "darwin"), \
                     mock.patch.object(runner, "macos_crash_diagnostics", side_effect=debug) as debugger:
                    status, calls, output = self.invoke(response,
                        extra_arguments=("--macos-crash-debugger",))
                self.assertEqual(status, 1)
                self.assertIn("combined-timing=FAIL (status -11)", output)
                expected = [None, *SUITES[:3]] if unsafe_debugger else [None, *SUITES]
                self.assertEqual([call[0] for call in calls], expected)
                debugger.assert_called_once_with(self.executable.resolve(), self.source.resolve(),
                                                 "combined-timing", runtime_state=mock.ANY)
                report = self.report.with_name(self.report.stem + "-combined-timing-debugger.txt")
                self.assertIn("LLDB stack", report.read_text(encoding="utf-8"))

    def test_macos_original_signal_gets_separate_report_without_changing_failure(self):
        def response(suite, command, options):
            return (subprocess.CompletedProcess(command, -11, "original shutdown crash\n")
                    if suite is None else self.passed(suite, command))
        with mock.patch.object(runner.sys, "platform", "darwin"), \
             mock.patch.object(runner, "macos_crash_diagnostics", return_value="LLDB original stack\n") as debugger:
            status, calls, output = self.invoke(response,
                extra_arguments=("--macos-crash-debugger",))
        self.assertEqual(status, 1)
        self.assertEqual([call[0] for call in calls], [None, *SUITES])
        self.assertIn("status -11", output)
        debugger.assert_called_once_with(self.executable.resolve(), self.source.resolve(), runtime_state=mock.ANY)
        self.assertIn("LLDB original stack", self.report.with_name(
            self.report.stem + "-debugger.txt").read_text(encoding="utf-8"))

    def test_macos_unsafe_original_and_benchmark_do_not_launch_debugger(self):
        for benchmark in (False, True):
            with self.subTest(benchmark=benchmark):
                def response(suite, command, options):
                    if not benchmark:
                        options["runtime_state"]["safe_to_release"] = False
                    return subprocess.CompletedProcess(command, -11, "original crash\n")
                with mock.patch.object(runner.sys, "platform", "darwin"), \
                     mock.patch.object(runner, "macos_crash_diagnostics") as debugger:
                    status, calls, _ = self.invoke(response, benchmark=benchmark,
                        extra_arguments=("--macos-crash-debugger",), retained_count=0 if benchmark else 1)
                self.assertEqual(status, 1)
                self.assertEqual([call[0] for call in calls], [None])
                debugger.assert_not_called()

    def test_macos_unavailable_debugger_is_reported_without_launch(self):
        with mock.patch.object(runner.shutil, "which", return_value=None), \
             mock.patch.object(runner, "run_captured_process") as launch:
            report = runner.macos_crash_diagnostics(self.executable, self.source, "combined-timing")
        launch.assert_not_called()
        self.assertIn("unavailable", report)
        self.assertIn("no debugger process was launched", report)
        self.assertIn("original native test failure remains authoritative", report)

    def test_macos_cleanup_requires_emitted_line_and_bounds_report(self):
        marker = b"native-lldb-target-cleanup: CONFIRMED"
        for result_kind in ("confirmed", "echo-only", "timeout", "launch-error"):
            with self.subTest(result_kind=result_kind):
                state = {"safe_to_release": True}
                directories = []
                payload = b"beginning\n" + b"x" * (300 * 1024) + b"\nstack tail\n"
                payload += marker + b"\n" if result_kind == "confirmed" else b"(lldb) script print('" + marker + b"')\n"
                def launch(command, **options):
                    directories.append(options["cwd"])
                    self.assertEqual(options["timeout"], 120)
                    self.assertEqual(options["environment"]["GROUNDZERO_NATIVE_UI_SUITE"], "combined-timing")
                    state["output"] = payload
                    if result_kind == "timeout":
                        raise subprocess.TimeoutExpired(command, 120, output=payload)
                    if result_kind == "launch-error":
                        raise OSError("LLDB launch denied")
                    return subprocess.CompletedProcess(command, 0, payload)
                with mock.patch.object(runner.shutil, "which", return_value="mock-lldb"), \
                     mock.patch.object(runner, "run_captured_process", side_effect=launch):
                    report = runner.macos_crash_diagnostics(self.executable, self.source,
                                                          "combined-timing", runtime_state=state)
                self.assertEqual(state["safe_to_release"], result_kind == "confirmed")
                self.assertIn("beginning", report)
                self.assertIn("stack tail", report)
                self.assertIn("output truncated", report)
                self.assertLess(len(report.encode("utf-8")), 256 * 1024 + 2048)
                self.assertEqual(len(directories), 1)
                if result_kind == "confirmed":
                    self.assertFalse(directories[0].exists())
                else:
                    self.assertTrue(directories[0].exists())
                    self.addCleanup(runner.shutil.rmtree, directories[0])
                    self.assertIn("target exit was not confirmed", report)
                if result_kind == "timeout":
                    self.assertIn("timed out after 120 seconds", report)
                if result_kind == "launch-error":
                    self.assertIn("OSError: LLDB launch denied", report)

    def test_unknown_diagnostic_selector_is_rejected(self):
        environment = {}
        with self.assertRaises(ValueError):
            runner.select_diagnostic_suite(environment, "unknown-fixture")
        self.assertNotIn("GROUNDZERO_NATIVE_UI_SUITE", environment)


if __name__ == "__main__":
    unittest.main()
