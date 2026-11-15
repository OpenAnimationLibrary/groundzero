"""CI-only contracts for failure diagnosis across GitHub API boundaries."""

import contextlib
import importlib.util
import io
import os
import unittest
from pathlib import Path
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "ci_audit.py"
SPEC = importlib.util.spec_from_file_location("ci_audit", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Cannot load CI auditor from {SCRIPT}")
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)
REPO = "example/animation"


def note(message, level="failure", **fields):
    return {"message": message, "annotation_level": level, "path": ".github", **fields}


def check(identifier, conclusion="success", **fields):
    return {
        "id": identifier,
        "name": f"check {identifier}",
        "status": "completed",
        "conclusion": conclusion,
        "app": {"slug": "external-checker"},
        **fields,
    }


class ClassificationTests(unittest.TestCase):
    def test_successful_checkpoint_words_are_not_network_or_cancellation(self):
        for text in (
            "checkpoint: cancelExportFidelity passed",
            "capture original then capture proposed",
            "icon-lifecycle-native: canceled/accepted New and checked/unchecked Load PASS",
            "worker cancellation and retirement PASS",
        ):
            with self.subTest(text=text):
                self.assertEqual(AUDIT.hint(text)[0], "unknown")

    def test_native_crash_survives_checkpoint_and_source_location_context(self):
        for failure in (
            "Native application exited with status -11",
            "Native application return code: -11",
            "Native application exited with status 3221226529 (0xC0000421)",
            "SIGSEGV",
            "Segmentation fault",
            "heap corruption",
        ):
            with self.subTest(failure=failure):
                text = f"{failure}\ncheckpoint: cancelExportFidelity passed\nframe capture complete"
                self.assertEqual(AUDIT.hint(text, "native_test.cpp:43")[0], "native crash")

    def test_timeout_is_not_assumed_to_be_a_network_failure(self):
        text = "Isolated diagnostic timed out after 120 seconds. drawing 1 capture proposed"
        self.assertEqual(AUDIT.hint(text)[0], "timeout")
        self.assertEqual(AUDIT.hint("Native UI regression: Timeout")[0], "timeout")

    def test_real_network_and_cancellation_annotations_remain_recognized(self):
        for text in (
            "apt-get update failed",
            "Temporary failure resolving archive.ubuntu.com",
            "Connection reset by peer",
            "HTTP 503",
            "Connection timed out",
        ):
            with self.subTest(text=text):
                self.assertEqual(AUDIT.hint(text)[0], "network or package mirror")
        self.assertEqual(AUDIT.hint("The operation was canceled.")[0], "cancelled")
        self.assertNotIn("superseded", AUDIT.hint("The operation was canceled.")[1])

    def test_compiler_diagnostic_without_error_prefix_keeps_its_hint(self):
        self.assertEqual(
            AUDIT.hint("no member named 'draw'", "tool.cpp:23")[0], "compile or link error"
        )

    def test_native_regression_failure_is_not_a_compile_error(self):
        self.assertEqual(AUDIT.hint("[Native UI regression] status 1")[0], "a test failed")

    def test_msvc_compiler_and_linker_diagnostics_require_an_error_code(self):
        failures = (
            r"D:\a\project\deformer_test.cpp(512,36): error C2059: syntax error: 'constant'",
            r"D:\a\project\deformer_test.cpp(513,35): error C2143: missing ')' before ','",
            "externfx_process_test.obj : error LNK2001: unresolved external symbol TParam",
            r"D:\a\project\test.exe : fatal error LNK1120: 12 unresolved externals",
        )
        for text in failures:
            with self.subTest(text=text):
                self.assertEqual(AUDIT.hint(text)[0], "compile or link error")
        for text in (
            "C2059 and LNK2001 diagnostics documented",
            "error C20590: unrelated identifier",
            "error LNK200: truncated identifier",
            "warning C4996: deprecated API",
            r"D:\a\project\capture.cpp(512,36): checkpoint passed",
        ):
            with self.subTest(text=text):
                self.assertEqual(AUDIT.hint(text)[0], "unknown")


class AnnotationTests(unittest.TestCase):
    def test_failure_beyond_first_page_is_not_hidden_by_notices_or_warnings(self):
        pages = [
            [note("cancelExportFidelity passed", "notice") for _ in range(100)],
            [note("apt deprecation", "warning"), note("status -11")],
        ]
        with mock.patch.object(AUDIT, "gh", side_effect=pages) as api:
            self.assertEqual(AUDIT.annotations(REPO, 42), [("", "status -11")])
        self.assertIn("page=2", api.call_args.args[0])

    def test_generic_process_errors_and_cancellations_are_not_suppressed(self):
        rows = [
            note("Process completed with exit code 1."),
            note("The operation was canceled."),
            note("Resource not accessible"),
        ]
        with mock.patch.object(AUDIT, "gh", return_value=rows):
            self.assertEqual(len(AUDIT.annotations(REPO, 42)), 3)

    def test_deduplication_preserves_distinct_errors_with_long_shared_prefixes(self):
        prefix = "checkpoint " * 30
        first, second = note(prefix + "failure A"), note(prefix + "failure B")
        with mock.patch.object(AUDIT, "gh", return_value=[first, first, second]):
            rows = AUDIT.annotations(REPO, 42)
        self.assertEqual(len(rows), 2)
        self.assertTrue(rows[1][1].endswith("failure B"))

    def test_source_location_and_title_are_retained(self):
        rows = [note("no member named draw", path="tool.cpp", start_line=7, title="Build")]
        with mock.patch.object(AUDIT, "gh", return_value=rows):
            self.assertEqual(
                AUDIT.annotations(REPO, 42), [("tool.cpp:7", "[Build] no member named draw")]
            )


class TargetAuditTests(unittest.TestCase):
    def setUp(self):
        environment = mock.patch.dict(os.environ, {"CI_AUDIT_REPO": REPO})
        environment.start()
        self.addCleanup(environment.stop)

    def test_failure_on_second_check_page_prevents_a_false_green(self):
        pages = [
            {"check_runs": [check(i) for i in range(100)]},
            {"check_runs": [check(101, "failure")]},
            [note("Native application exited with status -11")],
        ]
        output = io.StringIO()
        with mock.patch.object(AUDIT, "gh", side_effect=pages) as api:
            with contextlib.redirect_stdout(output):
                status = AUDIT.audit_target("abcdef12")
        self.assertEqual(status, 1)
        self.assertIn("101 checks", output.getvalue())
        self.assertIn("native crash", output.getvalue())
        self.assertFalse(any("actions/jobs" in call.args[0] for call in api.call_args_list))

    def test_actions_steps_use_linked_job_id_instead_of_check_id(self):
        item = check(
            7,
            "failure",
            app={"slug": "github-actions"},
            details_url=f"https://github.com/{REPO}/actions/runs/2/job/88",
        )
        responses = [
            {"check_runs": [item]},
            {"steps": [{"name": "Native fixture", "conclusion": "failure"}]},
            [note("Native UI regression: status -11")],
        ]
        output = io.StringIO()
        with mock.patch.object(AUDIT, "gh", side_effect=responses) as api:
            with contextlib.redirect_stdout(output):
                status = AUDIT.audit_target("abcdef12")
        self.assertEqual(status, 1)
        self.assertIn("failed step: Native fixture", output.getvalue())
        self.assertEqual(api.call_args_list[1].args[0], f"repos/{REPO}/actions/jobs/88")
        self.assertIn("check-runs/7/annotations", api.call_args_list[2].args[0])

    def test_non_actions_or_untrusted_job_links_do_not_abort_annotation_reading(self):
        for slug, url in (
            ("external-checker", f"https://github.com/{REPO}/actions/runs/2/job/88"),
            ("github-actions", "https://example.com/actions/runs/2/job/88"),
            ("github-actions", "https://github.com/other/repo/actions/runs/2/job/88"),
            ("github-actions", None),
        ):
            with self.subTest(slug=slug, url=url):
                item = check(7, "failure", app={"slug": slug}, details_url=url)
                with mock.patch.object(AUDIT, "gh") as api:
                    self.assertEqual(AUDIT.action_job(REPO, item), {})
                api.assert_not_called()

    def test_no_checks_and_pending_checks_are_not_green(self):
        for checks in ([], [check(7, None, status="in_progress")]):
            with self.subTest(checks=checks):
                with mock.patch.object(AUDIT, "gh", return_value={"check_runs": checks}):
                    with contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(AUDIT.audit_target("abcdef12"), 1)

    def test_skipped_and_neutral_checks_are_not_reported_as_passes(self):
        checks = [check(1), check(2, "skipped"), check(3, "neutral")]
        output = io.StringIO()
        with mock.patch.object(AUDIT, "gh", return_value={"check_runs": checks}):
            with contextlib.redirect_stdout(output):
                self.assertEqual(AUDIT.audit_target("abcdef12"), 0)
        text = output.getvalue()
        self.assertRegex(text, r"(?m)^\s+ok\s+check 1$")
        self.assertRegex(text, r"(?m)^\s+skip\s+check 2$")
        self.assertRegex(text, r"(?m)^\s+neutral\s+check 3$")
        self.assertIn("1 passed, 1 skipped, 1 neutral", text)
        self.assertNotIn("Every check passed", text)


class RecentAuditTests(unittest.TestCase):
    def test_recent_limit_stops_without_fetching_unrequested_history(self):
        rows = list(range(20))
        with mock.patch.object(AUDIT, "gh", return_value={"workflow_runs": rows}) as api:
            result = AUDIT.gh_items(
                "repos/example/animation/actions/runs?status=failure", "workflow_runs", 20
            )
        self.assertEqual(result, rows)
        api.assert_called_once()
        self.assertIn("status=failure&per_page=20&page=1", api.call_args.args[0])

    def test_recent_limit_can_cross_a_hundred_runs(self):
        pages = [{"workflow_runs": list(range(100))}, {"workflow_runs": list(range(100, 200))}]
        with mock.patch.object(AUDIT, "gh", side_effect=pages) as api:
            result = AUDIT.gh_items("runs", "workflow_runs", 105)
        self.assertEqual(result, list(range(105)))
        self.assertEqual(api.call_count, 2)

    def test_failed_job_after_page_one_uses_its_check_link_and_all_causes(self):
        run = {"id": 1, "name": "Native", "head_branch": "edition", "created_at": "2026-11-07"}
        job = {
            "id": 8,
            "name": "Native app",
            "conclusion": "failure",
            "check_run_url": f"https://api.github.com/repos/{REPO}/check-runs/99",
        }
        responses = [
            {"workflow_runs": [run]},
            {"jobs": [{"conclusion": "success"} for _ in range(100)]},
            {"jobs": [job]},
            [note("Process completed with exit code 1.") for _ in range(8)]
            + [note("status -11"), note("Isolated diagnostic timed out after 120 seconds")],
        ]
        output = io.StringIO()
        with mock.patch.dict(os.environ, {"CI_AUDIT_REPO": REPO}):
            with mock.patch.object(AUDIT, "gh", side_effect=responses) as api:
                with contextlib.redirect_stdout(output):
                    self.assertEqual(AUDIT.audit_recent(1), 0)
        self.assertIn("Native app: native crash", output.getvalue())
        self.assertIn("Native app: timeout", output.getvalue())
        self.assertIn("check-runs/99/annotations", api.call_args.args[0])


class CommandLineTests(unittest.TestCase):
    def test_invalid_recent_count_and_ambiguous_arguments_are_actionable_errors(self):
        for args in (
            [],
            ["--recent", "0"],
            ["--recent", "-1"],
            ["--recent", "invalid"],
            ["--recent", "2", "edition"],
            ["edition", "extra"],
        ):
            with self.subTest(args=args):
                errors = io.StringIO()
                with contextlib.redirect_stderr(errors):
                    with self.assertRaises(SystemExit) as raised:
                        AUDIT.main(["ci_audit.py", *args])
                self.assertEqual(raised.exception.code, 2)
                self.assertIn("error:", errors.getvalue())
                self.assertNotIn("Traceback", errors.getvalue())

    def test_help_exits_successfully_without_api_access(self):
        with mock.patch.object(AUDIT, "gh") as api:
            with contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    AUDIT.main(["ci_audit.py", "--help"])
        self.assertEqual(raised.exception.code, 0)
        api.assert_not_called()

    def test_malformed_api_json_reports_the_endpoint(self):
        with mock.patch.object(AUDIT, "run", return_value="not JSON"):
            with self.assertRaises(SystemExit) as raised:
                AUDIT.gh("repos/example/animation/check-runs/1")
        self.assertIn(
            "invalid JSON from repos/example/animation/check-runs/1", str(raised.exception)
        )


if __name__ == "__main__":
    unittest.main()
