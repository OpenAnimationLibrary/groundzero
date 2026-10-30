"""CI-only contracts for temporary Windows renderer setup and failure handling."""
from contextlib import contextmanager
import io
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_windows_gl_test as launcher
import run_native_ui_test as native


class CapturedProcessLifecycleTest(unittest.TestCase):
    def invoke(self, process, *, platform="nt", taskkill=None,
               group_error=None, group_survives=False, monotonic=None,
               launch_error=None, output=b"captured \xff\n"):
        state = {"safe_to_release": True}
        observed = {}
        native_os = Mock(wraps=os)
        native_os.name = platform
        def signal_group(pid, sig):
            if sig == 0 and not group_survives:
                raise ProcessLookupError("group is gone")
            if group_error is not None:
                raise group_error
        native_os.killpg = Mock(side_effect=signal_group)
        clock = Mock(side_effect=monotonic) if monotonic is not None else Mock(wraps=native.time.monotonic)

        def launch(*args, **kwargs):
            observed.update(kwargs)
            self.assertTrue(state["safe_to_release"], "launch failure must not retain unused files")
            if launch_error is not None:
                raise launch_error
            kwargs["stdout"].write(output)
            kwargs["stdout"].flush()
            return process

        with tempfile.TemporaryDirectory() as directory:
            with patch.object(native, "os", native_os), \
                    patch.object(native, "signal", SimpleNamespace(SIGKILL=9)), \
                    patch.object(native.subprocess, "Popen", side_effect=launch) as popen, \
                    patch.object(native.subprocess, "run", taskkill or Mock(
                        return_value=SimpleNamespace(returncode=0, stdout=b"tree stopped\n"))) as cleanup, \
                    patch.object(native.time, "monotonic", clock), \
                    patch.object(native.time, "sleep"):
                try:
                    result = native.run_captured_process(
                        ["native-fixture", "--test-native-ui"], cwd=Path(directory),
                        environment={"TEST_SENTINEL": "preserved"}, timeout=17,
                        runtime_state=state)
                except BaseException as error:
                    result = error
                capture = state.get("capture_path")
                if capture is not None:
                    observed["capture_exists_after"] = Path(capture).exists()
                    if observed["capture_exists_after"]:
                        observed["capture_bytes_after"] = Path(capture).read_bytes()
        return result, state, observed, popen, cleanup, native_os

    def test_success_and_native_error_preserve_raw_bytes_and_status(self):
        output = "native checkpoint: 描画 🎨\n".encode("utf-8") + b"invalid: \xff\n"
        for status in (0, 23, 0xC0000005):
            with self.subTest(status=status):
                process = Mock(pid=123, returncode=status)
                process.wait.return_value = status
                result, state, invocation, _, cleanup, _ = self.invoke(process, output=output)
                self.assertIsInstance(result, subprocess.CompletedProcess)
                self.assertEqual(result.stdout, output)
                self.assertEqual(result.returncode, status)
                self.assertTrue(state["safe_to_release"])
                self.assertFalse(invocation["capture_exists_after"])
                self.assertEqual(invocation["env"], {"TEST_SENTINEL": "preserved"})
                self.assertEqual(invocation["stderr"], subprocess.STDOUT)
                self.assertFalse(invocation.get("start_new_session", False))
                process.wait.assert_called_once_with(timeout=17)
                process.communicate.assert_not_called()
                cleanup.assert_not_called()

    def test_launch_error_does_not_mark_unstarted_process_unsafe(self):
        process = Mock()
        error = OSError("cannot launch native fixture")
        result, state, _, _, cleanup, _ = self.invoke(process, launch_error=error)
        self.assertIs(result, error)
        self.assertTrue(state["safe_to_release"])
        process.wait.assert_not_called()
        cleanup.assert_not_called()

    def test_windows_timeout_stops_tree_before_parent_with_bounded_cleanup(self):
        order = []
        process = Mock(pid=123, returncode=1)
        process.wait.side_effect = [subprocess.TimeoutExpired("fixture", 17), 1]
        process.poll.return_value = None
        process.kill.side_effect = lambda: order.append("parent")
        cleanup = Mock(side_effect=lambda *args, **kwargs: (
            order.append("tree") or SimpleNamespace(returncode=0, stdout=b"stopped\n")))
        output = b"native checkpoint before timeout\n\xff"
        result, state, _, _, _, _ = self.invoke(process, taskkill=cleanup, output=output)
        self.assertIsInstance(result, subprocess.TimeoutExpired)
        self.assertTrue(result.output.endswith(output))
        self.assertEqual(state["output"], result.output)
        self.assertEqual(result.timeout, 17)
        self.assertTrue(state["safe_to_release"])
        self.assertEqual(order, ["tree", "parent"])
        self.assertEqual(cleanup.call_args.args[0], ["taskkill", "/PID", "123", "/T", "/F"])
        self.assertFalse(cleanup.call_args.kwargs["check"])
        self.assertGreater(cleanup.call_args.kwargs["timeout"], 0)
        self.assertLessEqual(cleanup.call_args.kwargs["timeout"], 12)
        self.assertGreater(process.wait.call_args_list[-1].kwargs["timeout"], 0)
        self.assertLessEqual(process.wait.call_args_list[-1].kwargs["timeout"], 15)
        process.communicate.assert_not_called()

    def test_uncertain_tree_cleanup_remains_unsafe_even_when_parent_exited(self):
        failures = [SimpleNamespace(returncode=128, stdout=b"no process tree\n"),
                    OSError("taskkill unavailable"),
                    subprocess.TimeoutExpired("taskkill", 12)]
        for failure in failures:
            with self.subTest(failure=type(failure).__name__):
                process = Mock(pid=123, returncode=1)
                process.wait.side_effect = [subprocess.TimeoutExpired("fixture", 17), 1]
                process.poll.return_value = 1
                cleanup = Mock(return_value=failure,
                               side_effect=failure if isinstance(failure, Exception) else None)
                result, state, invocation, _, _, _ = self.invoke(process, taskkill=cleanup)
                self.assertIsInstance(result, subprocess.TimeoutExpired)
                self.assertTrue(result.output.endswith(b"captured \xff\n"))
                self.assertFalse(state["safe_to_release"])
                self.assertTrue(invocation["capture_exists_after"])
                self.assertEqual(invocation["capture_bytes_after"], b"captured \xff\n")

    def test_parent_reap_timeout_remains_unsafe_and_preserves_initial_deadline(self):
        process = Mock(pid=123, returncode=None)
        process.wait.side_effect = [subprocess.TimeoutExpired("fixture", 17),
                                    subprocess.TimeoutExpired("cleanup", 3)]
        process.poll.return_value = None
        result, state, _, _, _, _ = self.invoke(process)
        self.assertIsInstance(result, subprocess.TimeoutExpired)
        self.assertEqual(result.timeout, 17)
        self.assertTrue(result.output.endswith(b"captured \xff\n"))
        self.assertFalse(state["safe_to_release"])
        process.communicate.assert_not_called()

    def test_unexpected_wait_failure_still_attempts_tree_cleanup(self):
        process = Mock(pid=123, returncode=1)
        failure = OSError("native wait failed")
        process.wait.side_effect = [failure, 1]
        process.poll.return_value = None
        result, state, _, _, cleanup, _ = self.invoke(process)
        self.assertIs(result, failure)
        self.assertTrue(state["safe_to_release"])
        self.assertTrue(state["output"].endswith(b"captured \xff\n"))
        cleanup.assert_called_once()
        process.kill.assert_called_once_with()

    def test_posix_timeout_owns_and_stops_process_group(self):
        process = Mock(pid=123, returncode=-9)
        process.wait.side_effect = [subprocess.TimeoutExpired("fixture", 17), -9]
        process.poll.return_value = -9
        result, state, invocation, _, cleanup, native_os = self.invoke(process, platform="posix")
        self.assertIsInstance(result, subprocess.TimeoutExpired)
        self.assertEqual(result.output, b"captured \xff\n")
        self.assertTrue(state["safe_to_release"])
        self.assertTrue(invocation["start_new_session"])
        native_os.killpg.assert_any_call(123, 9)
        cleanup.assert_not_called()

    def test_posix_missing_group_and_denied_group_have_distinct_retention(self):
        for error, safe in ((ProcessLookupError("already exited"), True),
                            (PermissionError("cannot stop descendants"), False)):
            with self.subTest(error=type(error).__name__):
                process = Mock(pid=123, returncode=-9)
                process.wait.side_effect = [subprocess.TimeoutExpired("fixture", 17), -9]
                process.poll.return_value = -9
                result, state, _, _, _, _ = self.invoke(process, platform="posix", group_error=error)
                self.assertIsInstance(result, subprocess.TimeoutExpired)
                self.assertEqual(state["safe_to_release"], safe)

    def test_posix_signal_delivery_without_group_exit_retains_capture(self):
        process = Mock(pid=123, returncode=-9)
        process.wait.side_effect = [subprocess.TimeoutExpired("fixture", 17), -9]
        process.poll.return_value = -9
        result, state, invocation, _, _, native_os = self.invoke(
            process, platform="posix", group_survives=True, monotonic=[0, 0, 0, 15, 16])
        self.assertIsInstance(result, subprocess.TimeoutExpired)
        self.assertFalse(state["safe_to_release"])
        self.assertTrue(invocation["capture_exists_after"])
        native_os.killpg.assert_any_call(123, 9)
        native_os.killpg.assert_any_call(123, 0)
        self.assertTrue(result.output.endswith(b"captured \xff\n"))


class IsolatedFixtureLifetimeTest(unittest.TestCase):
    def test_safe_fixture_is_removed_and_original_data_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "stuff"
            source.mkdir()
            (source / "sentinel.txt").write_bytes(b"untouched original")
            executable = Path(directory) / "fixture.exe"
            executable.touch()
            state = {"safe_to_release": True}
            with native.isolated_fixture(source, executable, runtime_state=state) as (fixture, environment):
                self.assertEqual((fixture / "portablestuff" / "sentinel.txt").read_bytes(),
                                 b"untouched original")
                self.assertTrue((fixture / ".groundzero-native-ui-test").is_file())
                for key in ("TMPDIR", "TEMP", "TMP"):
                    self.assertEqual(environment[key], str(fixture.parent))
                (fixture / "portablestuff" / "sentinel.txt").write_bytes(b"disposable edit")
            self.assertFalse(fixture.exists())
            self.assertEqual((source / "sentinel.txt").read_bytes(), b"untouched original")

    def test_unsafe_fixture_survives_context_exit_with_or_without_mesa(self):
        for use_mesa in (False, True):
            with self.subTest(mesa=use_mesa), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source = root / "stuff"
                source.mkdir()
                (source / "sentinel.txt").write_bytes(b"fixture source")
                executable = root / "fixture.exe"
                executable.touch()
                runtime = root / "runtime"
                runtime.mkdir()
                for name in ("opengl32.dll", "libgallium_wgl.dll"):
                    (runtime / name).write_bytes(b"test-only-placeholder")
                native_os = Mock(wraps=os)
                native_os.name = "nt" if use_mesa else os.name
                state = {"safe_to_release": True}
                fixture = None
                try:
                    with patch.object(native, "os", native_os):
                        with self.assertRaisesRegex(RuntimeError, "retained"):
                            with native.isolated_fixture(source, executable,
                                    runtime if use_mesa else None, runtime_state=state) as (fixture, _):
                                state["safe_to_release"] = False
                    self.assertIsNotNone(fixture)
                    self.assertTrue(fixture.is_dir(), "unsafe root was removed on context exit")
                    self.assertEqual((fixture / "portablestuff" / "sentinel.txt").read_bytes(),
                                     b"fixture source")
                    if use_mesa:
                        for name in ("opengl32.dll", "libgallium_wgl.dll", "fixture.exe.local"):
                            self.assertTrue((root / name).is_file(), name)
                finally:
                    # Mocked processes hold no files; remove intentionally retained
                    # test-only data after proving the production cleanup boundary.
                    if fixture is not None and fixture.exists():
                        native.shutil.rmtree(fixture)


class WindowsGlTest(unittest.TestCase):
    def invoke(self, process, *, taskkill=None, launch_error=None, events=None):
        events = [] if events is None else events

        @contextmanager
        def renderer(executable, runtime, cleanup_allowed):
            self.assertEqual(executable.name, "renderer.exe")
            self.assertEqual(runtime, Path("verified-runtime"))
            events.append("installed")
            try:
                yield {"QT_OPENGL": "desktop", "QT_OPENGL_DLL": "opengl32.dll"}
            finally:
                if not cleanup_allowed():
                    events.append("retained")
                    raise RuntimeError("Renderer descendants may still be running")
                events.append("released")

        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "renderer.exe"
            executable.touch()
            with patch.object(launcher, "windows_mesa_renderer", renderer), \
                    patch.object(launcher.subprocess, "Popen", return_value=process,
                                 side_effect=launch_error) as popen, \
                    patch.object(launcher.subprocess, "run", taskkill or Mock(return_value=Mock(returncode=0))):
                try:
                    result = launcher.main([
                        "--runtime", "verified-runtime", "--renderer-executable", str(executable),
                        "--timeout", "17", "--", "ctest", "--output-on-failure"])
                except Exception as error:
                    result = error
        return result, popen, events

    def test_command_status_and_required_renderer_environment_are_preserved(self):
        process = Mock()
        process.wait.return_value = 19
        with patch.dict(os.environ, {"QT_OPENGL": "angle", "TEST_SENTINEL": "kept"}):
            result, popen, events = self.invoke(process)
        self.assertEqual(result, 19)
        self.assertEqual(popen.call_args.args, (["ctest", "--output-on-failure"],))
        environment = popen.call_args.kwargs["env"]
        self.assertEqual(environment["QT_OPENGL"], "desktop")
        self.assertEqual(environment["QT_OPENGL_DLL"], "opengl32.dll")
        self.assertEqual(environment["GROUNDZERO_REQUIRE_VECTOR_GL"], "1")
        self.assertEqual(environment["GROUNDZERO_OFFSCREEN_GL_DIAGNOSTICS"], "1")
        self.assertEqual(environment["TEST_SENTINEL"], "kept")
        process.wait.assert_called_once_with(timeout=17)
        self.assertEqual(events, ["installed", "released"])

    def test_timeout_terminates_tree_then_parent_before_releasing_runtime(self):
        process = Mock(pid=123)
        process.wait.side_effect = [subprocess.TimeoutExpired("ctest", 17), 1]
        process.poll.return_value = None
        order = []
        process.kill.side_effect = lambda: order.append("kill-parent")
        taskkill = Mock(side_effect=lambda *args, **kwargs: (order.append("kill-tree") or Mock(returncode=0)))
        result, _, events = self.invoke(process, taskkill=taskkill, events=order)
        self.assertIsInstance(result, RuntimeError)
        self.assertIn("deadline", str(result))
        taskkill.assert_called_once_with(["taskkill", "/PID", "123", "/T", "/F"],
                                        check=False, timeout=20)
        self.assertEqual(order, ["installed", "kill-tree", "kill-parent", "released"])
        self.assertEqual(process.wait.call_args_list[-1].kwargs, {"timeout": 10})
        self.assertIs(events, order)

    def test_failed_tree_cleanup_still_kills_parent_and_remains_failure(self):
        process = Mock(pid=123)
        process.wait.side_effect = [subprocess.TimeoutExpired("ctest", 17), 1]
        process.poll.return_value = None
        result, _, events = self.invoke(process, taskkill=Mock(side_effect=OSError("unavailable")))
        self.assertIsInstance(result, RuntimeError)
        self.assertIn("descendants", str(result))
        process.kill.assert_called_once_with()
        self.assertEqual(events, ["installed", "retained"])

    def test_launch_error_releases_runtime_and_does_not_report_success(self):
        process = Mock()
        result, _, events = self.invoke(process, launch_error=OSError("process unavailable"))
        self.assertIsInstance(result, OSError)
        process.wait.assert_not_called()
        self.assertEqual(events, ["installed", "released"])

    def test_unexpected_wait_error_retains_runtime_for_running_process(self):
        process = Mock()
        process.wait.side_effect = OSError("wait unavailable")
        result, _, events = self.invoke(process)
        self.assertIsInstance(result, RuntimeError)
        self.assertIn("descendants", str(result))
        self.assertEqual(events, ["installed", "retained"])

    def test_real_runtime_helper_retains_files_when_cleanup_is_unsafe(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runtime = root / "runtime"
            runtime.mkdir()
            for name in ("opengl32.dll", "libgallium_wgl.dll"):
                (runtime / name).write_bytes(b"test-only-placeholder")
            executable = root / "renderer.exe"
            executable.touch()
            (root / "tconverter.exe").touch()
            with patch.object(native, "os", SimpleNamespace(name="nt")):
                with self.assertRaisesRegex(RuntimeError, "retained"):
                    with native.windows_mesa_renderer(executable, runtime,
                                                      cleanup_allowed=lambda: False):
                        pass
            for name in ("opengl32.dll", "libgallium_wgl.dll", "renderer.exe.local", "tconverter.exe.local"):
                self.assertTrue((root / name).is_file(), name)

    def test_real_runtime_helper_cleans_files_after_confirmed_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runtime = root / "runtime"
            runtime.mkdir()
            for name in ("opengl32.dll", "libgallium_wgl.dll"):
                (runtime / name).write_bytes(b"test-only-placeholder")
            executable = root / "renderer.exe"
            executable.touch()
            (root / "tconverter.exe").touch()
            with patch.object(native, "os", SimpleNamespace(name="nt")):
                with native.windows_mesa_renderer(executable, runtime,
                                                  cleanup_allowed=lambda: True):
                    self.assertTrue((root / "opengl32.dll").is_file())
                    self.assertTrue((root / "tconverter.exe.local").is_file())
            for name in ("opengl32.dll", "libgallium_wgl.dll", "renderer.exe.local", "tconverter.exe.local"):
                self.assertFalse((root / name).exists(), name)



class NativeOffscreenCheckpointTest(unittest.TestCase):
    def invoke(self, result, *, offline=True, function=False, vector=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "OpenToonz.exe"
            executable.touch()
            source = root / "stuff"
            source.mkdir()
            report = root / "output.txt"

            @contextmanager
            def fixture(*args, **kwargs):
                yield root, {}

            arguments = ["run_native_ui_test.py", str(executable), str(source), "--output", str(report)]
            if offline:
                arguments.append("--require-offline-gl-workflow")
            if function:
                arguments.append("--require-function-workflow")
            if vector:
                arguments.append("--require-vector-workflows")
            with patch.object(native, "isolated_fixture", fixture), \
                    patch.object(native, "run_captured_process", return_value=result,
                                 side_effect=result if isinstance(result, Exception) else None) as run, \
                    patch.object(sys, "argv", arguments), patch.object(sys, "stdout", io.StringIO()):
                status = native.main()
            return status, report.read_text(encoding="utf-8"), run.call_args

    def test_success_requires_real_offscreen_completion_marker(self):
        status, output, invocation = self.invoke(SimpleNamespace(
            returncode=0, stdout=b"native-ui-integration: PASS\n"))
        self.assertNotEqual(status, 0)
        self.assertIn("required native offscreen", output)
        self.assertEqual(invocation.kwargs["timeout"], 240)
        status, output, _ = self.invoke(SimpleNamespace(
            returncode=0, stdout=b"offline-gl-workflow: PASS\nnative-ui-integration: PASS\n"))
        self.assertEqual(status, 0)

    def test_failure_with_marker_and_timeout_remain_failures(self):
        status, output, _ = self.invoke(SimpleNamespace(
            returncode=23, stdout=b"offline-gl-workflow: PASS\n"))
        self.assertNotEqual(status, 0)
        self.assertIn("status 23", output)
        status, output, _ = self.invoke(subprocess.TimeoutExpired(
            "OpenToonz", 240, output=b"offline-gl-workflow: converter started\n"))
        self.assertNotEqual(status, 0)
        self.assertIn("timed out after 240 seconds", output)
        self.assertIn("converter started", output)

    def test_vector_checkpoint_keeps_full_deadline_and_required_markers(self):
        markers = [b"native-ui-integration: PASS", b"vector-workflows: PASS",
                   b"offline-gl-workflow: PASS"]
        for missing in markers:
            with self.subTest(missing=missing):
                status, output, invocation = self.invoke(SimpleNamespace(returncode=0,
                    stdout=b"\n".join(marker for marker in markers if marker != missing)), vector=True)
                self.assertNotEqual(status, 0)
                self.assertIn("Missing required native acceptance checkpoints", output)
                self.assertEqual(invocation.kwargs["timeout"], 600)
        status, _, invocation = self.invoke(SimpleNamespace(returncode=0,
            stdout=b"\n".join(markers)), vector=True)
        self.assertEqual(status, 0)
        self.assertEqual(invocation.kwargs["timeout"], 600)

    def test_function_checkpoint_and_original_deadline_remain_required(self):
        status, output, invocation = self.invoke(SimpleNamespace(
            returncode=0, stdout=b"native-ui-integration: PASS\n"), offline=False, function=True)
        self.assertNotEqual(status, 0)
        self.assertIn("required native Function Editor", output)
        self.assertEqual(invocation.kwargs["timeout"], 180)
        status, _, _ = self.invoke(SimpleNamespace(returncode=0,
            stdout=b"function-workflow-native: PASS\noffline-gl-workflow: PASS\n"), function=True)
        self.assertEqual(status, 0)


class DebuggerCaptureLifecycleTest(unittest.TestCase):
    def test_debugger_timeout_keeps_raw_checkpoint_and_original_failure_authority(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            @contextmanager
            def fixture(*args, **kwargs):
                yield root, {"QT_OPENGL": "desktop"}

            checkpoint = "debugger native checkpoint: 描画\n".encode("utf-8")
            with patch.object(native, "isolated_fixture", fixture), \
                    patch.object(native, "run_captured_process", side_effect=subprocess.TimeoutExpired(
                        "debugger", 120, output=checkpoint)) as run:
                report = native.capture_debugger(["debugger", "fixture"], root / "fixture.exe", root)
            self.assertIn(checkpoint.decode("utf-8"), report)
            self.assertIn("timed out after 120 seconds", report)
            self.assertIn("original native test failure remains authoritative", report)
            self.assertEqual(run.call_args.kwargs["timeout"], 120)
            self.assertEqual(run.call_args.kwargs["environment"]["QT_OPENGL"], "desktop")

    def test_unsafe_debugger_cleanup_preserves_captured_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            @contextmanager
            def fixture(*args, **kwargs):
                try:
                    yield root, {}
                finally:
                    if not kwargs["runtime_state"]["safe_to_release"]:
                        raise RuntimeError("fixture retained")

            def launch(*args, **kwargs):
                kwargs["runtime_state"]["safe_to_release"] = False
                kwargs["runtime_state"]["output"] = b"last debugger checkpoint \xff\n"
                raise OSError("debugger wait failed")

            with patch.object(native, "isolated_fixture", fixture), \
                    patch.object(native, "run_captured_process", side_effect=launch):
                report = native.capture_debugger(["debugger", "fixture"], root / "fixture.exe", root)
            self.assertIn("last debugger checkpoint \ufffd", report)
            self.assertIn("fixture retained", report)
            self.assertIn("original native test failure remains authoritative", report)


class ShellThumbnailOutputTest(unittest.TestCase):
    def invoke(self, result):
        script = (Path(__file__).resolve().parents[2] / "toonz" / "sources" /
                  "toonzpreview" / "tests" / "run_thumbnail_test.py")
        raw = io.BytesIO()
        console = io.TextIOWrapper(raw, encoding="cp1252", errors="strict")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            @contextmanager
            def fixture(*args, **kwargs):
                yield root, {}

            arguments = [str(script)] + [str(root / name) for name in
                         ("fixture.exe", "converter.exe", "provider.dll", "stuff")]
            with patch.object(native, "isolated_fixture", fixture), \
                    patch.object(native, "run_captured_process", return_value=result,
                                 side_effect=result if isinstance(result, Exception) else None) as run, \
                    patch.object(sys, "argv", arguments), patch.object(sys, "path", list(sys.path)), \
                    patch.object(sys, "stdout", console):
                try:
                    runpy.run_path(str(script), run_name="__main__")
                    outcome = None
                except (Exception, SystemExit) as error:
                    outcome = error
        console.flush()
        emitted = raw.getvalue()
        console.detach()
        return outcome, emitted, run.call_args

    def test_raw_unicode_and_invalid_bytes_do_not_mask_child_status(self):
        output = "Shell fixture: 描画 🎨\n".encode("utf-8") + b"raw diagnostic: \xff\n"
        for status in (0, 23):
            with self.subTest(status=status):
                outcome, emitted, invocation = self.invoke(SimpleNamespace(returncode=status, stdout=output))
                self.assertEqual(emitted, output)
                if status:
                    self.assertIsInstance(outcome, SystemExit)
                    self.assertEqual(outcome.code, status)
                else:
                    self.assertIsNone(outcome)
                self.assertEqual(invocation.kwargs["timeout"], 180)
                self.assertEqual(invocation.kwargs["environment"]["GROUNDZERO_OFFSCREEN_GL_DIAGNOSTICS"], "1")

    def test_timeout_keeps_child_diagnostics_without_encoding_error(self):
        output = "Timed-out renderer: 描画 🎨".encode("utf-8")
        outcome, emitted, invocation = self.invoke(subprocess.TimeoutExpired("fixture", 180, output=output))
        self.assertIsInstance(outcome, RuntimeError)
        self.assertIn("deadline", str(outcome))
        self.assertEqual(emitted, output + b"\n")
        self.assertEqual(invocation.kwargs["timeout"], 180)


if __name__ == "__main__":
    unittest.main()
