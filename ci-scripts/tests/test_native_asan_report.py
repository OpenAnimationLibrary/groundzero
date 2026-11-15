"""Contracts for the AddressSanitizer report of the native UI run."""

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import native_asan_report as asan

USE_AFTER_FREE = """\
=================================================================
==4242==ERROR: AddressSanitizer: heap-use-after-free on address 0x60300002 at pc 0x55
READ of size 8 at 0x60300002 thread T0
    #0 0x55aa in TTool::getPixelSize() const tool.cpp:289
    #1 0x55bb in ToonzVectorBrushTool::onActivate() toonzvectorbrushtool.cpp:1249

0x60300002 is located 16 bytes inside of 400-byte region
freed by thread T0 here:
    #0 0x7f00 in operator delete(void*)
SUMMARY: AddressSanitizer: heap-use-after-free tool.cpp:289 in TTool::getPixelSize() const
"""

LEAKS = """\
=================================================================
==4243==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 64 byte(s) in 2 object(s) allocated from:
    #0 0x7f11 in operator new(unsigned long)
    #1 0x55cc in makePayload() rasterimagedata.cpp:183

SUMMARY: AddressSanitizer: 64 byte(s) leaked in 2 allocation(s).
"""


class NativeAsanReport(unittest.TestCase):
    def setUp(self):
        self.storage = tempfile.TemporaryDirectory()
        self.addCleanup(self.storage.cleanup)
        self.directory = Path(self.storage.name)

    def run_report(self):
        output = io.StringIO()
        with redirect_stdout(output):
            status = asan.report(self.directory)
        return status, output.getvalue()

    def test_memory_error_fails_and_names_the_faulting_frames(self):
        (self.directory / "asan.4242").write_text(USE_AFTER_FREE)
        status, output = self.run_report()
        self.assertEqual(status, 1)
        self.assertIn("::error title=AddressSanitizer::heap-use-after-free", output)
        self.assertIn("TTool::getPixelSize() const tool.cpp:289", output)
        self.assertIn("ToonzVectorBrushTool::onActivate()", output)
        self.assertNotIn("operator delete", output)

    def test_leaks_are_totalled_without_failing(self):
        (self.directory / "asan.4243").write_text(LEAKS)
        (self.directory / "asan.4244").write_text(LEAKS)
        status, output = self.run_report()
        self.assertEqual(status, 0)
        self.assertNotIn("::error", output)
        self.assertIn(
            "128 bytes in 4 allocations leaked across 2 sanitizer logs", output
        )

    def test_no_logs_passes_and_still_reports_the_leak_total(self):
        (self.directory / "unrelated.txt").write_text(USE_AFTER_FREE)
        status, output = self.run_report()
        self.assertEqual(status, 0)
        self.assertIn("0 bytes in 0 allocations leaked across 0 sanitizer logs", output)

    def test_error_count_is_bounded(self):
        for index in range(asan.MAX_ERRORS + 3):
            (self.directory / f"asan.{index}").write_text(USE_AFTER_FREE)
        status, output = self.run_report()
        self.assertEqual(status, 1)
        self.assertEqual(
            output.count("heap-use-after-free on address"), asan.MAX_ERRORS
        )
        self.assertIn("3 more memory errors are in the retained logs", output)

    def test_message_is_escaped_as_one_workflow_command(self):
        (self.directory / "asan.1").write_text(USE_AFTER_FREE)
        _, output = self.run_report()
        error = next(line for line in output.splitlines() if line.startswith("::error"))
        self.assertIn("%0A", error)
        self.assertEqual(asan.escape("50%\r\n"), "50%25%0D%0A")


if __name__ == "__main__":
    unittest.main()
