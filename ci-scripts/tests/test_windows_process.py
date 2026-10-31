"""CI contracts for tracked Windows descendants and original crash capture."""
import ctypes
from contextlib import ExitStack, redirect_stdout
import io
import json
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
import native_ui_windows_dump as dump_support
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


class OriginalTimeoutContracts(unittest.TestCase):
    def setUp(self):
        self.storage = tempfile.TemporaryDirectory()
        self.addCleanup(self.storage.cleanup)
        self.root = Path(self.storage.name)

    def hanging_process(self, *, cleanup_safe=True):
        events = []
        api = Mock()
        api.launch.return_value = (100, 200, 300)
        terminated = False
        def wait(handle, timeout):
            events.append(("wait", handle, timeout))
            return 1 if terminated else None
        def terminate(handle):
            nonlocal terminated
            events.append(("terminate", handle))
            terminated = True
        api.wait.side_effect = wait
        api.terminate.side_effect = terminate
        api.close.side_effect = lambda handle: events.append(("close", handle))
        if cleanup_safe:
            api.active.side_effect = lambda handle: (
                events.append(("active", handle)) or (0 if terminated else 1))
        else:
            api.active.side_effect = OSError("injected accounting uncertainty")
        process = process_support.WindowsJobProcess(["fixture.exe"], cwd=self.root, env={},
                                                   stdout=io.BytesIO(), _api=api)
        return process, api, events

    def test_capture_precedes_retirement_and_requires_both_jobs_safe(self):
        raw = b"original native checkpoint\n\x81 flushed bytes\n"
        for original_safe in (False, True):
            for debugger_safe in (False, True):
                with self.subTest(original_safe=original_safe, debugger_safe=debugger_safe):
                    process, api, events = self.hanging_process(cleanup_safe=original_safe)
                    state = {"safe_to_release": True}
                    def launch(command, **options):
                        options["stdout"].write(raw)
                        return process
                    def diagnose(observed, observed_state):
                        self.assertIs(observed, process)
                        self.assertIs(observed_state, state)
                        self.assertEqual(state["pid"], 300)
                        self.assertFalse(state["safe_to_release"])
                        self.assertEqual((process._job, process._process), (100, 200))
                        self.assertIsNone(process.poll())
                        api.terminate.assert_not_called()
                        api.close.assert_not_called()
                        events.append(("diagnose", process.pid))
                        return "original timeout thread stacks\n", debugger_safe
                    with patch.object(runner, "launch_captured_process", side_effect=launch):
                        with self.assertRaises(subprocess.TimeoutExpired) as failure:
                            runner.run_captured_process(["fixture.exe"], cwd=self.root,
                                environment={}, timeout=600, runtime_state=state, on_timeout=diagnose)
                    self.assertEqual(failure.exception.cmd, ["fixture.exe"])
                    self.assertEqual(failure.exception.timeout, 600)
                    self.assertIn(b"original timeout thread stacks", failure.exception.output)
                    self.assertIn(raw, failure.exception.output)
                    self.assertLess(failure.exception.output.index(raw),
                                    failure.exception.output.index(b"original timeout thread stacks"))
                    self.assertEqual(state["output"], failure.exception.output)
                    self.assertEqual(state["safe_to_release"], original_safe and debugger_safe)
                    self.assertEqual(Path(state["capture_path"]).exists(),
                                     not (original_safe and debugger_safe))
                    if Path(state["capture_path"]).exists():
                        self.assertEqual(Path(state["capture_path"]).read_bytes(), raw)
                    self.assertLess(events.index(("diagnose", 300)), events.index(("close", 200)))
                    if original_safe:
                        self.assertLess(events.index(("diagnose", 300)), events.index(("terminate", 100)))
                    self.assertEqual(api.close.call_count, 2)
                    self.assertLessEqual(state["started_at"], state["finished_at"])

    def test_callback_exception_still_retires_original_and_preserves_timeout(self):
        for interruption in (RuntimeError("injected diagnostic failure"), KeyboardInterrupt()):
            with self.subTest(interruption=type(interruption).__name__):
                process, api, events = self.hanging_process()
                state = {"safe_to_release": True}
                def diagnose(observed, observed_state):
                    events.append(("diagnose", observed.pid))
                    raise interruption
                with patch.object(runner, "launch_captured_process", return_value=process):
                    with self.assertRaises(subprocess.TimeoutExpired) as failure:
                        runner.run_captured_process(["fixture.exe"], cwd=self.root, environment={},
                            timeout=600, runtime_state=state, on_timeout=diagnose)
                self.assertEqual(failure.exception.timeout, 600)
                self.assertIn(type(interruption).__name__.encode(), failure.exception.output)
                self.assertFalse(state["safe_to_release"])
                self.assertTrue(Path(state["capture_path"]).is_file())
                self.assertLess(events.index(("diagnose", 300)), events.index(("terminate", 100)))
                self.assertEqual(api.close.call_count, 2)

    def test_callback_never_runs_for_returned_exit_or_non_timeout_wait_failure(self):
        for outcome in (0, 19, 0xC0000005, OSError("wait failed")):
            with self.subTest(outcome=outcome):
                process, api, _ = self.hanging_process()
                api.wait.side_effect = [outcome] if not isinstance(outcome, Exception) else [outcome, 1]
                callback = Mock(side_effect=AssertionError("only a live timeout may attach"))
                state = {"safe_to_release": True}
                with patch.object(runner, "launch_captured_process", return_value=process):
                    if isinstance(outcome, Exception):
                        with self.assertRaisesRegex(OSError, "wait failed"):
                            runner.run_captured_process(["fixture.exe"], cwd=self.root, environment={},
                                timeout=7, runtime_state=state, on_timeout=callback)
                    else:
                        result = runner.run_captured_process(["fixture.exe"], cwd=self.root,
                            environment={}, timeout=7, runtime_state=state, on_timeout=callback)
                        self.assertEqual(result.returncode, outcome)
                callback.assert_not_called()
                self.assertTrue(state["safe_to_release"])

    def test_unsafe_debugger_retains_original_capture_fixture_and_temporary_mesa(self):
        source = self.root / "stuff"
        source.mkdir()
        executable = self.root / "Fixture.exe"
        executable.touch()
        mesa = self.root / "mesa"
        mesa.mkdir()
        for name in ("opengl32.dll", "libgallium_wgl.dll"):
            (mesa / name).write_bytes(b"synthetic Mesa fixture")
        state = {"safe_to_release": True}
        native_os = Mock(wraps=os)
        native_os.name = "nt"
        process, _, _ = self.hanging_process()
        retained = []
        failure = None
        try:
            with patch.object(runner, "os", native_os), \
                    patch.object(runner, "launch_captured_process", return_value=process):
                with self.assertRaisesRegex(RuntimeError, "retained"):
                    with runner.isolated_fixture(source, executable, mesa,
                                                 runtime_state=state) as (directory, environment):
                        retained.append(directory)
                        try:
                            runner.run_captured_process([str(executable)], cwd=directory,
                                environment=environment, timeout=600, runtime_state=state,
                                on_timeout=lambda *args: ("debugger exit uncertain\n", False))
                        except subprocess.TimeoutExpired as error:
                            failure = error
                            raise
            self.assertIsNotNone(failure)
            self.assertEqual(failure.timeout, 600)
            self.assertIn(b"debugger exit uncertain", failure.output)
            self.assertFalse(state["safe_to_release"])
            self.assertTrue(retained[0].is_dir())
            self.assertTrue(Path(state["capture_path"]).is_file())
            for name in ("opengl32.dll", "libgallium_wgl.dll", "Fixture.exe.local"):
                self.assertTrue((executable.parent / name).exists())
        finally:
            # All processes here use the fake API; remove only synthetic files.
            for directory in retained:
                runner.shutil.rmtree(directory)

    def test_safe_capture_file_cleanup_failure_never_replaces_original_timeout(self):
        process, _, _ = self.hanging_process()
        state = {"safe_to_release": True}
        raw = b"original flushed timeout bytes\n\x81"
        native_unlink = Path.unlink
        def unlink(path, *args, **kwargs):
            if path == state.get("capture_path"):
                raise PermissionError(13, "synthetic sharing failure", "private filename must stay hidden")
            return native_unlink(path, *args, **kwargs)
        def launch(command, **options):
            options["stdout"].write(raw)
            return process
        with patch.object(runner, "launch_captured_process", side_effect=launch), \
                patch.object(Path, "unlink", unlink), redirect_stdout(io.StringIO()) as console:
            with self.assertRaises(subprocess.TimeoutExpired) as failure:
                runner.run_captured_process(["fixture.exe"], cwd=self.root, environment={}, timeout=600,
                    runtime_state=state, on_timeout=lambda *args: ("original stack\n", True))
        self.assertEqual(failure.exception.timeout, 600)
        self.assertIn(raw, failure.exception.output)
        self.assertIn(b"original stack", failure.exception.output)
        self.assertEqual(state["output"], failure.exception.output)
        self.assertTrue(state["safe_to_release"])
        self.assertEqual(Path(state["capture_path"]).read_bytes(), raw)
        self.assertIn("PermissionError", state["capture_cleanup_error"])
        self.assertIn("PermissionError", console.getvalue())
        self.assertNotIn("private filename", state["capture_cleanup_error"])
        self.assertNotIn("private filename", console.getvalue())
        self.assertLess(len(state["capture_cleanup_error"]), 1024)


class OriginalTimeoutDebuggerContracts(unittest.TestCase):
    hanging_process = OriginalTimeoutContracts.hanging_process

    def setUp(self):
        OriginalTimeoutContracts.setUp(self)
        self.executable = self.root / "Fixture.exe"
        self.executable.write_bytes(b"synthetic executable identity")
        self.debugger = self.root / "Windows Kits/10/Debuggers/x64/cdb.exe"
        self.debugger.parent.mkdir(parents=True)
        self.debugger.touch()
        (self.root / "Fixture.pdb").write_bytes(b"local symbols")

    def capture(self, process, launch):
        process.args[0] = str(self.executable)
        with patch.dict(os.environ, {"ProgramFiles(x86)": str(self.root),
                                     "_NT_SYMBOL_PATH": "srv*network", "_NT_ALT_SYMBOL_PATH": "remote",
                                     "_NT_EXECUTABLE_IMAGE_PATH": "remote-images"}):
            return dump_support.capture_original_windows_timeout(
                process, self.executable, [self.root], self.root, launch)

    def test_noninvasive_attach_uses_held_original_pid_and_fixed_local_commands(self):
        process, api, _ = self.hanging_process()
        def launch(command, **options):
            self.assertEqual(command[0], str(self.debugger))
            for flag in ("-pv", "-sins", "-netsyms:no", "-noshell", "-lines"):
                self.assertIn(flag, command)
            self.assertNotIn("-pd", command)
            self.assertNotIn("-ses", command)
            self.assertEqual(command[command.index("-p") + 1], str(process.pid))
            self.assertNotIn("-z", command)
            self.assertNotIn(str(self.executable), command)
            self.assertNotIn("--test-native-ui", command)
            commands = command[command.index("-c") + 1]
            self.assertIn("~* kb 40", commands)
            self.assertIn("lmvm Fixture", commands)
            self.assertIn("!lmi Fixture", commands)
            self.assertEqual([part.strip() for part in commands.split(";")[-2:]], [".detach", "q"])
            self.assertEqual(options["timeout"], 45)
            self.assertNotIn("on_timeout", options)
            self.assertNotIn("_NT_SYMBOL_PATH", options["environment"])
            self.assertNotIn("_NT_ALT_SYMBOL_PATH", options["environment"])
            self.assertNotIn("_NT_EXECUTABLE_IMAGE_PATH", options["environment"])
            self.assertEqual(options["cwd"], self.root)
            self.assertEqual(options["runtime_state"], {"safe_to_release": True})
            api.terminate.assert_not_called()
            api.close.assert_not_called()
            return subprocess.CompletedProcess(command, 0, b"native original stack\n")
        text, safe = self.capture(process, launch)
        self.assertTrue(safe)
        self.assertIn("PID 300", text)
        self.assertIn("Executable SHA256:", text)
        self.assertIn("PDB Fixture.pdb SHA256:", text)
        self.assertIn("native original stack", text)
        self.assertIsNone(process.poll())
        api.terminate.assert_not_called()
        process.close()

    def test_dead_original_and_missing_debugger_never_launch_or_replay(self):
        for dead in (False, True):
            with self.subTest(dead=dead):
                process, api, _ = self.hanging_process()
                if dead:
                    api.wait.side_effect = None
                    api.wait.return_value = 19
                else:
                    self.debugger.unlink()
                launch = Mock(side_effect=AssertionError("no attach or replay"))
                text, safe = self.capture(process, launch)
                self.assertTrue(safe)
                self.assertTrue(text.strip())
                launch.assert_not_called()
                process.close()

    def test_mismatched_identity_and_exit_during_hashing_do_not_attach(self):
        other = self.root / "Other.exe"
        other.write_bytes(b"different process image")
        for scenario in ("image", "pid", "late-exit"):
            with self.subTest(scenario=scenario):
                process, api, _ = self.hanging_process()
                process.args[0] = str(other if scenario == "image" else self.executable)
                if scenario == "pid":
                    process.pid = True  # bool is not a valid numeric PID witness.
                if scenario == "late-exit":
                    api.wait.side_effect = [None, 19]
                launch = Mock(side_effect=AssertionError("identity must remain exact and live"))
                with patch.dict(os.environ, {"ProgramFiles(x86)": str(self.root)}):
                    text, safe = dump_support.capture_original_windows_timeout(
                        process, self.executable, [self.root], self.root, launch)
                self.assertTrue(safe)
                self.assertTrue(text.strip())
                launch.assert_not_called()
                process.close()

    def test_debugger_timeout_or_error_keeps_raw_evidence_and_independent_cleanup_state(self):
        for safe in (False, True):
            for failure in ("timeout", "error", "returned"):
                with self.subTest(safe=safe, failure=failure):
                    process, _, _ = self.hanging_process()
                    def launch(command, **options):
                        options["runtime_state"].update(safe_to_release=safe,
                                                       output=b"partial original stack\n")
                        if failure == "timeout":
                            raise subprocess.TimeoutExpired(command, 45, output=b"partial original stack\n")
                        if failure == "error":
                            raise OSError("injected debugger launch failure")
                        return subprocess.CompletedProcess(command, 19, b"partial original stack\n")
                    text, actual_safe = self.capture(process, launch)
                    self.assertEqual(actual_safe, safe)
                    self.assertIn("partial original stack", text)
                    self.assertTrue("45" in text if failure == "timeout" else
                                    "OSError" in text if failure == "error" else "19" in text)
                    process.close()

    def test_complete_timeout_report_is_bounded_even_with_unicode_output(self):
        process, _, _ = self.hanging_process()
        raw = ("先頭 " + "描画" * (128 * 1024) + " 終端").encode("utf-8")
        def launch(command, **options):
            return subprocess.CompletedProcess(command, 0, raw)
        text, safe = self.capture(process, launch)
        self.assertTrue(safe)
        self.assertLessEqual(len(text.encode("utf-8")), 128 * 1024)
        self.assertIn("Executable SHA256:", text)
        self.assertIn("先頭", text)
        self.assertIn("終端", text)
        self.assertIn("truncated", text.lower())
        self.assertIn("Original timeout CDB exited with status 0.", text)
        self.assertNotIn("\ufffd", text)
        process.close()


class WindowsReleaseContracts(unittest.TestCase):
    def setUp(self):
        self.storage = tempfile.TemporaryDirectory()
        self.addCleanup(self.storage.cleanup)
        self.root = Path(self.storage.name)
        self.executable = self.root / "Fixture.exe"
        self.executable.write_bytes(b"fixture identity")
        self.source = self.root / "stuff"
        self.source.mkdir()
        self.report = self.root / "native-output.txt"
        self.release = self.root / "native-output-release.json"
        self.identity = {"GITHUB_SHA": "a" * 40, "GITHUB_RUN_ID": "12345", "GITHUB_RUN_ATTEMPT": "2"}
        self.launches = []

    def invoke(self, launch, *, clear=None, debugger=None, writer=None):
        native_os = Mock(wraps=os)
        native_os.name = "nt"
        def capture(command, **options):
            self.launches.append(options["cwd"])
            return launch(command, **options)
        with ExitStack() as stack:
            stack.enter_context(patch.object(sys, "argv", ["runner", str(self.executable), str(self.source),
                "--output", str(self.report), "--windows-crash-debugger", "--require-vector-workflows"]))
            stack.enter_context(patch.object(runner, "os", native_os))
            stack.enter_context(patch.dict(os.environ, self.identity))
            captured = stack.enter_context(patch.object(runner, "run_captured_process", side_effect=capture))
            clear_mock = stack.enter_context(patch.object(runner, "required_icon_clear",
                side_effect=clear or (lambda *args, **kwargs: ("separate clear PASS\n", 0))))
            debug_mock = stack.enter_context(patch.object(runner, "windows_crash_diagnostics",
                side_effect=debugger or (lambda *args, **kwargs: "separate debugger\n")))
            if writer is not None:
                stack.enter_context(patch.object(runner, "write_windows_release", side_effect=writer))
            console = stack.enter_context(redirect_stdout(io.StringIO()))
            status = runner.main()
        return status, captured, clear_mock, debug_mock, console.getvalue()

    def record_original(self, options, status=19):
        initial = json.loads(self.release.read_text())
        self.assertEqual(initial["schema"], 1)
        self.assertFalse(initial["safe_to_release"])
        self.assertIsNone(initial["original_pid"])
        self.assertIsNone(initial["original_status"])
        self.assertEqual(initial["source_sha"], self.identity["GITHUB_SHA"])
        options["runtime_state"].update(pid=300, started_at=10.0, finished_at=11.0,
                                        safe_to_release=True)
        return subprocess.CompletedProcess([str(self.executable)], status, b"original native bytes\n")

    def test_stale_success_is_invalidated_before_launch_and_identity_survives_later_processes(self):
        self.release.write_text(json.dumps({"safe_to_release": True, "source_sha": "old", "original_pid": 1}))
        def clear(*args, **kwargs):
            state = args[4]
            state.update(pid=999, started_at=20.0, finished_at=21.0, timed_out=True)
            return "separate clear PASS\n", 0
        status, captured, _, debugger, _ = self.invoke(
            lambda command, **options: self.record_original(options), clear=clear)
        self.assertEqual(status, 1)
        self.assertEqual(captured.call_count, 1)
        debugger.assert_not_called()
        self.assertEqual(json.loads(self.release.read_text()), {
            "schema": 1, "source_sha": self.identity["GITHUB_SHA"], "run_id": "12345", "run_attempt": "2",
            "original_pid": 300, "timed_out": False, "original_status": 19,
            "started_at": 10.0, "finished_at": 11.0, "safe_to_release": True})
        self.assertIn("status 19 (0x00000013)", self.report.read_text())

    def test_unsafe_clear_or_final_debugger_blocks_release_without_replacing_original_identity(self):
        for unsafe_stage in ("clear", "debugger"):
            with self.subTest(unsafe_stage=unsafe_stage):
                def clear(*args, **kwargs):
                    if unsafe_stage == "clear":
                        args[4]["safe_to_release"] = False
                    return "clear diagnostic\n", 1 if unsafe_stage == "clear" else 0
                def debug(*args, **kwargs):
                    kwargs["runtime_state"]["safe_to_release"] = False
                    return "debugger tree retirement uncertain\n"
                status, _, _, debugger, _ = self.invoke(
                    lambda command, **options: self.record_original(options, 0xC0000005),
                    clear=clear, debugger=debug)
                self.assertEqual(status, 1)
                record = json.loads(self.release.read_text())
                self.assertFalse(record["safe_to_release"])
                self.assertEqual(record["original_pid"], 300)
                self.assertEqual(record["original_status"], 0xC0000005)
                self.assertFalse(record["timed_out"])
                self.assertEqual(debugger.call_count, 1 if unsafe_stage == "debugger" else 0)
                self.assertIn("status 3221225477 (0xC0000005)", self.report.read_text())

    def test_actual_timeout_callback_wiring_retains_timeout_report_and_unsafe_fixture(self):
        retained = []
        def launch(command, **options):
            self.record_original(options)
            state = options["runtime_state"]
            text, safe = options["on_timeout"](Mock(pid=300), state)
            state.update(timed_out=True, safe_to_release=safe, original_windows_timeout=text)
            state["output"] = b"original hung bytes\n" + text.encode()
            retained.append(options["cwd"])
            raise subprocess.TimeoutExpired(command, options["timeout"], output=state["output"])
        try:
            with patch.object(dump_support, "capture_original_windows_timeout",
                              return_value=("original PID 300 stack; debugger uncertain\n", False)) as helper:
                status, captured, _, debugger, _ = self.invoke(launch)
            self.assertEqual(status, 1)
            self.assertEqual(captured.call_count, 1)
            helper.assert_called_once()
            self.assertIs(helper.call_args.args[-1], captured)
            debugger.assert_not_called()
            record = json.loads(self.release.read_text())
            self.assertTrue(record["timed_out"])
            self.assertIsNone(record["original_status"])
            self.assertEqual(record["original_pid"], 300)
            self.assertFalse(record["safe_to_release"])
            self.assertTrue(retained[0].is_dir())
            output = self.report.read_text()
            self.assertIn("timed out after 600 seconds", output)
            self.assertIn("original hung bytes", output)
            self.assertIn("original PID 300 stack", output)
            self.assertEqual(self.report.with_name("native-output-original-windows-timeout.txt").read_text(),
                             "original PID 300 stack; debugger uncertain\n")
        finally:
            for directory in retained:
                runner.shutil.rmtree(directory)

    def test_initial_atomic_write_failure_removes_stale_success_and_prevents_any_launch(self):
        self.release.write_text('{"safe_to_release": true}')
        native_replace = os.replace
        def fail_release(source, destination):
            if Path(destination) == self.release:
                raise PermissionError("injected initial publication failure")
            return native_replace(source, destination)
        with patch.object(runner.os, "replace", side_effect=fail_release):
            status, captured, clear, debugger, _ = self.invoke(
                Mock(side_effect=AssertionError("no launch without invalidation")))
        self.assertEqual(status, 1)
        captured.assert_not_called()
        clear.assert_not_called()
        debugger.assert_not_called()
        self.assertFalse(self.release.exists())
        self.assertEqual(list(self.root.glob(".native-output-release.json.*.tmp")), [])

    def test_failed_final_write_leaves_initial_false_and_preserves_original_failure(self):
        for timed_out in (False, True):
            with self.subTest(timed_out=timed_out):
                native_writer = runner.write_windows_release
                def write(path, record, *, invalidate=False):
                    if not invalidate:
                        raise PermissionError("injected final publication failure")
                    return native_writer(path, record, invalidate=invalidate)
                def launch(command, **options):
                    result = self.record_original(options)
                    if timed_out:
                        options["runtime_state"]["timed_out"] = True
                        raise subprocess.TimeoutExpired(command, options["timeout"], output=result.stdout)
                    return result
                status, captured, _, _, console = self.invoke(launch, writer=write)
                self.assertEqual(status, 1)
                self.assertEqual(captured.call_count, 1)
                initial = json.loads(self.release.read_text())
                self.assertFalse(initial["safe_to_release"])
                self.assertIsNone(initial["original_pid"])
                self.assertIsNone(initial["original_status"])
                self.assertIn("Could not publish Windows cleanup witness", console)
                self.assertIn("timed out after 600 seconds" if timed_out else "status 19 (0x00000013)",
                              self.report.read_text())


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

    def test_missing_dump_witnesses_are_read_only_and_do_not_print_registry_paths(self):
        registry = FakeRegistry({"Auto": ("1", 1), "Start": (4, 4),
                                 "Disabled": ("private\\account\\value", 1),
                                 "Fixture.exe": (1, 4)})
        old = dict(registry.values)
        with patch.object(dump_support, "_wer_service_diagnostic", return_value="WerSvc observed state=1"):
            text = dump_support.windows_dump_prerequisites("Fixture.exe", registry)
        self.assertIn("AeDebug Auto: 1 (registry type 1)", text)
        self.assertIn("AeDebug application exclusion: 1", text)
        self.assertIn("WerSvc start mode: 4", text)
        self.assertIn("WER policy Disabled: non-scalar", text)
        self.assertIn("LocalDumps application DumpType: absent", text)
        self.assertIn("WerSvc observed state=1", text)
        self.assertNotIn("private", text)
        self.assertEqual(registry.values, old)
        self.assertFalse(registry.deleted)

    def test_prerequisite_read_failure_is_bounded_and_does_not_hide_missing_dump(self):
        registry = FakeRegistry()
        with OriginalWindowsDump(self.executable, self.root, registry=registry) as capture:
            registry.OpenKey = Mock(side_effect=PermissionError("private path must not be printed"))
            with patch.object(dump_support, "_wer_service_diagnostic", return_value="WerSvc state unavailable"):
                text = capture.collect(123, 10, 11, [], Mock(side_effect=AssertionError("no replay")))
            self.assertIn("Original Windows dump unavailable", text)
            self.assertIn("PermissionError", text)
            self.assertNotIn("private path", text)
            self.assertLess(len(text), 4096)

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
    def test_original_hang_is_inspected_before_parent_and_child_retirement(self):
        script = r'''import pathlib, subprocess, sys, time
root = pathlib.Path(sys.argv[1])
child_script = "import os,pathlib,sys,time; pathlib.Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(120)"
child = subprocess.Popen([sys.executable, '-c', child_script, str(root / 'child-ready')])
deadline = time.monotonic() + 10
while not (root / 'child-ready').exists():
    if time.monotonic() >= deadline: raise RuntimeError('hang child did not start')
    time.sleep(.01)
print('original hang with live child', child.pid, flush=True)
child.wait()
raise RuntimeError('hang child unexpectedly exited')
'''
        root = Path(tempfile.mkdtemp(prefix="windows-hang-contract-"))
        state = {"safe_to_release": True}
        # A failed proof must retain the live fixture even when this test fails.
        self.addCleanup(lambda: runner.shutil.rmtree(root) if state["safe_to_release"] else None)
        executable = Path(sys.executable).resolve()
        observed = []
        def diagnose(process, original_state):
            self.assertIs(original_state, state)
            self.assertTrue((root / "child-ready").is_file())
            self.assertIsNone(process.poll())
            self.assertIsNotNone(process._process)
            text, safe = dump_support.capture_original_windows_timeout(
                process, executable, [], root, runner.run_captured_process)
            observed.append((process.pid, text, safe, process.poll()))
            return text, safe
        with self.assertRaises(subprocess.TimeoutExpired) as failure:
            runner.run_captured_process([str(executable), "-c", script, str(root)],
                cwd=root, environment=os.environ.copy(), timeout=15,
                runtime_state=state, on_timeout=diagnose)
        self.assertEqual(failure.exception.timeout, 15)
        self.assertIn(b"original hang with live child", failure.exception.output)
        self.assertEqual(len(observed), 1, failure.exception.output.decode(errors="replace"))
        pid, text, safe, status_after_attach = observed[0]
        self.assertEqual(pid, state["pid"])
        self.assertIsNone(status_after_attach, text)
        self.assertTrue(safe, text)
        self.assertIn("PID " + str(pid), text)
        self.assertIn("Executable SHA256:", text)
        self.assertIn("exited with status 0", text)
        # Require decoded stack frames, not the echoed debugger command.
        self.assertRegex(text.lower(), r"ntdll!nt(?:wait|delay)|kernelbase!(?:wait|sleep)")
        self.assertTrue(state["safe_to_release"], text)
        self.assertGreaterEqual(failure.exception.output.count(b"zero active processes"), 2)
        self.assertFalse(Path(state["capture_path"]).exists())
        child_pid = int((root / "child-ready").read_text())
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int32, ctypes.c_uint32]
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        kernel.WaitForSingleObject.restype = ctypes.c_uint32
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        kernel.CloseHandle.restype = ctypes.c_int32
        for exited_pid in (pid, child_pid):
            handle = kernel.OpenProcess(0x100000, False, exited_pid)
            if handle:
                try:
                    self.assertEqual(kernel.WaitForSingleObject(handle, 0), 0)
                finally:
                    kernel.CloseHandle(handle)
            else:
                self.assertEqual(ctypes.get_last_error(), 87)

    def test_failfast_root_and_live_child_are_tracked_and_original_dump_is_decoded(self):
        # This is a real fail-fast, not ExitProcess masquerading as an exception.
        # The child proves why post-exit PID-tree discovery was insufficient.
        script = r'''import ctypes, os, pathlib, subprocess, sys, time
sys.stdout.reconfigure(encoding='utf-8')
root = pathlib.Path(sys.argv[1])
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.GetErrorMode.argtypes = []
kernel.GetErrorMode.restype = ctypes.c_uint32
mode = kernel.GetErrorMode()
print('original child GetErrorMode=' + hex(mode), flush=True)
assert mode & 2 == 0, 'child inherited SEM_NOGPFAULTERRORBOX suppression'
folder_probe = pathlib.Path(sys.argv[2]) / 'child-write-probe'
folder_probe.write_bytes(b'capture folder access')
assert folder_probe.read_bytes() == b'capture folder access'
folder_probe.unlink()
print('original child private dump folder write/read/delete PASS', flush=True)
child_script = "import pathlib,sys,time; pathlib.Path(sys.argv[1]).write_text(str(__import__('os').getpid())); time.sleep(120)"
child = subprocess.Popen([sys.executable, '-c', child_script, str(root / 'child-ready')])
deadline = time.monotonic() + 15
while not (root / 'child-ready').exists():
    if time.monotonic() >= deadline: raise RuntimeError('child did not start')
    time.sleep(.01)
print('original failfast with live child', child.pid, os.environ['GZ_WINDOWS_JOB_PROBE'], flush=True)
failfast = kernel.RaiseFailFastException
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
                kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                kernel.GetErrorMode.argtypes = []
                kernel.GetErrorMode.restype = ctypes.c_uint32
                kernel.SetErrorMode.argtypes = [ctypes.c_uint32]
                kernel.SetErrorMode.restype = ctypes.c_uint32
                parent_mode = kernel.GetErrorMode()
                print(f"Windows probe parent GetErrorMode before controlled suppression={parent_mode:#x}", flush=True)
                # Qualify creation-time normalization with a known suppressed
                # parent, without changing any machine policy/service setting.
                try:
                    kernel.SetErrorMode(parent_mode | 2)
                    self.assertEqual(kernel.GetErrorMode() & 2, 2)
                    result = runner.run_captured_process([str(executable), "-c", script, str(root), str(capture.folder)],
                        cwd=root, environment=environment, timeout=60, runtime_state=state)
                finally:
                    kernel.SetErrorMode(parent_mode)
                self.assertEqual(kernel.GetErrorMode(), parent_mode)
                self.assertEqual(result.returncode, 0xC0000602)  # STATUS_FAIL_FAST_EXCEPTION
                mode_lines = [line for line in result.stdout.decode("utf-8", errors="replace").splitlines()
                              if line.startswith("original child GetErrorMode=")]
                self.assertEqual(len(mode_lines), 1)
                self.assertEqual(int(mode_lines[0].split("=", 1)[1], 16) & 2, 0)
                self.assertIn(b"original child private dump folder write/read/delete PASS", result.stdout)
                self.assertIn(b"original failfast with live child", result.stdout)
                self.assertIn("描画 frame".encode("utf-8"), result.stdout)
                self.assertTrue(state["safe_to_release"], result.stdout.decode(errors="replace"))
                self.assertIn(b"zero active processes", result.stdout)
                self.assertFalse(Path(state["capture_path"]).exists())
                child_pid = int((root / "child-ready").read_text())
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
                self.assertIn("Original dump CDB exited with status 0.", report,
                              report + "\nOriginal probe output:\n" + result.stdout.decode("utf-8", errors="replace"))
                self.assertIn("c0000602", report.lower())
                self.assertIn("Executable SHA256:", report)
            self.assertFalse(capture.restore_failed, "\n".join(capture.notes))
            self.assertFalse(capture.folder.exists())


if __name__ == "__main__":
    unittest.main()
