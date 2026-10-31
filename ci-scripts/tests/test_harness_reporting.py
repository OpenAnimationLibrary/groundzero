"""CI-only actual-entry contracts for reporting early Python harness failures."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from urllib.parse import unquote_to_bytes


RUNNER = Path(__file__).resolve().parents[1] / "run_harness_tests.py"


class HarnessReportingContracts(unittest.TestCase):
    def invoke(self, body, pattern="test_fixture.py"):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tests = root / "tests"; tests.mkdir()
            (tests / "test_fixture.py").write_text(body, encoding="utf-8")
            report = root / "reports" / "contracts.txt"
            environment = dict(os.environ, PYTHONIOENCODING="cp1252:strict", PYTHONUTF8="0")
            result = subprocess.run([sys.executable, str(RUNNER), "--directory", str(tests),
                                     "--pattern", pattern, "--report", str(report)],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    env=environment, timeout=20, check=False)
            return result.returncode, result.stdout, report.read_bytes()

    def test_actual_failure_and_error_each_keep_their_named_traceback(self):
        status, output, report = self.invoke('''import unittest
class Fixture(unittest.TestCase):
    def test_failure(self): self.fail('original dump missing: 描画 50%')
    def test_error(self): raise RuntimeError('exact child evidence missing')
''')
        self.assertEqual(status, 1)
        annotations = [line for line in output.splitlines() if line.startswith(b"::error ")]
        self.assertEqual(len(annotations), 2)
        self.assertTrue(any(b"test_failure::" in line and
                            "AssertionError: original dump missing: 描画 50%25".encode() in line
                            for line in annotations))
        self.assertTrue(any(b"test_error::" in line and b"RuntimeError: exact child evidence missing" in line
                            for line in annotations))
        self.assertIn("original dump missing: 描画 50%".encode(), report)
        self.assertIn(b"FAILED (failures=1, errors=1)", report)

    def test_success_has_no_error_annotation_and_empty_discovery_fails(self):
        source = "import unittest\nclass Fixture(unittest.TestCase):\n    def test_ok(self): pass\n"
        status, output, report = self.invoke(source)
        self.assertEqual(status, 0)
        self.assertNotIn(b"::error", output)
        self.assertIn(b"Ran 1 test", report)
        status, output, report = self.invoke(source, "test_missing.py")
        self.assertEqual(status, 1)
        self.assertIn(b"No tests matched test_missing.py", output)

    def test_discovery_and_test_body_streams_are_retained(self):
        status, output, report = self.invoke('''import sys, unittest
print('discovery output: 描画')
class Fixture(unittest.TestCase):
    def test_streams(self):
        print('body stdout: 動画')
        print('body stderr: 🎨', file=sys.stderr)
''')
        self.assertEqual(status, 0)
        for text in ('discovery output: 描画', 'body stdout: 動画', 'body stderr: 🎨'):
            self.assertIn(text.encode(), output)
            self.assertIn(text.encode(), report)

    def test_subtest_title_escapes_annotation_properties(self):
        status, output, report = self.invoke('''import unittest
class Fixture(unittest.TestCase):
    def test_sub(self):
        with self.subTest(case='draw: frame,50%'): self.fail('actual subtest assertion')
''')
        self.assertEqual(status, 1)
        annotation = next(line for line in output.splitlines() if line.startswith(b"::error "))
        self.assertIn(b"draw%3A frame%2C50%25", annotation.split(b"::", 2)[1])
        self.assertIn(b"actual subtest assertion", annotation)

    def test_keyboard_interrupt_remains_cancellation_with_partial_transcript(self):
        status, output, report = self.invoke('''import unittest
class Fixture(unittest.TestCase):
    def test_interrupted(self):
        print('before cancellation', flush=True)
        raise KeyboardInterrupt
''')
        self.assertNotIn(status, (0, 1))
        self.assertIn(b"before cancellation", report)
        self.assertIn(b"KeyboardInterrupt", output)
        self.assertNotIn(b"::error", output)

    def test_unexpected_success_is_a_named_failure(self):
        status, output, report = self.invoke('''import unittest
class Fixture(unittest.TestCase):
    @unittest.expectedFailure
    def test_stale_expectation(self): pass
''')
        self.assertEqual(status, 1)
        self.assertIn(b"test_stale_expectation::Unexpected success", output)
        self.assertIn(b"UNEXPECTED SUCCESS", report)

    def test_long_unicode_failure_is_bounded_without_truncating_the_raw_report(self):
        status, output, report = self.invoke('''import unittest
class Fixture(unittest.TestCase):
    def test_large(self): self.fail('描画' * 4000 + ' exact assertion end')
''')
        self.assertEqual(status, 1)
        annotation = next(line for line in output.splitlines() if line.startswith(b"::error "))
        message = unquote_to_bytes(annotation.split(b"::", 2)[2])
        self.assertLessEqual(len(message), 3500)
        message.decode("utf-8", errors="strict")
        self.assertIn(b"exact assertion end", message)
        self.assertGreater(len(report), 20000)


if __name__ == "__main__":
    unittest.main()
