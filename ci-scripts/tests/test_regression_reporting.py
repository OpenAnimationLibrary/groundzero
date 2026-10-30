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

    def test_missing_unicode_report_is_annotated_without_encoding_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "未作成.txt"
            status, output = invoke(SCRIPTS / "report_ui_tests.py", [report])
        self.assertEqual(status, 0)
        self.assertIn(b"Test output report is unavailable", output)
        self.assertIn("未作成.txt".encode("utf-8"), output)


if __name__ == "__main__":
    unittest.main()
