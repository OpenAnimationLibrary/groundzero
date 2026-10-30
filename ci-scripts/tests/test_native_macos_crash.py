"""Original macOS report identity/privacy contracts; no application launches."""

from datetime import datetime, timezone
from contextlib import nullcontext, redirect_stdout
import io
import json
import os
from pathlib import Path, PurePosixPath
import sys
import tempfile
import subprocess
import struct
import uuid
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import native_macos_crash as reports


class OriginalMacCrashTests(unittest.TestCase):
    def setUp(self):
        self.storage = tempfile.TemporaryDirectory()
        self.addCleanup(self.storage.cleanup)
        self.root = Path(self.storage.name)
        # Report identities use macOS paths even when these pure-reader
        # contracts are run on a Windows/Linux CI host.
        self.executable = PurePosixPath("/Applications/native-crash-fixture/OpenToonz.app/Contents/MacOS/OpenToonz")
        self.directory = self.root / "reports"
        self.directory.mkdir()
        self.start = datetime(2026, 10, 30, 21, 28, 40, tzinfo=timezone.utc).timestamp()
        self.finish = self.start + 40
        self.pid = 63307

    def ips(self, **changes):
        payload = {
            "pid": self.pid, "procName": self.executable.name,
            "procPath": str(self.executable),
            "procLaunch": "2026-10-30 21:28:40.125 +0000",
            "captureTime": "2026-10-30 21:29:19.500 +0000",
            "exception": {"type": "EXC_BAD_ACCESS", "signal": "SIGSEGV"},
            "faultingThread": 1,
            "threads": [{"frames": [{"imageIndex": 0, "symbol": "worker_loop"}]},
                        {"triggered": True, "frames": [
                            {"imageIndex": 0, "imageOffset": 123, "symbol": "TPanel::~TPanel()", "symbolLocation": 6}]}],
            "usedImages": [{"name": "OpenToonz", "path": "/Users/private-person/build/OpenToonz",
                            "base": 4096, "uuid": "12345678-1234-1234-1234-123456789abc", "arch": "x86_64"}],
            "crashReporterKey": "PRIVATE-IDENTIFIER",
            "asi": {"private": ["/Users/private-person/Documents/secret.txt"]},
        }
        payload.update(changes)
        return json.dumps({"bug_type": "309", "timestamp": "ignored tracking timestamp"}) + "\n" + json.dumps(payload)

    def write(self, name, content):
        path = self.directory / name
        path.write_bytes(content if isinstance(content, bytes) else content.encode("utf-8"))
        os.utime(path, (self.finish, self.finish))
        return path

    def macho(self, identity="12345678-1234-1234-1234-123456789abc", *, endian="<", bits=64,
              filetype=2, duplicate=False):
        command = struct.pack(endian + "2I", 0x1b, 24) + uuid.UUID(identity).bytes
        commands = command * (2 if duplicate else 1)
        header = struct.pack(endian + "7I", 0xfeedfacf if bits == 64 else 0xfeedface,
                             0x01000007, 3, filetype, 2 if duplicate else 1, len(commands), 0)
        return header + (b"\0" * 4 if bits == 64 else b"") + commands

    def prepare_binary(self, content=None):
        self.executable = self.root / "OpenToonz.app/Contents/MacOS/OpenToonz"
        self.executable.parent.mkdir(parents=True, exist_ok=True)
        self.executable.write_bytes(self.macho() if content is None else content)
        return self.executable

    def collect(self, **options):
        return reports.collect_original_macos_crash(
            self.executable, self.pid, self.start, self.finish,
            report_directories=[self.directory], timeout=0, **options)

    def test_exact_ips_uses_original_faulting_thread_without_private_metadata(self):
        self.write("OpenToonz-original.ips", self.ips())
        output = self.collect()
        self.assertIn("MATCHED original PID 63307; no replay", output)
        self.assertIn("EXC_BAD_ACCESS", output)
        self.assertIn("TPanel::~TPanel() + 6 [imageOffset=123]", output)
        self.assertLess(output.index("Thread 1 CRASHED"), output.index("Thread 0"))
        for private in (str(self.root), "/Users/", "private-person", "PRIVATE-IDENTIFIER", "secret.txt"):
            self.assertNotIn(private, output)

    def test_each_identity_field_is_required_even_when_filename_matches(self):
        invalid = [dict(pid=self.pid + 1), dict(pid=True), dict(procName="OtherApp"),
                   dict(procPath=str(self.root / "other/OpenToonz")),
                   dict(procPath="/Users/USER/*/OpenToonz.app/Contents/MacOS/OpenToonz"),
                   dict(captureTime="2026-10-30 20:29:19 +0000"),
                   dict(captureTime="2026-10-30 21:31:19 +0000"),
                   dict(captureTime=None), dict(captureTime="2026-10-30 21:29:19"),
                   dict(procLaunch="2026-10-29 21:28:40 +0000"),
                   dict(isSimulated=True), dict(isNonFatal=True)]
        for index, changes in enumerate(invalid):
            with self.subTest(changes=changes):
                path = self.write(f"OpenToonz-invalid{index}.ips", self.ips(**changes))
                self.assertIn("UNAVAILABLE", self.collect())
                path.unlink()

    def test_newest_unrelated_report_cannot_replace_exact_older_match(self):
        correct = self.write("OpenToonz-original.ips", self.ips())
        wrong = self.write("OpenToonz-newer.ips", self.ips(pid=77))
        os.utime(wrong, (self.finish + 2, self.finish + 2))
        self.assertIn("MATCHED original PID 63307", self.collect())
        self.assertTrue(correct.exists())
        self.assertTrue(wrong.exists())

    def test_no_unrelated_or_symlink_report_is_read_or_removed(self):
        unrelated = self.write("OtherApp.ips", self.ips())
        link = self.directory / "OpenToonz-link.ips"
        try:
            link.symlink_to(unrelated)
        except OSError:
            self.skipTest("Host does not permit creating symbolic links")
        with mock.patch.object(reports, "_read", wraps=reports._read) as reader:
            self.assertIn("UNAVAILABLE", self.collect())
            reader.assert_not_called()
        self.assertTrue(unrelated.exists())
        self.assertTrue(link.is_symlink())

    def test_known_username_redaction_only_without_other_path_guessing(self):
        executable = Path("/Users/runner/build/OpenToonz.app/Contents/MacOS/OpenToonz")
        with mock.patch.object(Path, "home", return_value=Path("/Users/runner")):
            self.assertTrue(reports._path_matches(
                "/Users/USER/build/OpenToonz.app/Contents/MacOS/OpenToonz", executable))
            self.assertFalse(reports._path_matches(
                "/Users/USER/*/OpenToonz.app/Contents/MacOS/OpenToonz", executable))

    def test_redacted_ips_requires_exact_thin_binary_uuid_and_unique_main_image(self):
        self.prepare_binary()
        redacted = "/Users/USER/*/OpenToonz.app/Contents/MacOS/OpenToonz"
        image = dict(name="OpenToonz", uuid="12345678-1234-1234-1234-123456789abc", path=redacted)
        self.write("OpenToonz-redacted.ips", self.ips(procPath=redacted, usedImages=[image]))
        output = self.collect()
        self.assertIn("MATCHED original PID 63307", output)
        self.assertIn("exact Mach-O LC_UUID and unique main image", output)
        self.assertNotIn("/Users/", output)
        for images in ([dict(image, uuid="aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa")],
                       [image, image], [dict(image, name="OtherApp")],
                       [dict(image, path="/Applications/another-build/OpenToonz")]):
            with self.subTest(images=images):
                self.write("OpenToonz-redacted.ips", self.ips(procPath=redacted, usedImages=images))
                self.assertIn("UNAVAILABLE", self.collect())
        # A conflicting unredacted path is never waived by a matching UUID.
        self.write("OpenToonz-redacted.ips", self.ips(
            procPath="/Applications/another-build/OpenToonz", usedImages=[image]))
        self.assertIn("UNAVAILABLE", self.collect())

    def test_redacted_legacy_binary_images_uuid_proves_same_executable(self):
        self.prepare_binary()
        redacted = "/Users/USER/*/OpenToonz.app/Contents/MacOS/OpenToonz"
        content = (f"Process: OpenToonz [{self.pid}]\nPath: {redacted}\n"
                   "Date/Time: 2026-10-30 21:29:19.500 +0000\n"
                   "Exception Type: EXC_BAD_ACCESS (SIGSEGV)\n"
                   "Thread 0 Crashed:\n0 OpenToonz 0x1000 original_fault + 4\n\n"
                   "Binary Images:\n0x1000 - 0x2000 +OpenToonz (1.7) "
                   f"<12345678-1234-1234-1234-123456789ABC> {redacted}\n")
        self.write("OpenToonz-redacted.crash", content)
        self.assertIn("exact Mach-O LC_UUID", self.collect())
        self.write("OpenToonz-redacted.crash", content.replace(
            "+OpenToonz (1.7)", "+org.example.groundzero (1.7)"))
        self.assertIn("exact Mach-O LC_UUID", self.collect())
        self.write("OpenToonz-redacted.crash", content.replace(
            "12345678-1234-1234-1234-123456789ABC", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"))
        self.assertIn("UNAVAILABLE", self.collect())

    def test_macho_uuid_reader_rejects_truncation_ambiguity_and_unbounded_headers(self):
        identity = uuid.UUID("12345678-1234-1234-1234-123456789abc")
        for bits in (32, 64):
            for endian in ("<", ">"):
                with self.subTest(bits=bits, endian=endian):
                    self.assertEqual(reports._macho_uuid(self.prepare_binary(
                        self.macho(bits=bits, endian=endian))), identity)
        oversized = struct.pack("<8I", 0xfeedfacf, 0x01000007, 3, 2, 1,
                                reports.MAX_MACHO_COMMAND_BYTES + 8, 0, 0)
        invalid = [self.macho()[:-1], self.macho()[:16], self.macho(duplicate=True),
                   self.macho(filetype=6), oversized, b"\xca\xfe\xba\xbe" + b"\0" * 64,
                   self.macho()[:36] + struct.pack("<I", 0) + self.macho()[40:]]
        for content in invalid:
            with self.subTest(content=content[:40]):
                self.assertIsNone(reports._macho_uuid(self.prepare_binary(content)))

    def test_legacy_crash_identity_and_native_stack(self):
        content = f"""Process: OpenToonz [{self.pid}]
Path: {self.executable}
Date/Time: 2026-10-30 14:29:19.500 -0700
Launch Time: 2026-10-30 14:28:40 -0700
Exception Type: EXC_BAD_ACCESS (SIGSEGV)
Thread 0 Crashed:: Dispatch queue: com.apple.main-thread
0 OpenToonz 0x1000 ConvertPopup::~ConvertPopup() + 4
1 OpenToonz 0x1010 native_fixture "/Users/private person/My Project/source.cpp":14

Thread 1:
0 libsystem 0x2000 thread_wait + 8

Binary Images:
 /Users/private-person/other
"""
        path = self.write("OpenToonz-original.crash", content)
        output = self.collect()
        self.assertIn("MATCHED", output)
        self.assertIn("ConvertPopup::~ConvertPopup()", output)
        self.assertIn("Thread 0 CRASHED", output)
        self.assertIn("Thread 1", output)
        self.assertNotIn("private-person", output)
        self.assertNotIn("private person", output)
        self.assertNotIn("My Project", output)
        path.write_text(content.replace(f"[{self.pid}]", "[9]"), encoding="utf-8")
        self.assertIn("UNAVAILABLE", self.collect())

    def test_malformed_partial_and_oversized_reports_are_bounded(self):
        self.write("OpenToonz-partial.ips", self.ips()[:-20])
        self.write("OpenToonz-binary.ips", b"\xff\xfe")
        self.write("OpenToonz-large.ips", b"x" * 1000)
        with mock.patch.object(reports, "MAX_REPORT_BYTES", 500):
            output = self.collect()
        self.assertIn("UNAVAILABLE", output)
        self.assertNotIn(str(self.root), output)

    def test_report_open_is_nonblocking_and_no_follow_before_type_check(self):
        path = self.write("OpenToonz-original.ips", self.ips())
        with mock.patch.object(reports.os, "open", wraps=os.open) as opened:
            content, count = reports._read(path, reports.MAX_REPORT_BYTES)
        self.assertEqual(content, path.read_bytes())
        self.assertEqual(count, len(content))
        flags = opened.call_args.args[1]
        for flag in (getattr(os, "O_NONBLOCK", 0), getattr(os, "O_NOFOLLOW", 0)):
            self.assertEqual(flags & flag, flag)

    def test_delayed_os_write_is_polled_without_new_process(self):
        moments = iter([0, 0, 0.1, 0.2, 0.3])
        def arrive(_delay):
            self.write("OpenToonz-late.ips", self.ips())
        with mock.patch.object(reports.time, "monotonic", side_effect=lambda: next(moments)), \
                mock.patch.object(reports.time, "sleep", side_effect=arrive) as sleep:
            output = reports.collect_original_macos_crash(
                self.executable, self.pid, self.start, self.finish,
                report_directories=[self.directory], timeout=1)
        self.assertIn("MATCHED", output)
        self.assertEqual(sleep.call_count, 1)

    def test_legacy_identity_headers_wait_for_the_original_stack(self):
        header = (f"Process: OpenToonz [{self.pid}]\nPath: {self.executable}\n"
                  "Date/Time: 2026-10-30 21:29:19.500 +0000\n"
                  "Exception Type: EXC_BAD_ACCESS (SIGSEGV)\n"
                  "Thread 0:\n0 libsystem 0x2000 thread_wait + 8\n\n")
        self.write("OpenToonz-incomplete.crash", header)
        self.assertIn("UNAVAILABLE", self.collect())
        moments = iter([0, 0, 0.1, 0.2])
        def complete(_delay):
            self.write("OpenToonz-incomplete.crash", header +
                       "Thread 1 Crashed:\n0 OpenToonz 0x1000 TPanel::~TPanel() + 4\n")
        with mock.patch.object(reports.time, "monotonic", side_effect=lambda: next(moments)), \
                mock.patch.object(reports.time, "sleep", side_effect=complete) as sleep:
            output = reports.collect_original_macos_crash(
                self.executable, self.pid, self.start, self.finish,
                report_directories=[self.directory], timeout=1)
        self.assertIn("TPanel::~TPanel()", output)
        self.assertLess(output.index("Thread 1 CRASHED"), output.index("Thread 0"))
        self.assertEqual(sleep.call_count, 1)

    def test_ips_triggered_thread_survives_thread_limit_without_fault_index(self):
        threads = [{"frames": [{"symbol": "thread_wait"}]} for _ in range(20)]
        threads[19] = {"triggered": True, "frames": [{"symbol": "original_fault"}]}
        self.write("OpenToonz-original.ips", self.ips(threads=threads, faultingThread=None))
        output = self.collect()
        self.assertIn("Thread 19 CRASHED", output)
        self.assertIn("original_fault", output)
        self.assertLess(output.index("Thread 19 CRASHED"), output.index("Thread 0"))

    def test_stack_output_bounds_and_control_path_redaction(self):
        frames = [{"imageIndex": 0, "symbol": "\x1b[31mfunction /Users/private person/My Project/secret.cpp " + "symbol" * 200}]
        self.write("OpenToonz-original.ips", self.ips(
            threads=[{"frames": frames * 100}] * 20, faultingThread=19))
        output = self.collect()
        self.assertLessEqual(len(output), reports.MAX_OUTPUT + 60)
        self.assertIn("Thread 19 CRASHED", output)
        self.assertNotIn("private-person", output)
        self.assertNotIn("private person", output)
        self.assertNotIn("My Project", output)
        self.assertNotIn("\x1b", output)

    def test_apple_fractional_time_offset_and_no_timezone_rejection(self):
        self.assertEqual(reports._date("2026-10-30 21:28:40.125 +0000"), self.start + .125)
        self.assertEqual(reports._date("2026-10-30 14:28:40.125 -0700"), self.start + .125)
        self.assertEqual(reports._date("2026-10-30T21:28:40.125Z"), self.start + .125)
        self.assertIsNone(reports._date("2026-10-30 21:28:40.125"))

    def test_missing_invalid_identity_is_explicit_without_scanning(self):
        for pid, start, finish in [(None, self.start, self.finish),
                                   (self.pid, float("nan"), self.finish),
                                   (self.pid, self.finish, self.start)]:
            with mock.patch.object(reports.os, "scandir") as scan:
                output = reports.collect_original_macos_crash(self.executable, pid, start, finish)
                self.assertIn("UNAVAILABLE", output)
                scan.assert_not_called()


class OriginalMacCrashRunnerContracts(unittest.TestCase):
    """Exercise the actual runner entry; mock only the external child/report."""
    def setUp(self):
        import run_native_ui_test
        self.runner = run_native_ui_test
        self.storage = tempfile.TemporaryDirectory()
        self.addCleanup(self.storage.cleanup)
        self.root = Path(self.storage.name)
        self.executable = self.root / "fixture-app"
        self.executable.write_bytes(b"Never executed")
        self.source = self.root / "stuff"
        self.source.mkdir()
        self.output = self.root / "native-output.txt"

    def invoke(self, status, safe=True, *, collector=None, clear_hook=None,
               diagnostic_hook=None, debugger_hook=None):
        markers = "\n".join((
            "native-ui-integration: PASS",
            "icon-lifecycle-native: terminal replacement PASS",
            "icon-lifecycle-native: PASS",
            "native-ui-integration: checkpoint: native icon workers drained",
            "native-ui-integration: checkpoint: native application cleanup completed"))
        def launch(command, **options):
            options["runtime_state"].update(
                pid=63307, started_at=1000.25, finished_at=1040.5, safe_to_release=safe)
            return subprocess.CompletedProcess(command, status, markers.encode())
        evidence = "original-macos-crash: MATCHED original PID 63307; no replay.\nThread 0 CRASHED\n"
        def clear(*arguments):
            if clear_hook is not None:
                clear_hook(arguments[-1])
            return "independent clear completed\n", 0
        def diagnose(*arguments, **options):
            diagnostic_hook(options["runtime_state"])
            return "independent diagnostics completed\n"
        def debug(*arguments, **options):
            debugger_hook(options["runtime_state"])
            return "independent debugger exited with status 0\n"
        arguments = ["native-runner", str(self.executable), str(self.source),
                     "--output", str(self.output)]
        if diagnostic_hook is not None:
            arguments.append("--require-combined-workflows")
        if debugger_hook is not None:
            arguments.append("--macos-crash-debugger")
        console = io.StringIO()
        with mock.patch.object(sys, "argv", arguments), \
                mock.patch.object(sys, "platform", "darwin"), \
                mock.patch.object(self.runner, "isolated_fixture", return_value=nullcontext((self.root, {}))), \
                mock.patch.object(self.runner, "run_captured_process", side_effect=launch) as process, \
                mock.patch.object(self.runner, "required_icon_clear", side_effect=clear), \
                mock.patch.object(self.runner, "combined_failure_diagnostics", side_effect=diagnose), \
                mock.patch.object(self.runner, "macos_crash_diagnostics", side_effect=debug), \
                mock.patch.object(reports, "collect_original_macos_crash", side_effect=collector,
                                  return_value=evidence) as collect, \
                redirect_stdout(console):
            result = self.runner.main()
        self.last_console = console.getvalue()
        self.assertEqual(process.call_count, 1)
        return result, collect, evidence

    def test_exact_original_identity_sidecar_and_failure_are_preserved(self):
        status, collect, evidence = self.invoke(-11)
        self.assertEqual(status, 1)
        collect.assert_called_once_with(self.executable.resolve(), 63307, 1000.25, 1040.5)
        sidecar = self.output.with_name("native-output-original-macos-crash.txt")
        saved = sidecar.read_text(encoding="utf-8")
        self.assertEqual(saved, "Original macOS identity: pid=63307 status=-11 "
                               "started_at=1000.25 finished_at=1040.5\n" + evidence)
        output = self.output.read_text(encoding="utf-8")
        self.assertIn("Native application exited with status -11", output)
        self.assertIn("Original macOS process crash report (no replay)", output)
        self.assertLess(output.index(evidence), output.index("independent clear completed"))
        self.assertNotIn("Final zero-wait scan", output)

    def test_final_scan_keeps_original_identity_after_all_diagnostics_overwrite_state(self):
        absent = "original-macos-crash: UNAVAILABLE (initial report not yet present).\n"
        matched = "original-macos-crash: MATCHED original PID 63307; no replay.\noriginal stack\n"
        events = []
        def collect(*identity, **options):
            events.append(("scan", identity, options))
            return absent if len(events) == 1 else matched
        def overwrite(label, pid):
            def change(state):
                events.append((label,))
                state.update(pid=pid, started_at=2000., finished_at=2040., output=b"replay PASS",
                             safe_to_release=True)
            return change
        status, collector, _ = self.invoke(-11, collector=collect,
            clear_hook=overwrite("clear", 70001), diagnostic_hook=overwrite("diagnostics", 70002),
            debugger_hook=overwrite("debugger", 70003))
        self.assertEqual(status, 1)
        self.assertEqual([event[0] for event in events], ["scan", "clear", "diagnostics", "debugger", "scan"])
        identity = (self.executable.resolve(), 63307, 1000.25, 1040.5)
        self.assertEqual(collector.call_args_list, [mock.call(*identity), mock.call(*identity, timeout=0)])
        saved = self.output.with_name("native-output-original-macos-crash.txt").read_text(encoding="utf-8")
        output = self.output.read_text(encoding="utf-8")
        for retained in (saved, output, self.last_console):
            self.assertIn("pid=63307 status=-11 started_at=1000.25 finished_at=1040.5", retained)
            self.assertLess(retained.index(absent), retained.index("Final zero-wait scan"))
            self.assertIn(matched, retained)
            self.assertNotIn("70003", retained)
        self.assertIn("Native application exited with status -11", output)

    def test_initial_match_skips_final_scan_even_after_debugger_identity_changes(self):
        def overwrite(state):
            state.update(pid=70003, started_at=2000., finished_at=2040.)
        status, collect, _ = self.invoke(-11, debugger_hook=overwrite)
        self.assertEqual(status, 1)
        collect.assert_called_once_with(self.executable.resolve(), 63307, 1000.25, 1040.5)
        self.assertNotIn("Final zero-wait scan", self.last_console)

    def test_unavailable_final_scan_retains_both_absence_results_and_failure(self):
        absent = "original-macos-crash: UNAVAILABLE (no matching report).\n"
        status, collect, _ = self.invoke(-11, collector=lambda *args, **kwargs: absent)
        self.assertEqual(status, 1)
        self.assertEqual(collect.call_count, 2)
        self.assertEqual(collect.call_args.kwargs, {"timeout": 0})
        for path in (self.output, self.output.with_name("native-output-original-macos-crash.txt")):
            text = path.read_text(encoding="utf-8")
            self.assertEqual(text.count(absent), 2)
            self.assertEqual(text.count("Final zero-wait scan"), 1)

    def test_final_scan_error_is_bounded_and_keeps_original_failure(self):
        absent = "original-macos-crash: UNAVAILABLE (initial report not yet present).\n"
        error = PermissionError(13, "Permission denied", "/Users/private person/My Project/report.ips")
        status, collect, _ = self.invoke(-11, collector=[absent, error])
        self.assertEqual(status, 1)
        self.assertEqual(collect.call_count, 2)
        for path in (self.output, self.output.with_name("native-output-original-macos-crash.txt")):
            retained = path.read_text(encoding="utf-8")
            self.assertIn(absent, retained)
            self.assertIn("final original-process scan failed: PermissionError", retained)
            self.assertNotIn("/Users/", retained)
            self.assertNotIn("My Project", retained)
        self.assertIn("Native application exited with status -11", self.last_console)

    def test_partial_write_and_replace_failures_preserve_exact_original_report_bytes(self):
        absent = "original-macos-crash: UNAVAILABLE (initial report not yet present).\n"
        matched = "original-macos-crash: MATCHED original PID 63307; no replay.\noriginal stack\n"
        create_temporary = self.runner.tempfile.NamedTemporaryFile
        replace = self.runner.os.replace
        for target in (self.output.name, "native-output-original-macos-crash.txt"):
            for failure in ("partial-write", "replace"):
                with self.subTest(target=target, failure=failure):
                    retained_path = self.output.parent / target
                    observed = {}
                    def temporary(*args, **kwargs):
                        stream = create_temporary(*args, **kwargs)
                        write = stream.write
                        def partial(content):
                            if (failure == "partial-write" and kwargs["prefix"] == f".{target}." and
                                    "Final zero-wait scan" in content):
                                observed["before"] = retained_path.read_bytes()
                                observed["temporary"] = Path(stream.name)
                                write(content[:32])
                                stream.flush()
                                self.assertEqual(Path(stream.name).read_bytes(), content[:32].encode("utf-8"))
                                raise OSError(28, "No space left", "/Users/private person/My Project/report.txt")
                            return write(content)
                        stream.write = partial
                        return stream
                    def fail_replace(source, destination):
                        if (failure == "replace" and Path(destination).name == target and
                                "Final zero-wait scan" in Path(source).read_text(encoding="utf-8")):
                            observed["before"] = retained_path.read_bytes()
                            observed["temporary"] = Path(source)
                            self.assertIn(matched, Path(source).read_text(encoding="utf-8"))
                            raise PermissionError(13, "Permission denied", "/Users/private person/My Project/report.txt")
                        return replace(source, destination)
                    with mock.patch.object(self.runner.tempfile, "NamedTemporaryFile", side_effect=temporary), \
                            mock.patch.object(self.runner.os, "replace", side_effect=fail_replace):
                        status, collect, _ = self.invoke(-11, collector=[absent, matched])
                    self.assertEqual(status, 1)
                    self.assertEqual(collect.call_count, 2)
                    self.assertEqual(retained_path.read_bytes(), observed["before"])
                    self.assertIn(absent.encode("utf-8"), observed["before"])
                    self.assertFalse(observed["temporary"].exists())
                    self.assertEqual(list(self.root.glob(".*.tmp")), [])
                    self.assertIn("Native application exited with status -11", self.last_console)
                    self.assertIn(absent, self.last_console)
                    self.assertIn(matched, self.last_console)
                    self.assertIn("errno=28" if failure == "partial-write" else "errno=13", self.last_console)
                    self.assertNotIn("/Users/", self.last_console)
                    self.assertNotIn("My Project", self.last_console)

    def test_final_real_report_scan_never_substitutes_a_delayed_replay_report(self):
        reader = reports.collect_original_macos_crash
        # OS report paths are macOS identities even when these reader/runner
        # contracts execute on another host. Assert the runner's real binary
        # argument before translating only the fixture's pathname for parsing.
        report_executable = PurePosixPath("/Applications/fixture-app")
        for include_original in (False, True):
            with self.subTest(include_original=include_original):
                directory = self.root / ("reports-original" if include_original else "reports-replay")
                directory.mkdir()
                def write_report(name, pid, start, finish, mtime):
                    stamp = lambda epoch: datetime.fromtimestamp(epoch, timezone.utc).isoformat()
                    payload = {"pid": pid, "procName": report_executable.name,
                               "procPath": str(report_executable), "procLaunch": stamp(start),
                               "captureTime": stamp(finish), "exception": {"type": "EXC_BAD_ACCESS"},
                               "faultingThread": 0, "threads": [{"frames": [{"symbol": name}]}],
                               "usedImages": []}
                    path = directory / ("fixture-app-" + name + ".ips")
                    path.write_text(json.dumps({"bug_type": "309"}) + "\n" + json.dumps(payload), encoding="utf-8")
                    os.utime(path, (mtime, mtime))
                def delayed(state):
                    state.update(pid=70003, started_at=2000., finished_at=2040.)
                    write_report("replay_stack", 70003, 2000., 2039., 3001.)
                    if include_original:
                        write_report("original_stack", 63307, 1000.5, 1040., 3000.)
                def collect(executable, pid, start, finish, **options):
                    self.assertEqual(executable, self.executable.resolve())
                    return reader(report_executable, pid, start, finish,
                                  report_directories=[directory], timeout=options.get("timeout", 0))
                with mock.patch.object(reports.time, "sleep", side_effect=AssertionError("zero-wait fixture must not sleep")):
                    status, collect_call, _ = self.invoke(-11, collector=collect, debugger_hook=delayed)
                self.assertEqual(status, 1)
                self.assertEqual(collect_call.call_count, 2)
                output = self.output.read_text(encoding="utf-8")
                self.assertIn("examined=0", output)
                self.assertNotIn("replay_stack", output)
                if include_original:
                    self.assertIn("MATCHED original PID 63307", output)
                    self.assertIn("original_stack", output)
                else:
                    self.assertEqual(output.count("original-macos-crash: UNAVAILABLE"), 2)

    def test_success_does_not_collect_original_crash(self):
        status, collect, _ = self.invoke(0)
        self.assertEqual(status, 0)
        collect.assert_not_called()
        self.assertFalse(self.output.with_name("native-output-original-macos-crash.txt").exists())

    def test_uncertain_descendant_exit_does_not_collect(self):
        status, collect, _ = self.invoke(-11, safe=False)
        self.assertEqual(status, 1)
        collect.assert_not_called()
        self.assertIn("status -11", self.output.read_text(encoding="utf-8"))

    def test_sidecar_write_failure_keeps_original_status_and_hides_private_path(self):
        original_replace = self.runner.os.replace
        def replace(source, destination):
            if Path(destination).name.endswith("-original-macos-crash.txt"):
                raise PermissionError(13, "Permission denied", "/Users/private person/My Project/report.txt")
            return original_replace(source, destination)
        with mock.patch.object(self.runner.os, "replace", side_effect=replace):
            status, collect, _ = self.invoke(-11)
        self.assertEqual(status, 1)
        collect.assert_called_once()
        output = self.output.read_text(encoding="utf-8")
        self.assertIn("Native application exited with status -11", output)
        self.assertIn("PermissionError", output)
        self.assertNotIn("/Users/", output)
        self.assertNotIn("private person", output)
        self.assertNotIn("My Project", output)


if __name__ == "__main__":
    unittest.main()
