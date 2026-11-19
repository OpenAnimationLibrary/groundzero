"""Fail or crash a package save at each of its steps and check that nothing is published."""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_failure_cases import _fault_worker, _second_worker
from mcp_file_publish_cases import staging_left

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

CRASH_EXIT_CODE = 86
# Each injection point of scene.save_copy, the stage its error names and the file it names.
PACKAGE_STEPS = (
    ("package:staged", "stage", None),
    ("package:level", "level", "assets/level-0..png"),
    ("package:sounds", "sounds", None),
    ("package:scene", "scene", "scene.tnz"),
    ("package:manifest", "manifest", "manifest.json"),
    ("package:verify", "verify", None),
    ("publish:flush", "flush", None),
    ("publish", "publish", None),
)
FAULT_FLAG = "needs --test-write-fault, which the adapter never passes"


def _save_request(case: NativeAcceptance, point: str, mode: str) -> tuple[str, Path, Any]:
    """Start a worker with the fault armed, build a scene with a level, and name a target."""
    name = point.replace(":", "-") + "-" + mode
    workspace = _fault_worker(case, name, "--test-write-fault", f"{point}:{mode}")
    document, _level, _column = case.populated_scene()
    target = workspace / "package"
    return document, target, case.write_params(document, {"directory": str(target)})


def _assert_nothing_left(case: NativeAcceptance, target: Path) -> None:
    case.assertFalse(target.exists(), "a failed save published its target")
    case.assertEqual(staging_left(case.workspace), [])
    hidden = [path.name for path in case.workspace.iterdir() if path.name.startswith(".mcp")]
    case.assertEqual(hidden, [])


@acceptance_case(worker_only=FAULT_FLAG)
def package_step_failures_name_their_stage(case: NativeAcceptance) -> None:
    """A save failing at any step names that step and file, publishes nothing, claims no
    rollback and leaves the document's revision and the workspace as they were."""
    for point, stage, file in PACKAGE_STEPS:
        with case.subTest(point=point):
            document, target, request = _save_request(case, point, "fail")
            revision = case.revisions[document]
            error = case.failure("scene.save_copy", request)
            details = error["details"]
            published = point.startswith("publish")
            expected = "partially_applied" if published else "package_write_failed"
            case.assertEqual(error["code"], expected, error)
            case.assertEqual(details.get("cause_code", error["code"]), "package_write_failed")
            case.assertEqual(
                (details["stage"], details["file"], details["published"]), (stage, file, False)
            )
            case.assertNotIn("roll", (error["message"] + str(details)).lower())
            _assert_nothing_left(case, target)
            summary = case.success("scene.summary", {"document_id": document})
            case.assertEqual(summary["revision"], revision)
            case.assertEqual(case.failure("scene.save_copy", request)["details"], details)
            _assert_nothing_left(case, target)


@acceptance_case(worker_only=FAULT_FLAG)
def package_step_crashes_publish_nothing(case: NativeAcceptance) -> None:
    """A worker killed at any step of a save publishes nothing, and the next worker on the
    workspace removes everything the dead one staged."""
    for point, _stage, _file in PACKAGE_STEPS:
        with case.subTest(point=point):
            _document, target, request = _save_request(case, point, "crash")
            with case.assertRaises(RuntimeError):
                case.worker.call("scene.save_copy", request)
            case.assertEqual(case.worker.process.wait(timeout=30), CRASH_EXIT_CODE)
            case.assertFalse(target.exists(), "a crashed save published its target")
            successor = _second_worker(case, point.replace(":", "-") + "-successor")
            documents = successor.call("automation.list_documents", {})["result"]["documents"]
            case.assertEqual(documents, [])
            _assert_nothing_left(case, target)
            successor.close()
