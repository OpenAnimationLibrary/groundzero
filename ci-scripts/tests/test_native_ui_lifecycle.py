"""CI-only contracts for two independent native terminal shutdown processes."""
from contextlib import redirect_stdout
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "run_native_ui_test.py"
sys.path.insert(0, str(SCRIPT.parent))
SPEC = importlib.util.spec_from_file_location("function_native_ui_runner", SCRIPT)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

LIFECYCLE = "icon-lifecycle-native: PASS (actual workers and Qt TLS retired)"
DRAINED = "native-ui-integration: checkpoint: native icon workers drained"
CLEANUP = "native-ui-integration: checkpoint: native application cleanup completed"
REPLACEMENT = "icon-lifecycle-native: terminal replacement PASS"
REVIEWED_CLEAR = "icon-lifecycle-native: terminal reviewed-clear PASS"
FUNCTION = "function-workflow-native: PASS"
FULL_OUTPUT = "\n".join(("native-ui-integration: PASS", FUNCTION, REPLACEMENT,
                          LIFECYCLE, DRAINED, CLEANUP)) + "\n"
CLEAR_OUTPUT = "\n".join((REVIEWED_CLEAR, LIFECYCLE, DRAINED, CLEANUP)) + "\n"


class NativeLifecycleContracts(unittest.TestCase):
    def invoke(self, output=FULL_OUTPUT, status=0, *, timeout=False, debugger=False,
               require_function=True, clear_output=CLEAR_OUTPUT, clear_status=0,
               clear_timeout=False, clear_error=None, unsafe=False, clear_unsafe=False,
               clear_report_error=False):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "OpenToonz"
            executable.write_bytes(b"mock executable; never launched")
            stuff = root / "stuff"
            stuff.mkdir()
            original_data = b"unchanged fixture source\n"
            (stuff / "source.txt").write_bytes(original_data)
            report = root / "native-output.txt"
            clear_report = root / "native-output-icon-clear.txt"
            calls = []
            make_directory = tempfile.mkdtemp
            write_text = Path.write_text

            def local_directory(*args, **kwargs):
                # Exercise the real copied-profile isolation, while retaining
                # intentionally unsafe fixture data inside this test's owner.
                kwargs["dir"] = root
                return make_directory(*args, **kwargs)

            def write_report(path, *args, **kwargs):
                if clear_report_error and path == clear_report:
                    raise OSError("reviewed-clear report cannot be written")
                return write_text(path, *args, **kwargs)

            def launch(command, **options):
                second = bool(calls)
                expected_mode = "--test-native-icon-clear" if second else "--test-native-ui"
                self.assertEqual(command, [str(executable.resolve()), expected_mode])
                self.assertLess(len(calls), 2, "reviewed-clear must run exactly once")
                self.assertEqual(set(options), {"cwd", "environment", "timeout", "runtime_state"})
                self.assertEqual(options["timeout"], 120 if second else 180)
                directory = options["cwd"]
                environment = options["environment"]
                state = options["runtime_state"]
                self.assertTrue(state["safe_to_release"])
                self.assertTrue((directory / ".groundzero-native-ui-test").is_file())
                self.assertEqual((directory / "portablestuff" / "source.txt").read_bytes(), original_data)
                self.assertNotIn("GROUNDZERO_NATIVE_UI_SUITE", environment)
                self.assertNotIn("GROUNDZERO_ARTIST_BENCHMARK", environment)
                if second:
                    first = calls[0]
                    self.assertNotEqual(directory, first.cwd)
                    self.assertFalse(first.cwd.exists(), "first profile must be released before the next launch")
                    self.assertIsNot(environment, first.environment)
                    self.assertIsNot(state, first.state)
                    self.assertNotIn("FIRST_PROCESS_ONLY", environment)
                    self.assertFalse((directory / "portablestuff" / "changed-by-first.txt").exists())
                else:
                    environment["FIRST_PROCESS_ONLY"] = "isolated mutation"
                    (directory / "portablestuff" / "changed-by-first.txt").write_bytes(b"authored state")
                calls.append(SimpleNamespace(mode=expected_mode, cwd=directory,
                    environment=environment, state=state, timeout=options["timeout"]))
                child_output = clear_output if second else output
                child_status = clear_status if second else status
                state["output"] = child_output.encode("utf-8")
                if (clear_unsafe if second else unsafe):
                    state["safe_to_release"] = False
                if second and clear_error is not None:
                    raise clear_error
                if (clear_timeout if second else timeout):
                    raise subprocess.TimeoutExpired(command, options["timeout"],
                                                    output=state["output"])
                return subprocess.CompletedProcess(command, child_status, state["output"])

            arguments = [str(SCRIPT), str(executable), str(stuff), "--output", str(report)]
            if require_function:
                arguments.append("--require-function-workflow")
            if debugger:
                arguments.append("--linux-crash-debugger")
            captured = io.StringIO()
            with mock.patch.object(runner.sys, "argv", arguments), \
                    mock.patch.object(runner.sys, "platform", "linux"), \
                    mock.patch.dict(runner.os.environ, {
                        "GROUNDZERO_NATIVE_UI_SUITE": "must-not-leak",
                        "GROUNDZERO_ARTIST_BENCHMARK": "1"}), \
                    mock.patch.object(runner.tempfile, "mkdtemp", side_effect=local_directory), \
                    mock.patch.object(Path, "write_text", write_report), \
                    mock.patch.object(runner, "run_captured_process", side_effect=launch), \
                    mock.patch.object(runner, "linux_crash_diagnostics", return_value=FULL_OUTPUT) as diagnostic, \
                    redirect_stdout(captured):
                result = runner.main()
            self.assertEqual(len(calls), 1 if unsafe else 2)
            self.assertEqual((stuff / "source.txt").read_bytes(), original_data)
            self.assertEqual(sorted(path.name for path in stuff.iterdir()), ["source.txt"])
            self.assertEqual(calls[0].cwd.exists(), unsafe)
            if len(calls) == 2:
                self.assertEqual(calls[1].cwd.exists(), clear_unsafe)
            clear_text = clear_report.read_text(encoding="utf-8") if clear_report.exists() else None
            main_text = report.read_text(encoding="utf-8")
            if clear_text is not None:
                self.assertIn(clear_text, main_text, "separate evidence must also remain in the main report")
            return SimpleNamespace(status=result, report=main_text, displayed=captured.getvalue(),
                                   diagnostics=diagnostic.call_count, clear_report=clear_text, calls=calls)

    def test_both_terminal_processes_are_required_and_fresh(self):
        for marker in (LIFECYCLE, "icon-lifecycle-native: PASS"):
            with self.subTest(marker=marker):
                output = FULL_OUTPUT.replace(LIFECYCLE, marker)
                result = self.invoke(output, clear_output=CLEAR_OUTPUT.replace(LIFECYCLE, marker))
                self.assertEqual(result.status, 0)
                self.assertTrue(result.report.startswith(output))
                self.assertEqual(result.displayed, result.report)
                self.assertEqual(result.diagnostics, 0)
                self.assertEqual([call.mode for call in result.calls],
                                 ["--test-native-ui", "--test-native-icon-clear"])
                self.assertIn(REVIEWED_CLEAR, result.clear_report)
                self.assertNotIn("native-ui-integration: PASS", result.clear_report)
                self.assertIn("Required reviewed-clear lifecycle: PASS (status 0)", result.report)

    def test_original_zero_exit_requires_each_actual_lifecycle_line(self):
        for marker in (REPLACEMENT, LIFECYCLE, DRAINED, CLEANUP):
            for replacement in ("", "echo: " + marker, "debugger-command " + marker):
                with self.subTest(marker=marker, replacement=replacement):
                    output = FULL_OUTPUT.replace(marker, replacement)
                    result = self.invoke(output)
                    self.assertEqual(result.status, 1)
                    self.assertTrue(result.report.startswith(output))
                    self.assertIn("Missing native icon lifecycle checkpoints", result.report)
                    self.assertIn("Required reviewed-clear lifecycle: PASS", result.report)
                    self.assertEqual(result.diagnostics, 0)

    def test_reviewed_clear_zero_exit_requires_each_own_actual_lifecycle_line(self):
        for marker in (REVIEWED_CLEAR, LIFECYCLE, DRAINED, CLEANUP):
            for replacement in ("", "echo: " + marker, "debugger-command " + marker):
                with self.subTest(marker=marker, replacement=replacement):
                    result = self.invoke(clear_output=CLEAR_OUTPUT.replace(marker, replacement))
                    self.assertEqual(result.status, 1)
                    self.assertTrue(result.report.startswith(FULL_OUTPUT))
                    self.assertIn("Missing required reviewed-clear lifecycle checkpoints", result.clear_report)
                    self.assertIn("Required reviewed-clear lifecycle: FAIL", result.report)

    def test_terminal_markers_cannot_substitute_for_the_other_process(self):
        for original, reviewed in ((FULL_OUTPUT.replace(REPLACEMENT, REVIEWED_CLEAR), CLEAR_OUTPUT),
                                   (FULL_OUTPUT, CLEAR_OUTPUT.replace(REVIEWED_CLEAR, REPLACEMENT))):
            with self.subTest(original=original, reviewed=reviewed):
                self.assertEqual(self.invoke(original, clear_output=reviewed).status, 1)

    def test_lifecycle_requirement_is_independent_of_function_flag(self):
        result = self.invoke("native-ui-integration: PASS\n", require_function=False)
        self.assertEqual(result.status, 1)
        self.assertIn("Missing native icon lifecycle checkpoints", result.report)

    def test_clear_output_cannot_satisfy_missing_original_function_checkpoint(self):
        result = self.invoke(FULL_OUTPUT.replace(FUNCTION, ""),
                             clear_output=CLEAR_OUTPUT + FUNCTION + "\n")
        self.assertEqual(result.status, 1)
        self.assertIn("required native Function Editor workflow fixture did not run", result.report)

    def test_nonzero_original_after_pass_markers_survives_clear_success(self):
        for child_status in (17, -11, 0xC0000005):
            with self.subTest(child_status=child_status):
                result = self.invoke(status=child_status)
                self.assertEqual(result.status, 1)
                self.assertTrue(result.report.startswith(FULL_OUTPUT))
                self.assertIn(f"Native application exited with status {child_status}", result.report)
                self.assertIn("Required reviewed-clear lifecycle: PASS", result.report)
                self.assertNotIn("Missing native icon lifecycle checkpoints", result.report)
                self.assertEqual(result.diagnostics, 0)

    def test_nonzero_clear_after_all_pass_markers_fails_overall(self):
        for child_status in (17, -11, 0xC0000005):
            with self.subTest(child_status=child_status):
                result = self.invoke(clear_status=child_status)
                self.assertEqual(result.status, 1)
                self.assertTrue(result.report.startswith(FULL_OUTPUT))
                self.assertIn(CLEAR_OUTPUT, result.clear_report)
                self.assertIn(f"FAIL (status {child_status})", result.clear_report)
                self.assertIn(f"Reviewed-clear native process exited with status {child_status}",
                              result.clear_report)

    def test_original_timeout_after_pass_markers_survives_clear_success(self):
        result = self.invoke(timeout=True, debugger=True)
        self.assertEqual(result.status, 1)
        self.assertTrue(result.report.startswith(FULL_OUTPUT))
        self.assertIn("timed out after 180 seconds", result.report)
        self.assertIn("Required reviewed-clear lifecycle: PASS", result.report)
        self.assertEqual(result.diagnostics, 0)

    def test_clear_timeout_and_launch_failure_keep_evidence_and_fail_overall(self):
        for failure in ("timeout", "launch"):
            with self.subTest(failure=failure):
                result = self.invoke(clear_timeout=failure == "timeout",
                    clear_error=OSError("cannot launch reviewed-clear") if failure == "launch" else None)
                self.assertEqual(result.status, 1)
                self.assertTrue(result.report.startswith(FULL_OUTPUT))
                self.assertIn(CLEAR_OUTPUT, result.clear_report)
                self.assertIn("timed out after 120 seconds" if failure == "timeout" else
                              "cannot launch reviewed-clear", result.clear_report)

    def test_unsafe_original_prevents_any_second_process_or_debugger(self):
        result = self.invoke(status=-11, unsafe=True, debugger=True)
        self.assertEqual(result.status, 1)
        self.assertEqual(len(result.calls), 1)
        self.assertIsNone(result.clear_report)
        self.assertIn("Native application exited with status -11", result.report)
        self.assertIn("process-tree exit was not confirmed", result.report)
        self.assertEqual(result.diagnostics, 0)

    def test_unsafe_clear_retains_its_fixture_and_prevents_original_debugger(self):
        result = self.invoke(status=-11, clear_status=17, clear_unsafe=True, debugger=True)
        self.assertEqual(result.status, 1)
        self.assertIn("Native application exited with status -11", result.report)
        self.assertIn(CLEAR_OUTPUT, result.clear_report)
        self.assertIn("process-tree cleanup could not confirm", result.clear_report)
        self.assertIn("Reviewed-clear native process exited with status 17", result.clear_report)
        self.assertEqual(result.diagnostics, 0)

    def test_clear_report_write_failure_remains_failure(self):
        result = self.invoke(clear_report_error=True)
        self.assertEqual(result.status, 1)
        self.assertIsNone(result.clear_report)
        self.assertIn("Could not save required reviewed-clear lifecycle report", result.report)

    def test_successful_debugger_cannot_replace_original_crash(self):
        result = self.invoke(status=-11, debugger=True)
        self.assertEqual(result.status, 1)
        self.assertIn("Native application exited with status -11", result.report)
        self.assertEqual(result.diagnostics, 1)
        self.assertEqual(result.displayed.count(LIFECYCLE), 3)
        self.assertEqual(result.report.count(LIFECYCLE), 2)


if __name__ == "__main__":
    unittest.main()
