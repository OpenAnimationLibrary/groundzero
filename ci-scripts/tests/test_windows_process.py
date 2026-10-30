"""CI contracts for tracked Windows descendants and original crash capture."""
import ctypes
from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import windows_process as process_support
import run_native_ui_test as runner
from native_ui_windows_dump import OriginalWindowsDump


class JobRetirementContracts(unittest.TestCase):
    def process(self, status=0xC0000409, active=(1, 0)):
        events = []
        api = Mock()
        api.launch.return_value = (100, 200, 300)
        api.wait.side_effect = lambda handle, timeout: events.append(("wait", handle)) or status
        counts = iter(active)
        api.active.side_effect = lambda handle: events.append(("active", handle)) or next(counts)
        api.terminate.side_effect = lambda handle: events.append(("terminate", handle))
        api.close.side_effect = lambda handle: events.append(("close", handle))
        process = process_support.WindowsJobProcess(["fixture.exe"], cwd="fixture", env={},
                                                   stdout=io.BytesIO(), _api=api)
        return process, api, events

    def test_dead_root_preserves_uint32_status_and_retires_live_descendant(self):
        process, api, events = self.process()
        self.assertEqual(process.wait(17), 0xC0000409)
        safe, notes = process.retire()
        self.assertTrue(safe)
        self.assertIn(b"zero active processes", notes)
        self.assertEqual(process.returncode, 0xC0000409)
        self.assertEqual(events, [("wait", 200), ("active", 100), ("terminate", 100),
                                  ("close", 200), ("active", 100)])
        process.close(); process.close()
        self.assertEqual(events[-1], ("close", 100))
        self.assertEqual(api.close.call_count, 2)

    def test_query_or_termination_failure_never_proves_cleanup(self):
        for operation in ("active", "terminate"):
            with self.subTest(operation=operation):
                process, api, _ = self.process()
                getattr(api, operation).side_effect = OSError("injected job failure")
                safe, notes = process.retire()
                self.assertFalse(safe)
                self.assertIn(b"could not confirm", notes)
                process.close()

    def test_timeout_cannot_become_safe_before_accounting_reaches_zero(self):
        process, api, _ = self.process(status=1)
        api.active.side_effect = None
        api.active.return_value = 1
        with patch.object(process_support.time, "monotonic", side_effect=[0, 1, 16]):
            safe, notes = process.retire(15)
        self.assertFalse(safe)
        self.assertIn(b"did not reach zero", notes)
        self.assertEqual(process.returncode, 1)
        process.close()

    def test_wait_timeout_is_not_a_root_exit_code(self):
        process, api, _ = self.process()
        api.wait.side_effect = None
        api.wait.return_value = None
        with self.assertRaises(subprocess.TimeoutExpired):
            process.wait(3)
        self.assertIsNone(process.returncode)
        process.close()

    def test_windows_launch_failure_has_no_untracked_fallback(self):
        native_os = Mock(wraps=os); native_os.name = "nt"
        with patch.object(runner, "os", native_os), \
                patch.object(runner, "WindowsJobProcess", side_effect=OSError("job setup failed")), \
                patch.object(runner.subprocess, "Popen") as fallback:
            with self.assertRaisesRegex(OSError, "job setup failed"):
                runner.launch_captured_process(["fixture.exe"], cwd="fixture", env={},
                    stdout=io.BytesIO(), stderr=subprocess.STDOUT, start_new_session=False)
        fallback.assert_not_called()

    def test_capture_checks_zero_status_descendants_and_releases_handles(self):
        process, api, _ = self.process(status=0, active=(0, 0))
        raw = "actual capture: 動画\n".encode()
        with tempfile.TemporaryDirectory() as directory:
            state = {"safe_to_release": True}
            def launch(command, **options):
                options["stdout"].write(raw)
                return process
            with patch.object(runner, "launch_captured_process", side_effect=launch):
                result = runner.run_captured_process(["fixture.exe"], cwd=Path(directory),
                    environment={}, timeout=7, runtime_state=state)
            self.assertEqual(result.returncode, 0)
            self.assertTrue(result.stdout.endswith(raw))
            self.assertTrue(state["safe_to_release"])
            self.assertLessEqual(state["started_at"], state["finished_at"])
            self.assertFalse(Path(state["capture_path"]).exists())
            self.assertEqual(api.close.call_count, 2)


class _RegistryKey:
    def __enter__(self):
        return self
    def __exit__(self, *unused):
        return False


class FakeRegistry:
    HKEY_LOCAL_MACHINE = 1
    KEY_QUERY_VALUE, KEY_SET_VALUE, KEY_WOW64_64KEY = 1, 2, 256
    REG_EXPAND_SZ, REG_DWORD = 2, 4
    def __init__(self, values=None, exists=True):
        self.values = dict(values or {})
        self.exists, self.deleted, self.subkeys = exists, False, 0
        self.fail_value = None
    def OpenKey(self, *args):
        if not self.exists:
            raise FileNotFoundError()
        return _RegistryKey()
    def CreateKeyEx(self, *args):
        self.exists = True
        return _RegistryKey()
    def QueryValueEx(self, key, name):
        if name not in self.values:
            raise FileNotFoundError()
        return self.values[name]
    def SetValueEx(self, key, name, reserved, kind, value):
        if self.fail_value == name:
            self.fail_value = None
            raise OSError("injected partial configuration failure")
        self.values[name] = value, kind
    def DeleteValue(self, key, name):
        if name not in self.values:
            raise FileNotFoundError()
        del self.values[name]
    def CloseKey(self, key):
        pass
    def QueryInfoKey(self, key):
        return self.subkeys, len(self.values), 0
    def DeleteKeyEx(self, *args):
        self.deleted = True
        self.exists = False


class OriginalDumpContracts(unittest.TestCase):
    def setUp(self):
        self.storage = tempfile.TemporaryDirectory()
        self.addCleanup(self.storage.cleanup)
        self.root = Path(self.storage.name)
        self.executable = self.root / "Fixture.exe"
        self.executable.write_bytes(b"synthetic executable identity")

    def test_existing_values_types_and_unrelated_data_survive_exception(self):
        old = {"DumpFolder": ("old folder", 1), "DumpType": (2, 4), "Unrelated": (b"keep", 3)}
        registry = FakeRegistry(old)
        with self.assertRaisesRegex(RuntimeError, "original failure"):
            with OriginalWindowsDump(self.executable, self.root, registry=registry) as capture:
                self.assertTrue(capture.enabled)
                self.assertEqual(registry.values["DumpType"], (1, 4))
                raise RuntimeError("original failure")
        self.assertEqual(registry.values, old)
        self.assertFalse(registry.deleted)
        self.assertFalse(capture.folder.exists())

    def test_partial_configuration_restores_all_earlier_mutations(self):
        old = {"DumpFolder": ("unchanged", 2), "DumpCount": (9, 4)}
        registry = FakeRegistry(old); registry.fail_value = "DumpType"
        with OriginalWindowsDump(self.executable, self.root, registry=registry) as capture:
            self.assertFalse(capture.enabled)
        self.assertEqual(registry.values, old)
        self.assertIn("unavailable", "\n".join(capture.notes))

    def test_interrupted_setup_restores_values_even_without_context_entry(self):
        old = {"DumpFolder": ("unchanged", 2), "DumpCount": (9, 4)}
        registry = FakeRegistry(old)
        original_set = registry.SetValueEx
        interrupted = False
        def set_value(key, name, reserved, kind, value):
            nonlocal interrupted
            if name == "DumpType" and not interrupted:
                interrupted = True
                raise KeyboardInterrupt()
            original_set(key, name, reserved, kind, value)
        registry.SetValueEx = set_value
        capture = OriginalWindowsDump(self.executable, self.root, registry=registry)
        with self.assertRaises(KeyboardInterrupt):
            with capture:
                self.fail("interrupted setup must not enter the launch body")
        self.assertEqual(registry.values, old)
        self.assertFalse(capture.folder.exists())

    def test_new_key_removed_only_if_no_unrelated_data_appeared(self):
        for concurrent in (False, True):
            registry = FakeRegistry(exists=False)
            with OriginalWindowsDump(self.executable, self.root, registry=registry):
                if concurrent:
                    registry.values["Unrelated"] = ("keep", 1)
            self.assertEqual(registry.deleted, not concurrent)
            self.assertEqual(registry.values, {"Unrelated": ("keep", 1)} if concurrent else {})

    def test_uncertain_timeout_retains_dump_but_restores_registry(self):
        registry = FakeRegistry({"DumpType": (2, 4)})
        state = {"safe_to_release": True}
        with self.assertRaises(subprocess.TimeoutExpired):
            with OriginalWindowsDump(self.executable, self.root, registry=registry,
                                     runtime_state=state) as capture:
                state["safe_to_release"] = False
                raise subprocess.TimeoutExpired("original", 60)
        self.assertEqual(registry.values, {"DumpType": (2, 4)})
        self.assertTrue(capture.folder.is_dir())
        self.assertIn("retained", "\n".join(capture.notes))

    def test_postmortem_requires_exact_original_pid_and_does_not_replay(self):
        debugger = self.root / "Windows Kits/10/Debuggers/x64/cdb.exe"
        debugger.parent.mkdir(parents=True); debugger.touch()
        (self.root / "Fixture.pdb").write_bytes(b"local symbols")
        registry = FakeRegistry()
        calls = []
        def analyze(command, **options):
            calls.append((command, options))
            self.assertIn("-z", command)
            self.assertNotIn(str(self.executable), command)
            self.assertNotIn("--test-native-ui", command)
            self.assertEqual(options["timeout"], 120)
            self.assertNotIn("_NT_SYMBOL_PATH", options["environment"])
            self.assertNotIn("_NT_ALT_SYMBOL_PATH", options["environment"])
            return subprocess.CompletedProcess(command, 0, b"original exception context\n")
        with patch.dict(os.environ, {"ProgramFiles(x86)": str(self.root),
                                     "_NT_SYMBOL_PATH": "srv*network", "_NT_ALT_SYMBOL_PATH": "remote"}):
            with OriginalWindowsDump(self.executable, self.root, registry=registry) as capture:
                now = time.time()
                (capture.folder / "Fixture.exe.999.dmp").write_bytes(b"MDMP" + b"x" * 60)
                missing = capture.collect(123, now - 1, now + 1, [self.root], analyze)
                self.assertIn("unavailable", missing)
                self.assertFalse(calls)
                (capture.folder / "Fixture.exe.123.dmp").write_bytes(b"MDMP" + b"x" * 60)
                result = capture.collect(123, now - 1, now + 1, [self.root], analyze)
                self.assertIn("Executable SHA256:", result)
                self.assertIn("PDB Fixture.pdb SHA256:", result)
                self.assertIn("original exception context", result)
                self.assertEqual(len(calls), 1)
            self.assertFalse(capture.folder.exists())

    def test_interrupted_uncertain_debugger_retains_dump_and_restores_registry(self):
        debugger = self.root / "Windows Kits/10/Debuggers/x64/cdb.exe"
        debugger.parent.mkdir(parents=True); debugger.touch()
        old = {"DumpType": (2, 4)}
        registry = FakeRegistry(old)
        state = {"safe_to_release": True}
        def interrupted(command, **options):
            options["runtime_state"]["safe_to_release"] = False
            raise KeyboardInterrupt()
        with patch.dict(os.environ, {"ProgramFiles(x86)": str(self.root)}):
            with self.assertRaises(KeyboardInterrupt):
                with OriginalWindowsDump(self.executable, self.root, registry=registry,
                                         runtime_state=state) as capture:
                    now = time.time()
                    (capture.folder / "Fixture.exe.123.dmp").write_bytes(b"MDMP" + b"x" * 60)
                    capture.collect(123, now - 1, now + 1, [], interrupted)
        self.assertFalse(state["safe_to_release"])
        self.assertFalse(capture.safe_to_release)
        self.assertTrue(capture.folder.exists())
        self.assertEqual(registry.values, old)
        self.assertIn("retained", "\n".join(capture.notes))

    def test_runner_original_dump_keeps_crash_authority_and_suppresses_primary_replay(self):
        source = self.root / "stuff"; source.mkdir()
        report = self.root / "ui-integration-output.txt"
        native_os = Mock(wraps=os); native_os.name = "nt"
        collector = Mock(notes=[], restore_failed=False, safe_to_release=True)
        collector.__enter__ = Mock(return_value=collector)
        collector.__exit__ = Mock(return_value=False)
        collector.collect.return_value = "exact original crash stack\n"
        def launch(command, **options):
            options["runtime_state"].update(pid=123, started_at=10.0, finished_at=11.0)
            return subprocess.CompletedProcess(command, 0xC0000409, b"original native checkpoint\n")
        arguments = ["runner", str(self.executable), str(source), "--output", str(report),
                     "--windows-crash-debugger", "--windows-original-dump-directory", str(self.root / "dumps")]
        with patch.object(sys, "argv", arguments), patch.object(runner, "os", native_os), \
                patch("native_ui_windows_dump.OriginalWindowsDump", return_value=collector), \
                patch.object(runner, "run_captured_process", side_effect=launch) as captured, \
                patch.object(runner, "required_icon_clear", return_value=("separate clear PASS\n", 0)), \
                patch.object(runner, "windows_crash_diagnostics") as replay, redirect_stdout(io.StringIO()):
            status = runner.main()
        self.assertEqual(status, 1)
        self.assertIn("3221226505 (0xC0000409)", report.read_text())
        self.assertIn("exact original crash stack", report.read_text())
        self.assertEqual(report.with_name("ui-integration-output-original-windows-dump.txt").read_text(),
                         "exact original crash stack\n")
        collector.collect.assert_called_once_with(123, 10.0, 11.0, [], captured)
        self.assertEqual(captured.call_count, 1)
        replay.assert_not_called()

    def test_runner_uncertain_original_tree_blocks_dump_clear_and_replay_launches(self):
        source = self.root / "stuff"; source.mkdir()
        report = self.root / "ui-integration-output.txt"
        native_os = Mock(wraps=os); native_os.name = "nt"
        collector = Mock(notes=[], restore_failed=False, safe_to_release=True)
        collector.__enter__ = Mock(return_value=collector)
        collector.__exit__ = Mock(return_value=False)
        retained = []
        def launch(command, **options):
            retained.append(options["cwd"])
            options["runtime_state"].update(pid=123, started_at=10.0, finished_at=11.0,
                                            safe_to_release=False)
            return subprocess.CompletedProcess(command, 0xC0000409, b"original crash\n")
        arguments = ["runner", str(self.executable), str(source), "--output", str(report),
                     "--windows-crash-debugger", "--windows-original-dump-directory", str(self.root / "dumps")]
        try:
            with patch.object(sys, "argv", arguments), patch.object(runner, "os", native_os), \
                    patch("native_ui_windows_dump.OriginalWindowsDump", return_value=collector), \
                    patch.object(runner, "run_captured_process", side_effect=launch) as captured, \
                    patch.object(runner, "windows_crash_diagnostics") as replay, redirect_stdout(io.StringIO()):
                status = runner.main()
            self.assertEqual(status, 1)
            self.assertEqual(captured.call_count, 1)
            collector.collect.assert_not_called()
            self.assertFalse(collector.safe_to_release)
            replay.assert_not_called()
            self.assertTrue(retained[0].is_dir())
            output = report.read_text()
            self.assertIn("3221226505 (0xC0000409)", output)
            self.assertIn("Original Windows dump analysis was not launched", output)
            self.assertIn("Further diagnostics were not launched", output)
        finally:
            for directory in retained:
                runner.shutil.rmtree(directory)

    def test_registry_restore_failure_cannot_qualify_a_successful_native_process(self):
        source = self.root / "stuff"; source.mkdir()
        report = self.root / "ui-integration-output.txt"
        native_os = Mock(wraps=os); native_os.name = "nt"
        collector = Mock(notes=["Original Windows dump setting restoration failed"],
                         restore_failed=True, safe_to_release=True)
        collector.__enter__ = Mock(return_value=collector)
        collector.__exit__ = Mock(return_value=False)
        markers = (b"native-ui-integration: PASS\nicon-lifecycle-native: terminal replacement PASS\n"
                   b"icon-lifecycle-native: PASS\n"
                   b"native-ui-integration: checkpoint: native icon workers drained\n"
                   b"native-ui-integration: checkpoint: native application cleanup completed\n")
        arguments = ["runner", str(self.executable), str(source), "--output", str(report),
                     "--windows-original-dump-directory", str(self.root / "dumps")]
        with patch.object(sys, "argv", arguments), patch.object(runner, "os", native_os), \
                patch("native_ui_windows_dump.OriginalWindowsDump", return_value=collector), \
                patch.object(runner, "run_captured_process", return_value=subprocess.CompletedProcess([], 0, markers)), \
                patch.object(runner, "required_icon_clear", return_value=("separate clear PASS\n", 0)), \
                redirect_stdout(io.StringIO()):
            status = runner.main()
        self.assertEqual(status, 1)
        self.assertIn("restoration failed", report.read_text())
        collector.collect.assert_not_called()


@unittest.skipUnless(os.name == "nt", "Actual Win32 job/WER acceptance runs on Windows CI")
class NativeWindowsProcessContracts(unittest.TestCase):
    def test_failfast_root_and_live_child_are_tracked_and_original_dump_is_decoded(self):
        # This is a real fail-fast, not ExitProcess masquerading as an exception.
        # The child proves why post-exit PID-tree discovery was insufficient.
        script = r'''import ctypes, os, pathlib, subprocess, sys, time
sys.stdout.reconfigure(encoding='utf-8')
root = pathlib.Path(sys.argv[1])
child_script = "import pathlib,sys,time; pathlib.Path(sys.argv[1]).write_text(str(__import__('os').getpid())); time.sleep(120)"
child = subprocess.Popen([sys.executable, '-c', child_script, str(root / 'child-ready')])
deadline = time.monotonic() + 15
while not (root / 'child-ready').exists():
    if time.monotonic() >= deadline: raise RuntimeError('child did not start')
    time.sleep(.01)
print('original failfast with live child', child.pid, os.environ['GZ_WINDOWS_JOB_PROBE'], flush=True)
failfast = ctypes.WinDLL('kernel32').RaiseFailFastException
failfast.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32]
failfast.restype = None
failfast(None, None, 0)
raise RuntimeError('failfast unexpectedly returned')
'''
        with tempfile.TemporaryDirectory(prefix="windows-job-contract-") as directory:
            root = Path(directory) / "描画 fixture"; root.mkdir()
            executable = Path(sys.executable).resolve()
            state = {"safe_to_release": True}
            environment = os.environ.copy()
            environment["GZ_WINDOWS_JOB_PROBE"] = "描画 frame"
            with OriginalWindowsDump(executable, root, runtime_state=state) as capture:
                self.assertTrue(capture.enabled, "\n".join(capture.notes))
                result = runner.run_captured_process([str(executable), "-c", script, str(root)],
                    cwd=root, environment=environment, timeout=60, runtime_state=state)
                self.assertEqual(result.returncode, 0xC0000602)  # STATUS_FAIL_FAST_EXCEPTION
                self.assertIn(b"original failfast with live child", result.stdout)
                self.assertIn("描画 frame".encode("utf-8"), result.stdout)
                self.assertTrue(state["safe_to_release"], result.stdout.decode(errors="replace"))
                self.assertIn(b"zero active processes", result.stdout)
                self.assertFalse(Path(state["capture_path"]).exists())
                child_pid = int((root / "child-ready").read_text())
                kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int32, ctypes.c_uint32]
                kernel.OpenProcess.restype = ctypes.c_void_p
                kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
                kernel.WaitForSingleObject.restype = ctypes.c_uint32
                kernel.CloseHandle.argtypes = [ctypes.c_void_p]
                kernel.CloseHandle.restype = ctypes.c_int32
                handle = kernel.OpenProcess(0x100000, False, child_pid)  # SYNCHRONIZE
                if handle:
                    try:
                        self.assertEqual(kernel.WaitForSingleObject(handle, 0), 0)
                    finally:
                        kernel.CloseHandle(handle)
                else:
                    self.assertEqual(ctypes.get_last_error(), 87)  # PID already retired
                report = capture.collect(state["pid"], state["started_at"], state["finished_at"],
                                         [], runner.run_captured_process)
                self.assertIn("Original dump CDB exited with status 0.", report)
                self.assertIn("c0000602", report.lower())
                self.assertIn("Executable SHA256:", report)
            self.assertFalse(capture.restore_failed, "\n".join(capture.notes))
            self.assertFalse(capture.folder.exists())


if __name__ == "__main__":
    unittest.main()
