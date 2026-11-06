"""CI-only contracts for annotations on a redirected Windows code-page stream."""
import io
import json
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import unquote_to_bytes
import xml.etree.ElementTree as ET


SCRIPTS = Path(__file__).resolve().parents[1]


def invoke(script, arguments):
    """Run the real entry point with the strict stream that failed in CI."""
    raw = io.BytesIO()
    stream = io.TextIOWrapper(raw, encoding="cp1252", errors="strict",
                              write_through=True)
    status = 0
    try:
        with patch.object(sys, "stdout", stream), \
                patch.object(sys, "argv", [str(script), *map(str, arguments)]):
            try:
                runpy.run_path(str(script), run_name="__main__")
            except SystemExit as error:
                status = error.code
        stream.flush()
        return status, raw.getvalue()
    finally:
        stream.detach()


class RegressionReportingTest(unittest.TestCase):
    def test_cel_report_uses_own_outcome_not_unrelated_native_failure(self):
        original = ("100% tests passed, 0 tests failed out of 34\n"
                    "Later native-ui-integration: FAIL: unrelated drawing\n").encode("utf-8")
        for outcome in ("success", "skipped", "cancelled"):
            with self.subTest(outcome=outcome), tempfile.TemporaryDirectory() as directory:
                report = Path(directory) / "cel-output.txt"
                report.write_bytes(original)
                status, output = invoke(SCRIPTS / "report_cel_tests.py",
                                        [report, "--outcome", outcome])
                self.assertEqual((status, output), (0, b""))
                self.assertEqual(report.read_bytes(), original)
                # A skipped command is not required to have produced a report.
                report.unlink()
                status, output = invoke(SCRIPTS / "report_cel_tests.py",
                                        [report, "--outcome", outcome])
                self.assertEqual((status, output), (0, b""))

    def test_failed_cel_outcome_preserves_unicode_and_successful_test_summary(self):
        original = ("100% tests passed, 0 tests failed out of 34\n"
                    "runner failed after tests: 動画 🎨 50%\n").encode("utf-8") + b"invalid: \xff\n"
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "cel-output.txt"
            report.write_bytes(original)
            status, output = invoke(SCRIPTS / "report_cel_tests.py",
                                    [report, "--outcome", "failure"])
            self.assertEqual(status, 0)  # Keep the original command's failure status.
            self.assertEqual(report.read_bytes(), original)
        self.assertTrue(output.startswith(
            b"::error title=Cel effect regression::Cel test command failed.%0A"))
        self.assertIn(b"100%25 tests passed, 0 tests failed out of 34", output)
        self.assertIn("runner failed after tests: 動画 🎨 50%25%0A".encode("utf-8"), output)
        self.assertIn("invalid: �%0A".encode("utf-8"), output)

    def test_failed_cel_command_without_output_still_has_error_annotation(self):
        for contents in (None, b""):
            with self.subTest(contents=contents), tempfile.TemporaryDirectory() as directory:
                report = Path(directory) / "未作成.txt"
                if contents is not None:
                    report.write_bytes(contents)
                status, output = invoke(SCRIPTS / "report_cel_tests.py",
                                        [report, "--outcome", "failure"])
                self.assertEqual(status, 0)
                self.assertTrue(output.startswith(b"::error title=Cel effect regression::"))
                self.assertIn(b"unavailable" if contents is None else b"empty output report", output)

    def test_failed_ctest_keeps_raw_output_report_and_failure_status(self):
        diagnostic = "thumbnail 動画.pli failed: 🎨 50%\nsecond line"
        raw_child = b"unchanged child bytes: \xff\r\n"
        for child_status in (0, 8, -11):
            with self.subTest(child_status=child_status), \
                    tempfile.TemporaryDirectory() as directory:
                report = Path(directory) / "ui-tests.xml"
                root = ET.Element("testsuite")
                case = ET.SubElement(root, "testcase", name="ui_shellthumbnail")
                ET.SubElement(case, "failure", message="native failure")
                ET.SubElement(case, "system-out").text = diagnostic
                original = ET.tostring(root, encoding="utf-8", xml_declaration=True)
                result = subprocess.CompletedProcess(["ctest"], child_status)

                def ctest(command, **kwargs):
                    if "--show-only=json-v1" in command:
                        return subprocess.CompletedProcess(command, 0, json.dumps(
                            {"tests": [{"name": "ui_shellthumbnail"}]}))
                    self.assertEqual(kwargs, {}, "child output must remain inherited")
                    self.assertIn("--output-on-failure", command)
                    self.assertIn("--no-tests=error", command)
                    self.assertEqual(command[command.index("--output-junit") + 1],
                                     str(report))
                    report.write_bytes(original)
                    sys.stdout.buffer.write(raw_child)
                    return result

                with patch.object(subprocess, "run", side_effect=ctest) as run:
                    status, output = invoke(SCRIPTS / "run_regression_tests.py",
                                            ["--report", report, "ui_shellthumbnail"])
                self.assertEqual(run.call_count, 2)
                self.assertEqual(status, 1)  # The existing aggregate failure contract.
                self.assertEqual(result.returncode, child_status)
                self.assertEqual(report.read_bytes(), original)
                self.assertTrue(output.startswith(raw_child))
                self.assertIn(("::error::ui_shellthumbnail failed or skipped%0A" +
                               diagnostic.replace("%", "%25").replace("\n", "%0A"))
                              .encode("utf-8"), output)
                self.assertNotIn(b"Regression runner failed", output)

    def test_success_is_still_success_and_missing_unicode_test_still_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "success.xml"

            def ctest(command, **kwargs):
                if "--show-only=json-v1" in command:
                    return subprocess.CompletedProcess(command, 0,
                        '{"tests":[{"name":"ui_success"}]}')
                report.write_bytes(b'<testsuite><testcase name="ui_success"/></testsuite>')
                return subprocess.CompletedProcess(command, 0)

            with patch.object(subprocess, "run", side_effect=ctest):
                status, output = invoke(SCRIPTS / "run_regression_tests.py",
                                        ["--report", report, "ui_success"])
            self.assertEqual((status, output), (0, b""))
            with patch.object(subprocess, "run", side_effect=ctest) as run:
                status, output = invoke(SCRIPTS / "run_regression_tests.py", ["ui_描画"])
            self.assertEqual(status, 1)
            self.assertEqual(run.call_count, 1)
            self.assertIn("Required tests were not registered: ui_描画".encode("utf-8"), output)

    def test_launch_exception_reports_unicode_and_unencodable_path_surrogate(self):
        with patch.object(subprocess, "run", side_effect=OSError("描画\udcff unavailable")):
            status, output = invoke(SCRIPTS / "run_regression_tests.py", ["ui_example"])
        self.assertEqual(status, 1)
        self.assertIn("Regression runner failed: 描画".encode("utf-8"), output)
        self.assertIn(b"\\udcff unavailable", output)

    def test_reporter_preserves_unicode_escaping_and_raw_report(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "test-output.txt"
            original = "native: FAIL: 動画.pli 🎨 50%\n".encode("utf-8") + b"invalid: \xff\n"
            report.write_bytes(original)
            status, output = invoke(SCRIPTS / "report_ui_tests.py",
                                    [report, "描画,50%"])
            self.assertEqual(status, 0)
            self.assertEqual(report.read_bytes(), original)
        self.assertTrue(output.startswith("::error title=描画%2C50%25::".encode("utf-8")))
        self.assertIn("native: FAIL: 動画.pli 🎨 50%25%0A".encode("utf-8"), output)
        self.assertIn("invalid: �%0A".encode("utf-8"), output)

    def test_native_timeout_remains_primary_before_failed_isolated_diagnostic(self):
        original = (
            "vector-width-context-native: Undo/Redo after native PLI write 1\n"
            "PulseAudioService: pa_context_connect() failed\n"
            "Native application integration test timed out after 600 seconds.\n"
            "\nIndependent diagnostics; original all-workflows result unchanged:\n"
            "combined-cast-vector=FAIL (status 1)\ncombined-timing=PASS (status 0)\n"
            "\n--- Independent native diagnostic: combined-cast-vector ---\n"
            "cast-vector-native: checkpoint: rejecting incompatible consolidation\n"
            "native-ui-integration: FAIL: consolidation 動画 🎨 50%\n"
            "Independent native diagnostic: combined-cast-vector exited with status 1 (0x00000001).\n"
            "The original all-workflows failure remains authoritative.\n"
            "\n--- Independent native diagnostic: combined-timing ---\n"
            "Expected failure dialog: FAIL text is part of a successful fixture\n"
            "function-timing-native: PASS\nnative-ui-diagnostic: PASS combined-timing\n"
            "Independent native diagnostic: combined-timing exited with status 0 (0x00000000).\n"
            "The original all-workflows failure remains authoritative.\n"
        ).encode("utf-8")
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "native-output.txt"
            report.write_bytes(original)
            status, output = invoke(SCRIPTS / "report_ui_tests.py", [report])
            self.assertEqual(report.read_bytes(), original)
        self.assertEqual(status, 0)  # Reporting must not replace the runner status.
        annotations = output.splitlines()
        self.assertEqual(len(annotations), 2)
        self.assertTrue(annotations[0].startswith(
            b"::error title=Native UI regression::Failure: Native application integration test timed out after 600 seconds."))
        self.assertIn(b"Undo/Redo after native PLI write 1", annotations[0])
        self.assertNotIn(b"consolidation", annotations[0])
        self.assertNotIn(b"combined-timing", output)
        self.assertTrue(annotations[1].startswith(
            b"::error title=Native UI regression - secondary diagnostic: combined-cast-vector::Failure: native-ui-integration: FAIL:"))
        self.assertIn("consolidation 動画 🎨 50%25".encode("utf-8"), annotations[1])

    def test_ordinary_native_failure_stays_primary_before_secondary_timeout(self):
        original = (
            "native-ui-integration: checkpoint: native drawing\n"
            "native-ui-integration: FAIL: original drawing assertion\n"
            "Native application exited with status 1 (0x00000001).\n"
            "\nIndependent diagnostics; original all-workflows result unchanged:\n"
            "cast-workflow=FAIL (status 1)\n"
            "\n--- Independent native diagnostic: cast-workflow ---\n"
            "Isolated diagnostic timed out after 120 seconds.\n"
            "Independent native diagnostic: cast-workflow exited with status 1 (0x00000001).\n"
            "The original all-workflows failure remains authoritative.\n"
        ).encode("utf-8")
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "native-output.txt"
            report.write_bytes(original)
            status, output = invoke(SCRIPTS / "report_ui_tests.py", [report])
            self.assertEqual(report.read_bytes(), original)
        self.assertEqual(status, 0)
        annotations = output.splitlines()
        self.assertEqual(len(annotations), 2)
        self.assertIn(b"::Failure: native-ui-integration: FAIL: original drawing assertion", annotations[0])
        self.assertNotIn(b"timed out", annotations[0])
        self.assertIn(b"secondary diagnostic: cast-workflow::Failure: Isolated diagnostic timed out after 120 seconds.", annotations[1])

    def test_native_exit_status_outranks_audio_xcb_and_passing_lifecycle_noise(self):
        for native_status in (-11, 17, 3221225477):
            with self.subTest(native_status=native_status), tempfile.TemporaryDirectory() as directory:
                failure = (f"Native application exited with status {native_status} "
                           f"(0x{native_status & 0xffffffff:08X}).")
                original = ("PulseAudioService: pa_context_connect() failed\n"
                    "vector-morph-caller: PLI reload matches exact stored geometry\n" + failure + "\n"
                    "\n--- Required reviewed-clear lifecycle (fresh process) ---\n"
                    "PulseAudioService: pa_context_connect() failed\n"
                    "qt.qpa.xcb: QXcbConnection: XCB error: 8 (BadMatch)\n"
                    "icon-lifecycle-native: terminal reviewed-clear PASS\n"
                    "Required reviewed-clear lifecycle: PASS (status 0).\n" + "描画 🎨 noise\n" * 1500).encode()
                report = Path(directory) / "native-output.txt"
                report.write_bytes(original)
                status, output = invoke(SCRIPTS / "report_ui_tests.py", [report])
                self.assertEqual(status, 0)
                self.assertEqual(report.read_bytes(), original)
                annotations = output.splitlines()
                self.assertEqual(len(annotations), 1)
                self.assertTrue(annotations[0].startswith(
                    ("::error title=Native UI regression::Failure: " + failure).encode()))
                body = unquote_to_bytes(annotations[0].split(b"::", 2)[2])
                self.assertLessEqual(len(body), 3500)
                body.decode("utf-8", errors="strict")

    def test_actual_signal_outranks_routine_errors_but_not_first_native_assertion(self):
        for assertion in (False, True):
            with self.subTest(assertion=assertion), tempfile.TemporaryDirectory() as directory:
                first = "native-ui-integration: FAIL: original drawing assertion\n" if assertion else ""
                original = (first + "Thread 1 received signal SIGSEGV, Segmentation fault.\n"
                    "Native application exited with status -11 (0xFFFFFFF5).\n"
                    "qt.qpa.xcb: QXcbConnection: XCB error: 8 (BadMatch)\n"
                    "PulseAudioService: pa_context_connect() failed\n").encode()
                report = Path(directory) / "native-output.txt"
                report.write_bytes(original)
                status, output = invoke(SCRIPTS / "report_ui_tests.py", [report])
                self.assertEqual(status, 0)
                self.assertEqual(report.read_bytes(), original)
                expected = first.strip() if assertion else "Thread 1 received signal SIGSEGV, Segmentation fault."
                self.assertTrue(output.startswith(
                    ("::error title=Native UI regression::Failure: " + expected).encode()))

    def test_first_native_assertion_precedes_lifecycle_and_isolated_failures(self):
        original = (
            "native-ui-integration: checkpoint: checking gradient owner\n"
            "native-ui-integration: FAIL: gradient owner 動画 50% unavailable\n"
            "native-ui-integration: checkpoint: native application event loop returned\n"
            "icon-lifecycle-native: scene retention and reinstatement PASS\n"
            "native-ui-integration: FAIL: icon lifecycle: missing fixture 🎨.pli\n"
            "Native application exited with status 1 (0x00000001).\n"
            "\nIndependent diagnostics; original all-workflows result unchanged:\n"
            "cast-workflow=FAIL (status 1)\n"
            "\n--- Independent native diagnostic: cast-workflow ---\n"
            "native-ui-integration: FAIL: fresh Cast tag acceptance\n"
            "native-ui-integration: checkpoint: native application event loop returned\n"
            "native-ui-integration: FAIL: icon lifecycle: missing fixture 🎨.pli\n"
            "Independent native diagnostic: cast-workflow exited with status 1 (0x00000001).\n"
        ).encode("utf-8")
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "native-output.txt"
            report.write_bytes(original)
            status, output = invoke(SCRIPTS / "report_ui_tests.py", [report])
            self.assertEqual(report.read_bytes(), original)
        self.assertEqual(status, 0)
        annotations = output.splitlines()
        self.assertEqual(len(annotations), 4)
        self.assertTrue(annotations[0].startswith(
            "::error title=Native UI regression::Failure: native-ui-integration: FAIL: gradient owner 動画 50%25 unavailable".encode("utf-8")))
        self.assertTrue(annotations[1].startswith(
            b"::error title=Native UI regression - additional native failure 2::Failure: native-ui-integration: FAIL: icon lifecycle:"))
        self.assertTrue(annotations[2].startswith(
            b"::error title=Native UI regression - secondary diagnostic: cast-workflow::Failure: native-ui-integration: FAIL: fresh Cast tag acceptance"))
        self.assertTrue(annotations[3].startswith(
            b"::error title=Native UI regression - secondary diagnostic: cast-workflow - additional native failure 2::Failure: native-ui-integration: FAIL: icon lifecycle:"))
        for annotation in annotations:
            body = unquote_to_bytes(annotation.split(b"::", 2)[2])
            self.assertLessEqual(len(body), 3500)
            body.decode("utf-8", errors="strict")

    def test_standalone_failure_and_each_secondary_excerpt_remain_bounded_utf8(self):
        for diagnostics in (False, True):
            with self.subTest(diagnostics=diagnostics), tempfile.TemporaryDirectory() as directory:
                primary = "ui_widget: FAIL: ordinary assertion\n" + "描画 noise\n" * 1500
                original = primary
                if diagnostics:
                    original += (
                        "\nIndependent diagnostics; original all-workflows result unchanged:\n"
                        "cast-workflow=FAIL (status -11)\n"
                        "\n--- Independent native diagnostic: cast-workflow ---\n"
                        "Thread 1 received signal SIGSEGV\n" + "動画 noise\n" * 1500 +
                        "Independent native diagnostic: cast-workflow exited with status -11 (0xFFFFFFF5).\n"
                        "The original all-workflows failure remains authoritative.\n")
                report = Path(directory) / "native-output.txt"
                report.write_text(original, encoding="utf-8")
                status, output = invoke(SCRIPTS / "report_ui_tests.py", [report])
                self.assertEqual(status, 0)
                self.assertEqual(report.read_text(encoding="utf-8"), original)
                annotations = output.splitlines()
                self.assertEqual(len(annotations), 2 if diagnostics else 1)
                self.assertIn(b"::Failure: ui_widget: FAIL: ordinary assertion", annotations[0])
                for annotation in annotations:
                    body = unquote_to_bytes(annotation.split(b"::", 2)[2])
                    self.assertLessEqual(len(body), 3500)
                    body.decode("utf-8", errors="strict")
                if diagnostics:
                    self.assertIn(b"secondary diagnostic: cast-workflow::Failure: Thread 1 received signal SIGSEGV", annotations[1])

    def test_missing_unicode_report_is_annotated_without_encoding_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "未作成.txt"
            status, output = invoke(SCRIPTS / "report_ui_tests.py", [report])
        self.assertEqual(status, 0)
        self.assertIn(b"Test output report is unavailable", output)
        self.assertIn("未作成.txt".encode("utf-8"), output)


if __name__ == "__main__":
    unittest.main()
