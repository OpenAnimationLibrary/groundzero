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

ICON_LIFECYCLE_FAILURE = (
    "Failure: native-ui-integration: FAIL: icon lifecycle: "
    "Native scene retirement timer did not finish\n\n"
    "Recent phases:\n"
    "icon-lifecycle-native: Quit Cancel begin\n"
    "icon-lifecycle-native: Quit Cancel decision returned closed=0 "
    "(decision after {cancel} ms, close returned after {close} ms)\n"
    "icon-lifecycle-native: Quit Discard with replacement begin\n"
    "icon-lifecycle-native: Quit Discard with replacement decision returned closed=1 "
    "(decision after 15 ms, close returned after {slow} ms)\n\n"
    "Failure context:\n"
    "native-ui-integration: FAIL: icon lifecycle: Native scene retirement timer did not finish\n\n"
    "Native application exited with status 1 (0x00000001)."
)

WINDOWS_CRASH_REPORT = """Failure: Native application exited with status 1 (0x00000001).

Recent phases:
native-ui-integration: checkpoint: entering native application event loop
native-ui-integration: checkpoint: entered native fixture callback

Failure context:
Crash Reason: EXCEPTION_ACCESS_VIOLATION

==== Backtrace ====
  3> RtlDispatchException [0x7ffea5f262ce] <ntdll.dll>
  4> TModifierTangents::modifyTrack {modifiertangents.cpp:110} <tnztools.dll>
  5> TInputModifier::modifyTracks {inputmanager.cpp:67} <tnztools.dll>
==== End ====

Native application exited with status 1 (0x00000001)."""

SANITIZER_HEAP_USE_AFTER_FREE = (
    "heap-use-after-free on address 0x516000bbd7d8 at pc 0x7fefa5d591a1 bp 0x7ffc708e4330 "
    "sp 0x7ffc708e4320 #0 0x7fefa5d591a0 in TStageObject::getId() const "
    "toonz/sources/toonzlib/tstageobject.cpp:627 #1 0x7fefa79d3c3c in "
    "StageObjectChannelGroup::data(int) const toonz/sources/toonzqt/functiontreeviewer.cpp:1011"
)
SANITIZER_TYPE_MISMATCH = (
    "new-delete-type-mismatch on 0x50200009c390 in thread T0: #0 0x7f5057eb724f in operator "
    "delete(void*, unsigned long) ../../../../src/libsanitizer/asan/asan_new_delete.cpp:172 "
    "#1 0x7f5051e67c04 in FunctionSelection::~FunctionSelection() "
    "toonz/sources/toonzqt/functionselection.cpp:61"
)


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


class NativeCrashClassificationTests(unittest.TestCase):
    def test_windows_structured_exceptions_are_native_crashes(self):
        for text in (
            "Crash Reason: EXCEPTION_ACCESS_VIOLATION",
            "EXCEPTION_STACK_OVERFLOW at 0x00007ff6061f7f57",
            "Unhandled EXCEPTION_ILLEGAL_INSTRUCTION in worker",
            "Exception code: STATUS_STACK_BUFFER_OVERRUN",
            "Last event: Access violation - code c0000005 (first chance)",
            "Stack overflow in thread 3",
            "Native application exited with status 3221225477 (0xC0000005)",
        ):
            with self.subTest(text=text):
                self.assertEqual(AUDIT.hint(text)[0], "native crash")

    def test_crash_handler_report_marks_a_status_1_exit_as_a_crash(self):
        text = "[Native UI regression] " + AUDIT.clean(WINDOWS_CRASH_REPORT)
        self.assertEqual(AUDIT.hint(text)[0], "native crash")
        frames_only = "[Native UI regression] Native application exited with status 1 "
        frames_only += "(0x00000001). 4> TModifierTangents::modifyTrack "
        frames_only += "{modifiertangents.cpp:110} <tnztools.dll>"
        self.assertEqual(AUDIT.hint(frames_only)[0], "native crash")

    def test_timed_out_diagnostic_with_stack_frames_stays_a_timeout(self):
        text = (
            "[Native UI regression - secondary diagnostic: workflow-switch] Failure: Isolated "
            "diagnostic timed out after 120 seconds. Recent phases: native-ui-integration: "
            "checkpoint: workflow rejected-request checks passed Output tail: "
            "9> runArtistWorkflowBenchmark {artistworkflowbenchmark.cpp:167} <OpenToonz.exe>"
        )
        self.assertEqual(AUDIT.hint(text)[0], "timeout")

    def test_exit_status_from_a_signal_is_a_native_crash(self):
        for status in (
            "-6 (0xFFFFFFFA)",
            "-4 (0xFFFFFFFC)",
            "-7",
            "-8",
            "-1073741819",
        ):
            text = f"[Native UI regression] Native application exited with status {status}."
            with self.subTest(status=status):
                self.assertEqual(AUDIT.hint(text)[0], "native crash")
        text = "Independent native diagnostic: workflow-ui exited with status -6 (0xFFFFFFFA)."
        self.assertEqual(AUDIT.hint(text)[0], "native crash")

    def test_plain_failure_exit_without_a_crash_report_is_not_a_crash(self):
        for text in (
            "Native application exited with status 1 (0x00000001).",
            "Native application exited with status 0 (0x00000000).",
            "Native application exited with status -1 (0xFFFFFFFF).",
            "Original timeout CDB exited with status 0.",
            "native-ui-integration: FAIL: icon lifecycle: Native scene retirement timer unfinished",
        ):
            with self.subTest(text=text):
                self.assertNotEqual(AUDIT.hint(text)[0], "native crash")

    def test_native_ui_regression_status_1_without_report_stays_a_test_failure(self):
        text = "[Native UI regression] " + AUDIT.clean(
            ICON_LIFECYCLE_FAILURE.format(cancel=24, close=32, slow=10527)
        )
        self.assertEqual(AUDIT.hint(text)[0], "a test failed")

    def test_posix_signals_other_than_segv_are_native_crashes(self):
        for text in ("SIGABRT", "SIGBUS received", "Terminated by SIGILL", "SIGFPE in worker"):
            with self.subTest(text=text):
                self.assertEqual(AUDIT.hint(text)[0], "native crash")


class OtherClassificationTests(unittest.TestCase):
    def test_sanitizer_reports_are_not_unknown(self):
        for text in (SANITIZER_HEAP_USE_AFTER_FREE, SANITIZER_TYPE_MISMATCH):
            with self.subTest(text=text[:30]):
                self.assertEqual(AUDIT.hint(f"[AddressSanitizer] {text}")[0], "sanitizer error")
        self.assertEqual(
            AUDIT.hint("SUMMARY: AddressSanitizer: 48 byte(s) leaked in 3 allocation(s).")[0],
            "sanitizer error",
        )
        self.assertEqual(
            AUDIT.hint("tcolorstyles.cpp:82:14: runtime error: load of misaligned address")[0],
            "sanitizer error",
        )

    def test_a_test_failure_in_a_sanitizer_job_is_not_a_sanitizer_report(self):
        text = "[Native UI under AddressSanitizer] " + AUDIT.clean(
            ICON_LIFECYCLE_FAILURE.format(cancel=24, close=32, slow=10527)
        )
        self.assertEqual(AUDIT.hint(text)[0], "a test failed")

    def test_fail_marker_and_python_tracebacks_are_test_failures(self):
        for text in (
            "native-ui-integration: FAIL: raster paste: External paste changed its pixels",
            (
                "[__main__.NativeAcceptance.test_movie] Traceback (most recent call last): "
                'File "ci-scripts/mcp_acceptance.py", line 74, in test_movie'
            ),
            "[Native 120-frame shot] AssertionError: Movie ffmpeg failed",
        ):
            with self.subTest(text=text):
                self.assertEqual(AUDIT.hint(text)[0], "a test failed")

    def test_ninja_failed_target_is_a_compile_error(self):
        text = "[Failed build target] FAILED: [code=1] toonz/CMakeFiles/OpenToonz.dir/a.cpp.o"
        self.assertEqual(AUDIT.hint(text)[0], "compile or link error")
        self.assertNotEqual(
            AUDIT.hint("Packaged brush: step failed: command returned 1")[0],
            "compile or link error",
        )


class SignatureTests(unittest.TestCase):
    def sig(self, text, place=""):
        return AUDIT.signature(AUDIT.clean(text), place)

    def test_same_test_message_matches_across_timings_and_platforms(self):
        first = self.sig(
            "[Native UI regression] "
            + ICON_LIFECYCLE_FAILURE.format(cancel=753, close=1000, slow=11241)
        )
        second = self.sig(
            "[Native UI regression] "
            + ICON_LIFECYCLE_FAILURE.format(cancel=24, close=32, slow=10527)
        )
        self.assertEqual(first, second)
        self.assertEqual(
            first,
            "[Native UI regression] native-ui-integration: FAIL: icon lifecycle: "
            "Native scene retirement timer did not finish",
        )

    def test_different_test_messages_differ(self):
        sigs = {
            self.sig(
                "[Native UI regression] Failure: native-ui-integration: FAIL: raster paste: A"
            ),
            self.sig(
                "[Native UI regression] Failure: native-ui-integration: FAIL: raster paste: B"
            ),
            self.sig("[Other check] Failure: native-ui-integration: FAIL: raster paste: A"),
        }
        self.assertEqual(len(sigs), 3)

    def test_crash_signature_names_the_last_phase_and_top_frame(self):
        text = "[Native UI regression] " + WINDOWS_CRASH_REPORT
        self.assertEqual(
            self.sig(text),
            "[Native UI regression] Native application exited with status 1 (0x00000001). "
            "(last phase: native-ui-integration; top frame: TModifierTangents::modifyTrack)",
        )
        moved = text.replace("[0x7ffea5f262ce]", "[0x7ffe11112222]").replace(":110}", ":111}")
        self.assertEqual(self.sig(moved), self.sig(text))

    def test_exit_status_is_part_of_the_signature(self):
        template = "[Native UI regression] Failure: Native application exited with status {}."
        self.assertNotEqual(
            self.sig(template.format("-11 (0xFFFFFFF5)")),
            self.sig(template.format("-6 (0xFFFFFFFA)")),
        )

    def test_numbers_durations_addresses_hashes_and_paths_are_normalised(self):
        first = (
            "[Qt 6 test pythonengine failed] Test project toonz/build Start 47: pythonengine "
            "1/1 Test #47: pythonengine ......***Failed 0.11 sec "
            "heap at 0x7fefa5d591a1 commit 9061ae82f1c3 "
            "/home/runner/work/groundzero/groundzero/toonz/sources/toonzlib/scene.cpp:627 "
            "D:\\a\\groundzero\\toonz\\build\\scene.cpp(512,36) at 2026-11-17T13:39:11Z"
        )
        second = (
            "[Qt 6 test pythonengine failed] Test project toonz/build Start 53: pythonengine "
            "1/1 Test #53: pythonengine ......***Failed 0.59 sec "
            "heap at 0x55aa00112233 commit 415150890a7d "
            "/Users/runner/work/groundzero/groundzero/toonz/sources/toonzlib/scene.cpp:12 "
            "D:\\a\\other\\toonz\\build\\scene.cpp(9,4) at 2026-11-18T01:02:03Z"
        )
        self.assertEqual(self.sig(first), self.sig(second))
        self.assertIn("scene.cpp", self.sig(first))
        self.assertNotIn("home", self.sig(first))
        self.assertNotIn("0.11", self.sig(first))

    def test_ordinary_words_with_slashes_and_identifiers_with_digits_survive(self):
        text = "icon-lifecycle-native: canceled/accepted New on Qt5Core.dll x86_64"
        self.assertEqual(self.sig(text), text)

    def test_generic_wrappers_have_no_signature(self):
        self.assertEqual(self.sig("Process completed with exit code 1."), "")
        self.assertEqual(self.sig("[Windows] Process completed with exit code 8."), "")

    def test_source_location_is_kept_as_a_file_name(self):
        self.assertEqual(
            self.sig("no member named 'draw'", "toonz/sources/tools/tool.cpp:23"),
            "tool.cpp no member named 'draw'",
        )

    def test_sanitizer_report_signature_ignores_addresses(self):
        first = self.sig(f"[AddressSanitizer] {SANITIZER_TYPE_MISMATCH}")
        second = self.sig(
            "[AddressSanitizer] "
            + SANITIZER_TYPE_MISMATCH.replace("0x50200009c390", "0x5020000a0430").replace(
                "0x7f5057eb724f", "0x7f2092cb724f"
            )
        )
        self.assertEqual(first, second)
        self.assertIn("FunctionSelection::~FunctionSelection()", first)

    def test_repeated_search_path_listing_is_collapsed(self):
        text = "[Qt 6 test deformer failed] Could not find executable deformer_test Looked in: "
        listing = self.sig(text + "deformer_test " * 3 + "Debug")
        self.assertEqual(listing, self.sig(text + "deformer_test " * 9 + "Debug"))

    def test_long_messages_are_cut_at_a_word(self):
        result = self.sig("[Build] " + " ".join(f"word{i}x" for i in range(100)))
        self.assertLessEqual(len(result), AUDIT.SIGNATURE_LENGTH + 4)
        self.assertRegex(result, r" word\d+x \.\.\.$")


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

    def recent_output(self, runs, job_notes):
        """Run audit_recent over runs that each have one failed job with the given notes."""
        responses = [{"workflow_runs": runs}]
        for index, notes in enumerate(job_notes):
            job = {
                "id": index,
                "name": "Windows" if index % 2 else "macOS",
                "conclusion": "failure",
                "check_run_url": f"https://api.github.com/repos/{REPO}/check-runs/{index + 90}",
            }
            responses += [{"jobs": [job]}, notes]
        output = io.StringIO()
        with (
            mock.patch.dict(os.environ, {"CI_AUDIT_REPO": REPO}),
            mock.patch.object(AUDIT, "gh", side_effect=responses),
            contextlib.redirect_stdout(output),
        ):
            self.assertEqual(AUDIT.audit_recent(len(runs)), 0)
        return output.getvalue()

    def test_same_failure_on_two_branches_is_listed_as_a_recurring_signature(self):
        runs = [
            {"id": 1, "name": "Checks", "head_branch": "fix/a", "created_at": "2026-11-12"},
            {"id": 2, "name": "Checks", "head_branch": "fix/b", "created_at": "2026-11-17"},
            {"id": 3, "name": "Checks", "head_branch": "fix/b", "created_at": "2026-11-18"},
        ]
        icon = ICON_LIFECYCLE_FAILURE
        notes = [
            [note(icon.format(cancel=753, close=1000, slow=11241), title="Native UI regression")],
            [note(icon.format(cancel=24, close=32, slow=10527), title="Native UI regression")],
            [
                note(
                    "Failure: native-ui-integration: FAIL: raster paste",
                    title="Native UI regression",
                )
            ],
        ]
        text = self.recent_output(runs, notes)
        self.assertIn("Failure signatures that recur across runs:", text)
        self.assertRegex(
            text,
            r"(?m)^\s+2x  \[Native UI regression\] native-ui-integration: FAIL: icon lifecycle: "
            r"Native scene retirement timer did not finish$",
        )
        self.assertIn("2 branch(es), latest 2026-11-17; jobs: Windows, macOS", text)
        self.assertNotIn("raster paste", text.split("Failure signatures")[1])
        self.assertIn("Checks / macOS: a test failed", text)

    def test_windows_access_violation_is_grouped_as_a_native_crash(self):
        runs = [
            {"id": 1, "name": "Windows Build", "head_branch": "x", "created_at": "2026-11-16"},
        ]
        crash = WINDOWS_CRASH_REPORT
        text = self.recent_output(runs, [[note(crash, title="Native UI regression")]])
        self.assertIn("Windows Build / macOS: native crash", text)
        self.assertNotIn("unknown", text)

    def test_signatures_in_one_run_only_do_not_recur(self):
        runs = [{"id": 1, "name": "Checks", "head_branch": "x", "created_at": "2026-11-16"}]
        notes = [[note("one failure"), note("another failure"), note("one failure")]]
        text = self.recent_output(runs, notes)
        self.assertIn("none: every distinct failure message appears in one run only", text)

    def test_generic_wrapper_annotations_do_not_form_a_recurring_signature(self):
        runs = [
            {"id": 1, "name": "Checks", "head_branch": "a", "created_at": "2026-11-15"},
            {"id": 2, "name": "Checks", "head_branch": "b", "created_at": "2026-11-16"},
        ]
        notes = [[note("Process completed with exit code 1.")]] * 2
        text = self.recent_output(runs, notes)
        self.assertIn("none: every distinct failure message", text)


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
