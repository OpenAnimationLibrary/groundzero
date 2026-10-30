"""CI-only contracts for original-process cores; no native binary is executed."""
from contextlib import nullcontext, redirect_stdout
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import native_ui_core as cores
import run_native_ui_test as runner


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux core contract")
class OriginalCoreContracts(unittest.TestCase):
    def setUp(self):
        self.storage = tempfile.TemporaryDirectory()
        self.addCleanup(self.storage.cleanup)
        self.root = Path(self.storage.name)
        self.directory = self.root / "cores"
        self.directory.mkdir(mode=0o700)
        self.executable = self.root / "OpenToonz"
        self.executable.write_bytes(b"\x7fELFmock binary; never executed")
        self.executable.chmod(0o700)
        self.core = self.directory / "core.42"
        self.core.write_bytes(b"mock original core")

    def test_exact_pid_and_executable_postmortem_never_runs_binary(self):
        other = self.directory / "core.99"
        other.write_bytes(b"another process")
        with mock.patch.object(cores, "capture", side_effect=[
                (f"ELF core, execfn: '{self.executable}'", True),
                ("Program terminated with signal SIGSEGV.\n#0 actual_crash\n", True)]) as capture:
            output = cores.collect_original_core(self.executable, self.directory, 42)
        command = capture.call_args_list[1].args[0]
        self.assertIn("--core=" + str(self.core), command)
        self.assertIn("--se=" + str(self.executable), command)
        self.assertIn("set auto-load off", command)
        self.assertIn("set debuginfod enabled off", command)
        self.assertFalse(any(value in ("run", "attach", "--args") for value in command))
        self.assertIn("actual_crash", output)
        self.assertIn("original native process result remains authoritative", output)
        self.assertFalse(self.core.exists())
        self.assertEqual(other.read_bytes(), b"another process")

    def test_missing_or_mismatched_core_is_unavailable_not_a_pass(self):
        with mock.patch.object(cores, "capture") as capture:
            output = cores.collect_original_core(self.executable, self.directory, 404)
            capture.assert_not_called()
        self.assertIn("diagnostics unavailable", output)
        self.assertTrue(self.core.exists())
        with mock.patch.object(cores, "capture", return_value=("execfn: '/bin/sh'", True)) as capture:
            output = cores.collect_original_core(self.executable, self.directory, 42)
        self.assertEqual(capture.call_count, 1)
        self.assertIn("exact original executable", output)
        self.assertFalse(self.core.exists())
        self.assertNotIn("PASS", output)

    def test_rejected_symlink_does_not_read_or_remove_target(self):
        self.core.unlink()
        target = self.root / "retained"
        target.write_bytes(b"untouched")
        self.core.symlink_to(target)
        with mock.patch.object(cores, "capture") as capture:
            output = cores.collect_original_core(self.executable, self.directory, 42)
        capture.assert_not_called()
        self.assertIn("not an owned regular file", output)
        self.assertEqual(target.read_bytes(), b"untouched")
        self.assertFalse(self.core.is_symlink())

    def test_collector_failure_still_removes_original_core(self):
        with mock.patch.object(cores, "capture", side_effect=[
                (f"execfn: '{self.executable}'", True), OSError("debugger unavailable")]):
            output = cores.collect_original_core(self.executable, self.directory, 42)
        self.assertIn("debugger unavailable", output)
        self.assertFalse(self.core.exists())

    def test_command_timeout_retains_partial_text_and_bounds_deadline(self):
        def timeout(command, **options):
            self.assertEqual(options["timeout"], 6)
            options["stdout"].write(b"partial original stack\n")
            raise subprocess.TimeoutExpired(command, 6)
        with mock.patch.object(cores.time, "monotonic", return_value=100), \
                mock.patch.object(cores.subprocess, "run", side_effect=timeout):
            output, success = cores.capture(["gdb"], 106, 45)
        self.assertFalse(success)
        self.assertIn("partial original stack", output)
        self.assertIn("time limit", output)

    def test_output_is_bounded_without_unbounded_pipe_capture(self):
        def verbose(command, **options):
            self.assertNotEqual(options["stdout"], subprocess.PIPE)
            options["stdout"].write(b"x" * (cores.MAX_TEXT_BYTES + 5000))
            return subprocess.CompletedProcess(command, 0)
        with mock.patch.object(cores.subprocess, "run", side_effect=verbose):
            output, success = cores.capture(["gdb"], cores.time.monotonic() + 60, 45)
        self.assertTrue(success)
        self.assertIn("truncated", output)
        self.assertLess(len(output.encode()), cores.MAX_TEXT_BYTES + 200)

    def test_primary_core_limit_restored_even_on_launch_failure(self):
        import resource
        previous = (0, resource.RLIM_INFINITY)
        with mock.patch.object(Path, "read_text", return_value=str(self.directory / "core.%p")), \
                mock.patch.object(resource, "getrlimit", return_value=previous), \
                mock.patch.object(resource, "setrlimit") as setting:
            with self.assertRaisesRegex(OSError, "launch failed"):
                with cores.original_core_limit(self.directory):
                    raise OSError("launch failed")
        self.assertEqual(setting.call_args_list, [
            mock.call(resource.RLIMIT_CORE, (cores.CORE_BYTES, resource.RLIM_INFINITY)),
            mock.call(resource.RLIMIT_CORE, previous)])

    def test_pattern_mismatch_cannot_launch_or_change_limits(self):
        import resource
        with mock.patch.object(Path, "read_text", return_value="|other-handler"), \
                mock.patch.object(resource, "setrlimit") as setting:
            with self.assertRaisesRegex(ValueError, "Kernel core pattern"):
                with cores.original_core_limit(self.directory):
                    self.fail("unsafe capture configuration was admitted")
        setting.assert_not_called()

    def test_recorded_pid_belongs_to_direct_application_popen(self):
        command = [str(self.executable), "--test-native-ui"]
        process = mock.Mock(pid=42)
        process.wait.return_value = 0
        state = {"safe_to_release": True}
        with mock.patch.object(runner.subprocess, "Popen", return_value=process) as popen:
            result = runner.run_captured_process(command, cwd=self.root,
                environment={"QT_QPA_PLATFORM": "xcb"}, timeout=600, runtime_state=state)
        self.assertEqual(popen.call_args.args[0], command)
        self.assertEqual(state["pid"], 42)
        self.assertEqual(result.returncode, 0)
        self.assertTrue(state["safe_to_release"])

    def test_runner_retains_crash_and_collects_before_clear_without_replay(self):
        source = self.root / "stuff"
        source.mkdir()
        report = self.root / "ui-integration-output.txt"
        order = []
        clear = "\n".join(("icon-lifecycle-native: terminal reviewed-clear PASS",
            "icon-lifecycle-native: PASS", "native-ui-integration: checkpoint: native icon workers drained",
            "native-ui-integration: checkpoint: native application cleanup completed")) + "\n"
        def launch(command, **options):
            if command[-1] == "--test-native-ui":
                order.append("primary")
                options["runtime_state"]["pid"] = 42
                return subprocess.CompletedProcess(command, -11, b"original final checkpoint\n")
            order.append("clear")
            return subprocess.CompletedProcess(command, 0, clear.encode())
        def collect(binary, directory, pid):
            self.assertEqual((binary, directory, pid), (self.executable, self.directory, 42))
            order.append("original-core")
            return "Original stack unavailable; original failure remains authoritative.\n"
        args = ["runner", str(self.executable), str(source), "--output", str(report),
                "--linux-crash-debugger", "--linux-original-core-directory", str(self.directory)]
        with mock.patch.object(sys, "argv", args), redirect_stdout(io.StringIO()), \
                mock.patch.object(runner, "run_captured_process", side_effect=launch), \
                mock.patch.object(cores, "original_core_limit", return_value=nullcontext()), \
                mock.patch.object(cores, "collect_original_core", side_effect=collect), \
                mock.patch.object(runner, "linux_crash_diagnostics") as replay:
            status = runner.main()
        self.assertEqual(status, 1)
        self.assertEqual(order, ["primary", "original-core", "clear"])
        replay.assert_not_called()
        self.assertIn("exited with status -11", report.read_text())
        self.assertIn("Required reviewed-clear lifecycle: PASS (status 0)", report.read_text())
        self.assertIn("Original stack unavailable", report.with_name(
            "ui-integration-output-original-core.txt").read_text())

    def test_workflow_restores_kernel_pattern_and_never_retains_raw_core(self):
        # Execute the actual workflow shell body with fake sysctl/sudo/launcher
        # commands. This changes no host kernel setting and runs no application.
        workflow = Path(__file__).resolve().parents[2] / ".github/workflows/workflow_linux.yml"
        section = workflow.read_text().split("    - name: Test native application integration\n", 1)[1]
        section = section.split("\n    - name:", 1)[0]
        body = textwrap.dedent(section.split("      run: |\n", 1)[1])
        tools = self.root / "commands"
        tools.mkdir()
        build = self.root / "toonz/build"
        build.mkdir(parents=True)
        def script(name, text):
            path = tools / name
            path.write_text("#!" + sys.executable + "\n" + text)
            path.chmod(0o700)
        script("sudo", "import os,sys\nos.execvp(sys.argv[2],sys.argv[2:])\n")
        script("sysctl", "import os,sys,pathlib\n"
            "p=pathlib.Path(os.environ['MOCK_PATTERN'])\n"
            "if sys.argv[1]=='-n': print(p.read_text())\n"
            "else:\n"
            " value=sys.argv[2].split('=',1)[1]\n"
            " if value=='|original handler' and os.environ['MOCK_RESTORE_FAIL']=='1': sys.exit(9)\n"
            " p.write_text(value)\n")
        script("xvfb-run", "import os,sys,pathlib\n"
            "p=pathlib.Path(sys.argv[sys.argv.index('--linux-original-core-directory')+1])\n"
            "(p/'core.42').write_bytes(b'raw core must never persist')\n"
            "sys.exit(int(os.environ['MOCK_NATIVE_STATUS']))\n")
        pattern = self.root / "kernel-pattern"
        for native_status, restore_fail in ((0, False), (23, False), (0, True), (23, True)):
            with self.subTest(native_status=native_status, restore_fail=restore_fail):
                pattern.write_text("|original handler")
                environment = dict(os.environ, GITHUB_WORKSPACE=str(self.root),
                    PATH=str(tools) + os.pathsep + os.environ["PATH"], MOCK_PATTERN=str(pattern),
                    MOCK_NATIVE_STATUS=str(native_status), MOCK_RESTORE_FAIL=str(int(restore_fail)))
                result = subprocess.run(["bash", "-e", "-c", body], cwd=self.root,
                    env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    timeout=20, check=False)
                self.assertEqual(result.returncode, native_status or int(restore_fail), result.stdout)
                self.assertEqual(list(build.glob("native-ui-cores.*")), [])
                if not restore_fail:
                    self.assertEqual(pattern.read_text(), "|original handler")
                else:
                    self.assertIn("restoration failed", (build / "ui-integration-output-original-core.txt").read_text())


if __name__ == "__main__":
    unittest.main()
