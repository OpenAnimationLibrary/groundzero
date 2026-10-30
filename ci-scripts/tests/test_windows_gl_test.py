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
            with patch.object(native, "os", SimpleNamespace(name="nt")):
                with self.assertRaisesRegex(RuntimeError, "retained"):
                    with native.windows_mesa_renderer(executable, runtime,
                                                      cleanup_allowed=lambda: False):
                        pass
            for name in ("opengl32.dll", "libgallium_wgl.dll", "renderer.exe.local"):
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
            with patch.object(native, "os", SimpleNamespace(name="nt")):
                with native.windows_mesa_renderer(executable, runtime,
                                                  cleanup_allowed=lambda: True):
                    self.assertTrue((root / "opengl32.dll").is_file())
            for name in ("opengl32.dll", "libgallium_wgl.dll", "renderer.exe.local"):
                self.assertFalse((root / name).exists(), name)


class ShellThumbnailOutputTest(unittest.TestCase):
    def invoke(self, process, cleanup=None):
        script = (Path(__file__).resolve().parents[2] / "toonz" / "sources" /
                  "toonzpreview" / "tests" / "run_thumbnail_test.py")
        raw = io.BytesIO()
        console = io.TextIOWrapper(raw, encoding="cp1252", errors="strict")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            @contextmanager
            def fixture(*args):
                yield root, {}

            arguments = [str(script)] + [str(root / name) for name in
                         ("fixture.exe", "converter.exe", "provider.dll", "stuff")]
            with patch.object(native, "isolated_fixture", fixture), \
                    patch.object(subprocess, "Popen", return_value=process), \
                    patch.object(subprocess, "run", cleanup or Mock()), \
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
        return outcome, emitted

    def test_raw_unicode_and_invalid_bytes_do_not_mask_child_status(self):
        output = "Shell fixture: 描画 🎨\n".encode("utf-8") + b"raw diagnostic: \xff\n"
        for status in (0, 23):
            with self.subTest(status=status):
                process = Mock(returncode=status)
                process.communicate.return_value = (output, None)
                outcome, emitted = self.invoke(process)
                self.assertEqual(emitted, output)
                if status:
                    self.assertIsInstance(outcome, SystemExit)
                    self.assertEqual(outcome.code, status)
                else:
                    self.assertIsNone(outcome)
                process.communicate.assert_called_once_with(timeout=180)

    def test_timeout_keeps_cleanup_and_child_diagnostics_without_encoding_error(self):
        output = "Timed-out renderer: 描画 🎨".encode("utf-8")
        cleanup_output = "Terminated process: 描画\n".encode("utf-8")
        process = Mock(pid=123, returncode=9)
        process.communicate.side_effect = [subprocess.TimeoutExpired("fixture", 180), (output, None)]
        process.poll.return_value = 9
        cleanup = Mock(return_value=SimpleNamespace(stdout=cleanup_output, returncode=0))
        outcome, emitted = self.invoke(process, cleanup)
        self.assertIsInstance(outcome, RuntimeError)
        self.assertIn("deadline", str(outcome))
        self.assertEqual(emitted, cleanup_output + output + b"\n")
        cleanup.assert_called_once_with(["taskkill", "/PID", "123", "/T", "/F"],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        timeout=15, check=False)
        self.assertEqual(process.communicate.call_args_list[-1].kwargs, {"timeout": 10})


if __name__ == "__main__":
    unittest.main()
