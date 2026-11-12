"""CI contracts for page-heap registry ownership and bounded diagnostic capture."""

import subprocess
import sys
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import native_ui_page_heap as diagnostic


def registry_fixture(values):
    registry = Mock()
    registry.KEY_QUERY_VALUE, registry.KEY_SET_VALUE = 1, 2
    registry.KEY_WOW64_64KEY, registry.HKEY_LOCAL_MACHINE = 4, 5
    registry.REG_DWORD, registry.REG_SZ = 4, 1
    registry.CreateKeyEx.return_value = "image-key"

    def query(key, name):
        if name not in values:
            raise FileNotFoundError(name)
        return values[name]

    def write(key, name, reserved, kind, value):
        values[name] = value, kind

    def delete(key, name):
        if name not in values:
            raise FileNotFoundError(name)
        del values[name]

    registry.QueryValueEx.side_effect = query
    registry.SetValueEx.side_effect = write
    registry.DeleteValue.side_effect = delete
    return registry


class PageHeapRegistryContracts(unittest.TestCase):
    def test_preserves_existing_flags_types_and_unrelated_state(self):
        values = {
            "GlobalFlag": ("0x00000100", 1),
            "PageHeapFlags": (8, 4),
            "Debugger": ("existing debugger", 1),
        }
        original = values.copy()
        registry = registry_fixture(values)
        with diagnostic.PageHeapSettings("fixture.exe", [], registry):
            self.assertEqual(values["GlobalFlag"], ("0x02000100", 1))
            self.assertEqual(values["PageHeapFlags"], (11, 4))
            values["OtherTool"] = (17, 4)
        self.assertEqual(values, {**original, "OtherTool": (17, 4)})
        registry.DeleteKeyEx.assert_not_called()
        registry.CloseKey.assert_called_once_with("image-key")

    def test_absent_values_are_removed_even_after_body_failure(self):
        values = {"Debugger": ("existing debugger", 1)}
        registry = registry_fixture(values)
        with self.assertRaisesRegex(RuntimeError, "injected"):
            with diagnostic.PageHeapSettings("fixture.exe", [], registry):
                self.assertEqual(values["GlobalFlag"], (0x02000000, 4))
                raise RuntimeError("injected body failure")
        self.assertEqual(values, {"Debugger": ("existing debugger", 1)})
        registry.DeleteKeyEx.assert_not_called()

    def test_partial_setup_failure_restores_written_values(self):
        values = {"GlobalFlag": (256, 4)}
        registry = registry_fixture(values)
        write = registry.SetValueEx.side_effect

        def fail_second(key, name, reserved, kind, value):
            if name == "PageHeapFlags":
                raise PermissionError("injected setting failure")
            write(key, name, reserved, kind, value)

        registry.SetValueEx.side_effect = fail_second
        with self.assertRaises(PermissionError):
            with diagnostic.PageHeapSettings("fixture.exe", [], registry):
                self.fail("The diagnostic must not start after setup failed")
        self.assertEqual(values, {"GlobalFlag": (256, 4)})

    def test_restore_failure_is_reported_and_other_value_is_restored(self):
        values = {"GlobalFlag": (256, 4), "PageHeapFlags": (8, 4)}
        registry, notes = registry_fixture(values), []
        write = registry.SetValueEx.side_effect

        def reject_one_restore(key, name, reserved, kind, value):
            if name == "PageHeapFlags" and value == 8:
                raise PermissionError("injected restoration failure")
            write(key, name, reserved, kind, value)

        registry.SetValueEx.side_effect = reject_one_restore
        with diagnostic.PageHeapSettings("fixture.exe", notes, registry):
            pass
        self.assertEqual(values["GlobalFlag"], (256, 4))
        self.assertTrue(any("restoration failed: PageHeapFlags" in line for line in notes))
        self.assertFalse(any("values restored;" in line for line in notes))


class PageHeapCaptureContracts(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        self.executable = self.root / "fixture.exe"
        self.executable.write_bytes(b"Never executed")
        self.source = self.root / "stuff"
        self.source.mkdir()
        self.debugger = self.root / "Windows Kits/10/Debuggers/x64/cdb.exe"
        self.debugger.parent.mkdir(parents=True)
        self.debugger.write_bytes(b"Never executed")
        self.args = SimpleNamespace(
            executable=self.executable,
            stuff=self.source,
            symbols=[self.root],
            suite="workflow-switch",
            windows_mesa=None,
            timeout=123,
        )

    @contextmanager
    def fixture(self, source, executable, mesa, *, runtime_state):
        yield self.source, {"_NT_SYMBOL_PATH": "untrusted external symbol source"}

    def test_verifier_stop_uses_one_bounded_cdb_process_and_local_symbols(self):
        values = {}
        registry = registry_fixture(values)
        output = (
            diagnostic.STOP_MARKER + "\nExceptionCode: c0000421\n"
            "fixture!MainWindow::clearWorkflowRooms\n"
        ).encode()
        with (
            patch.dict(diagnostic.os.environ, {"ProgramFiles(x86)": str(self.root)}),
            patch.object(diagnostic, "isolated_fixture", self.fixture),
            patch.object(diagnostic, "run_captured_process") as capture,
        ):
            capture.return_value = subprocess.CompletedProcess([], 0, output)
            report, summary = diagnostic.collect_diagnostic(self.args, registry)
        capture.assert_called_once()
        command = capture.call_args.args[0]
        self.assertEqual(command[0], str(self.debugger))
        self.assertEqual(command[-2:], [str(self.executable), "--test-native-ui"])
        self.assertIn("sxe 0xc0000421", command[command.index("-c") + 1])
        self.assertNotIn("!gflag", command[command.index("-c") + 1])
        self.assertNotIn("!heap", command[command.index("-c") + 1])
        self.assertIn("-netsyms:no", command)
        self.assertEqual(capture.call_args.kwargs["timeout"], 123)
        self.assertNotIn("_NT_SYMBOL_PATH", capture.call_args.kwargs["environment"])
        self.assertIn("clearWorkflowRooms", diagnostic.exception_excerpt(report))
        self.assertIn("not an application acceptance result", summary)
        self.assertEqual(values, {})

    def test_timeout_preserves_flushed_text_and_restores_settings(self):
        values = {"GlobalFlag": (256, 4)}
        registry = registry_fixture(values)

        def timeout(command, **options):
            options["runtime_state"]["safe_to_release"] = False
            raise subprocess.TimeoutExpired(command, 123, output=b"flushed stack bytes")

        with (
            patch.dict(diagnostic.os.environ, {"ProgramFiles(x86)": str(self.root)}),
            patch.object(diagnostic, "isolated_fixture", self.fixture),
            patch.object(diagnostic, "run_captured_process", side_effect=timeout) as capture,
        ):
            report, _ = diagnostic.collect_diagnostic(self.args, registry)
        capture.assert_called_once()
        self.assertIn("flushed stack bytes", report)
        self.assertIn("timed out after 123 seconds", report)
        self.assertIn("retirement unconfirmed", report)
        self.assertEqual(values, {"GlobalFlag": (256, 4)})

    def test_missing_debugger_does_not_change_registry_or_launch_target(self):
        registry = registry_fixture({})
        with (
            patch.dict(diagnostic.os.environ, {}, clear=True),
            patch.object(diagnostic, "run_captured_process") as capture,
        ):
            report, _ = diagnostic.collect_diagnostic(self.args, registry)
        self.assertIn("debugger location is unavailable", report)
        registry.CreateKeyEx.assert_not_called()
        capture.assert_not_called()

    def test_echoed_debugger_command_is_not_an_exception_witness(self):
        output = "0:000> .echo " + diagnostic.STOP_MARKER + "; .exr -1; q\n"
        self.assertIn("No exception stop marker", diagnostic.exception_excerpt(output))


if __name__ == "__main__":
    unittest.main()
