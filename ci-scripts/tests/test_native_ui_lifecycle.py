"""Required native thumbnail-lifecycle gates for the Function Editor runner."""
from contextlib import contextmanager, redirect_stdout
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "run_native_ui_test.py"
SPEC = importlib.util.spec_from_file_location("function_native_ui_runner", SCRIPT)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

LIFECYCLE = "icon-lifecycle-native: PASS (actual workers and Qt TLS retired)"
DRAINED = "native-ui-integration: checkpoint: native icon workers drained"
FUNCTION = "function-workflow-native: PASS"
FULL_OUTPUT = "\n".join(("native-ui-integration: PASS", FUNCTION, LIFECYCLE, DRAINED)) + "\n"


class NativeLifecycleContracts(unittest.TestCase):
    def invoke(self, output=FULL_OUTPUT, status=0, *, timeout=False, debugger=False,
               require_function=True):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "OpenToonz"
            executable.write_bytes(b"mock executable; never launched")
            stuff = root / "stuff"
            stuff.mkdir()
            fixture = root / "fixture"
            fixture.mkdir()
            report = root / "native-output.txt"
            environment = {"NATIVE_LIFECYCLE_FIXTURE": "isolated"}

            captured_boundary = hasattr(runner, "run_captured_process")
            fixture_state = None

            @contextmanager
            def isolate(actual_source, actual_executable, mesa_runtime=None,
                        runtime_state=None):
                nonlocal fixture_state
                self.assertEqual(actual_source, stuff.resolve())
                self.assertEqual(actual_executable, executable.resolve())
                self.assertIsNone(mesa_runtime)
                if captured_boundary:
                    self.assertEqual(runtime_state, {"safe_to_release": True})
                else:
                    self.assertIsNone(runtime_state)
                fixture_state = runtime_state
                yield fixture, environment

            def launch(command, **options):
                self.assertEqual(command, [str(executable.resolve()), "--test-native-ui"])
                self.assertEqual(options["cwd"], fixture)
                self.assertEqual(options["timeout"], 180)
                if captured_boundary:
                    self.assertEqual(set(options), {"cwd", "environment", "timeout", "runtime_state"})
                    self.assertIs(options["environment"], environment)
                    self.assertIs(options["runtime_state"], fixture_state)
                    self.assertTrue(fixture_state["safe_to_release"])
                else:
                    self.assertEqual(options["env"], environment)
                    self.assertEqual(options["encoding"], "utf-8")
                    self.assertEqual(options["errors"], "replace")
                    self.assertEqual(options["stdout"], subprocess.PIPE)
                    self.assertEqual(options["stderr"], subprocess.STDOUT)
                    self.assertFalse(options.get("check", False))
                if timeout:
                    raise subprocess.TimeoutExpired(command, 180, output=output.encode("utf-8"))
                return subprocess.CompletedProcess(command, status,
                    output.encode("utf-8") if captured_boundary else output)

            arguments = [str(SCRIPT), str(executable), str(stuff), "--output", str(report)]
            if require_function:
                arguments.append("--require-function-workflow")
            if debugger:
                arguments.append("--linux-crash-debugger")
            captured = io.StringIO()
            # Later stack branches use the captured-process helper. Exercise
            # that actual boundary there, without calling a real subprocess or
            # importing their unrelated suite selectors into this runner.
            launch_owner = runner if captured_boundary else runner.subprocess
            launch_name = "run_captured_process" if captured_boundary else "run"
            with mock.patch.object(runner.sys, "argv", arguments), \
                    mock.patch.object(runner.sys, "platform", "linux"), \
                    mock.patch.object(runner, "isolated_fixture", isolate), \
                    mock.patch.object(launch_owner, launch_name, side_effect=launch) as child, \
                    mock.patch.object(runner, "linux_crash_diagnostics", return_value=FULL_OUTPUT) as diagnostic, \
                    redirect_stdout(captured):
                result = runner.main()
            child.assert_called_once()
            return result, report.read_text(encoding="utf-8"), captured.getvalue(), diagnostic.call_count

    def test_complete_native_lifecycle_pass_keeps_success_and_original_output(self):
        for marker in (LIFECYCLE, "icon-lifecycle-native: PASS"):
            with self.subTest(marker=marker):
                output = FULL_OUTPUT.replace(LIFECYCLE, marker)
                status, report, displayed, diagnostics = self.invoke(output)
                self.assertEqual(status, 0)
                self.assertEqual(report, output)
                self.assertEqual(displayed, output)
                self.assertEqual(diagnostics, 0)

    def test_zero_exit_requires_both_actual_lifecycle_lines(self):
        for marker in (LIFECYCLE, DRAINED):
            for replacement in ("", "echo: " + marker, "debugger-command " + marker):
                with self.subTest(marker=marker, replacement=replacement):
                    output = FULL_OUTPUT.replace(marker, replacement)
                    status, report, _, diagnostics = self.invoke(output)
                    self.assertEqual(status, 1)
                    self.assertTrue(report.startswith(output))
                    self.assertIn("Missing native icon lifecycle checkpoints", report)
                    self.assertEqual(diagnostics, 0)

    def test_lifecycle_requirement_is_independent_of_function_flag(self):
        status, report, _, _ = self.invoke("native-ui-integration: PASS\n", require_function=False)
        self.assertEqual(status, 1)
        self.assertIn("Missing native icon lifecycle checkpoints", report)

    def test_function_checkpoint_remains_required(self):
        status, report, _, _ = self.invoke(FULL_OUTPUT.replace(FUNCTION, ""))
        self.assertEqual(status, 1)
        self.assertIn("required native Function Editor workflow fixture did not run", report)

    def test_nonzero_after_all_pass_markers_remains_the_original_failure(self):
        for child_status in (17, -11, 0xC0000005):
            with self.subTest(child_status=child_status):
                status, report, _, diagnostics = self.invoke(status=child_status)
                self.assertEqual(status, 1)
                self.assertTrue(report.startswith(FULL_OUTPUT))
                self.assertIn(f"Native application exited with status {child_status}", report)
                self.assertNotIn("Missing native icon lifecycle checkpoints", report)
                self.assertEqual(diagnostics, 0)

    def test_timeout_after_pass_markers_is_still_failure(self):
        status, report, _, diagnostics = self.invoke(timeout=True, debugger=True)
        self.assertEqual(status, 1)
        self.assertTrue(report.startswith(FULL_OUTPUT))
        self.assertIn("timed out after 180 seconds", report)
        self.assertEqual(diagnostics, 0)

    def test_successful_diagnostic_cannot_replace_original_crash(self):
        status, report, displayed, diagnostics = self.invoke(status=-11, debugger=True)
        self.assertEqual(status, 1)
        self.assertIn("Native application exited with status -11", report)
        self.assertEqual(diagnostics, 1)
        self.assertEqual(displayed.count(LIFECYCLE), 2)
        self.assertEqual(report.count(LIFECYCLE), 1)


if __name__ == "__main__":
    unittest.main()
