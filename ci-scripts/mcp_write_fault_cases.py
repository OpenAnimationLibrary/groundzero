"""Fail or crash file writes (package save, PNG export and import) and native edits at each
step, and check that nothing is published, left behind or claimed."""

from __future__ import annotations

import os
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_failure_cases import _fault_worker, _second_worker
from mcp_file_publish_cases import staging_left
from mcp_file_tools import PATH_TOOLS
from mcp_journal_cases import restart, status

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


def _unchanged(case: NativeAcceptance, document: str, revision: str) -> None:
    summary = case.success("scene.summary", {"document_id": document})
    case.assertEqual(summary["revision"], revision)
    case.assertEqual(staging_left(case.workspace), [])


@acceptance_case(worker_only=FAULT_FLAG)
def png_export_failure_never_publishes_or_replaces(case: NativeAcceptance) -> None:
    """A PNG export failing at its final rename is a typed error naming that stage, with no
    rollback claim and nothing at the destination; an existing file is never replaced."""
    _fault_worker(case, "png-publish", "--test-write-fault", "publish")
    export = PATH_TOOLS["raster.export_png"]
    state = export.prepare(case)
    document = state["document"]
    revision = case.revisions[document]
    target = case.workspace / "exported.png"
    error = export.drive(case, state, str(target))["error"]
    case.assertEqual(error["details"].get("cause_code", error["code"]), "io_error", error)
    case.assertEqual((error["details"]["stage"], error["details"]["published"]), ("publish", False))
    case.assertNotIn("roll", (error["message"] + str(error["details"])).lower())
    case.assertFalse(target.exists())
    _unchanged(case, document, revision)
    existing = case.workspace / "existing.png"
    existing.write_bytes(b"a valid earlier asset\n")
    refused = export.drive(case, state, str(existing))
    case.assertIn("error", refused)
    case.assertEqual(existing.read_bytes(), b"a valid earlier asset\n")
    _unchanged(case, document, revision)


@acceptance_case(worker_only=FAULT_FLAG)
def png_import_copy_failure_leaves_nothing(case: NativeAcceptance) -> None:
    """A PNG import whose private copy fails, or whose worker dies during it, adds no drawing,
    keeps the revision and leaves no staged copy once the next worker starts."""
    for mode in ("fail", "crash"):
        with case.subTest(mode=mode):
            name = f"png-copy-{mode}"
            _fault_worker(case, name, "--test-write-fault", f"import:png-copy:{mode}")
            png = PATH_TOOLS["raster.import_png"]
            state = png.prepare(case)
            document = state["document"]
            revision = case.revisions[document]
            source = case.inputs / f"{name}.png"
            png.place(case, state, source)
            if mode == "crash":
                with case.assertRaises(RuntimeError):
                    png.drive(case, state, str(source))
                case.assertEqual(case.worker.process.wait(timeout=30), CRASH_EXIT_CODE)
                _second_worker(case, f"{name}-successor").close()
                case.assertEqual(staging_left(case.workspace), [])
                continue
            error = png.drive(case, state, str(source))["error"]
            case.assertEqual(error["code"], "io_error", error)
            case.assertNotIn("roll", (error["message"] + str(error["details"])).lower())
            _unchanged(case, document, revision)


@acceptance_case(worker_only=FAULT_FLAG)
def silent_native_refusal_is_a_typed_error(case: NativeAcceptance) -> None:
    """A native call that refuses without saying so is caught by checking its effect: the
    write fails with a typed error, the revision stays and nothing is claimed."""
    _fault_worker(case, "set-frame", "--test-write-fault", "native:set-frame")
    document = case.create("Silent refusal")
    level = case.mutate("level.create", document, name="Vector", format="vector")["level_id"]
    revision = case.revisions[document]
    target = {"level_id": level, "drawing_id": {"number": 1, "suffix": ""}}
    error = case.failure(
        "drawing.create", case.write_params(document, {**target, "format": "vector"})
    )
    case.assertEqual(
        (error["code"], error["details"]["reason"]), ("native_error", "silent_refusal")
    )
    case.assertNotIn("roll", (error["message"] + str(error["details"])).lower())
    _unchanged(case, document, revision)
    inspected = case.success("level.inspect", {"document_id": document, "level_id": level})
    case.assertEqual(inspected["drawings"], [])


def _cell_scene(case: NativeAcceptance, columns: int) -> tuple[str, str, list[str]]:
    document = case.create("Refused cells")
    level = case.mutate("level.create", document, name="Ink", format="vector")["level_id"]
    case.mutate(
        "drawing.create",
        document,
        level_id=level,
        drawing_id={"number": 1, "suffix": ""},
        format="vector",
    )
    ids = [
        case.mutate("column.create", document, name=f"C{index}")["column_id"]
        for index in range(columns)
    ]
    return document, level, ids


def _cell_writes(level: str, columns: list[str]) -> dict[str, dict[str, Any]]:
    drawing = {"number": 1, "suffix": ""}
    runs = [{"level_id": level, "drawing_id": drawing, "length": 3}]
    return {
        "column.expose": {
            "column_id": columns[0],
            "level_id": level,
            "drawing_id": drawing,
            "start_frame": 0,
            "length": 3,
        },
        "cells.set": {
            "columns": [{"column_id": c, "start_frame": 0, "runs": runs} for c in columns],
            "overwrite": False,
        },
    }


def _rows_of(case: NativeAcceptance, document: str, column: str) -> list[bool]:
    cells = case.success(
        "column.cells",
        {"document_id": document, "column_id": column, "start_frame": 0, "length": 3},
    )["cells"]
    return [not cell["empty"] for cell in cells]


@acceptance_case(worker_only=FAULT_FLAG)
def a_refused_first_cell_write_is_a_rejection_that_changes_nothing(case: NativeAcceptance) -> None:
    """TXsheet::setCells returns false for a cell it will not take. With that forced on the
    first native write, each tool that writes cells answers native_error naming the column
    and row, as a rejection: no success is claimed, the cells are as they were and the
    revision has not moved."""
    _fault_worker(case, "set-cells", "--test-write-fault", "native:set-cells")
    document, level, columns = _cell_scene(case, 2)
    for tool, params in _cell_writes(level, columns).items():
        with case.subTest(tool=tool):
            revision = case.revisions[document]
            error = case.failure(tool, case.write_params(document, params))
            case.assertEqual(error["code"], "native_error", error)
            case.assertIn("native cell write was refused", error["message"])
            case.assertEqual(error["details"]["column_id"], columns[0])
            case.assertEqual(error["details"]["frame"], 0)
            case.assertNotIn("completed", error["details"])
            _unchanged(case, document, revision)
            for column in columns:
                case.assertEqual(_rows_of(case, document, column), [False] * 3)
    case.assertEqual(case.success("scene.summary", {"document_id": document})["frame_count"], 0)


@acceptance_case(worker_only=FAULT_FLAG)
def a_cell_write_refused_after_another_landed_says_what_landed(case: NativeAcceptance) -> None:
    """When the first column's write lands and the second's is refused, the answer is
    partially_applied with the native_error as its cause, the revision has moved, and the
    details name exactly the write that landed; the column that landed holds its cells."""
    _fault_worker(case, "set-cells-later", "--test-write-fault", "native:set-cells-later")
    document, level, columns = _cell_scene(case, 2)
    revision = case.revisions[document]
    params = _cell_writes(level, columns)["cells.set"]
    error = case.failure("cells.set", case.write_params(document, params))
    case.assertEqual(error["code"], "partially_applied", error)
    case.assertEqual(error["details"]["cause_code"], "native_error")
    case.assertEqual(error["details"]["column_id"], columns[1])
    landed = [{"column_id": columns[0], "start_frame": 0, "length": 3}]
    case.assertEqual(error["details"]["completed"], landed)
    summary = case.success("scene.summary", {"document_id": document})
    case.assertNotEqual(summary["revision"], revision)
    case.assertEqual(_rows_of(case, document, columns[0]), [True] * 3)
    case.assertEqual(_rows_of(case, document, columns[1]), [False] * 3)


def _staging(root: Path) -> list[str]:
    return [name for name in os.listdir(root) if name.startswith(".groundzero-staging-")]


@acceptance_case(worker_only=FAULT_FLAG)
def output_root_publication_crash_windows(case: NativeAcceptance) -> None:
    """A worker killed at each step of a save into an output root leaves its staging
    container; a successor without that root leaves it and reports root_not_granted, and
    one holding the root removes it and reconciles the save."""
    expected = {
        "publish:staged-container": ("not_published", False),
        "publish:copied": ("not_published", False),
        "publish:renamed": ("published", True),
    }
    for point, (kind, present) in expected.items():
        with case.subTest(point=point):
            name = point.replace(":", "-")
            root = case.directory / f"out-{name}"
            root.mkdir()
            granted = ("--output-root", f"deliveries={root.resolve()}")
            restart(case, name, *granted, "--test-write-fault", f"{point}:crash")
            document, _level, _column = case.populated_scene()
            request = case.write_params(document, {"directory": "pkg", "root": "deliveries"})
            with case.assertRaises(RuntimeError):
                case.worker.call("scene.save_copy", request)
            case.assertEqual(case.worker.process.wait(timeout=30), CRASH_EXIT_CODE)
            case.assertEqual(len(_staging(root)), 1)
            restart(case, name + "-without")
            answer = status(case, request["operation_id"])
            case.assertEqual(answer["verdict"]["evidence"]["found"], "root_not_granted")
            case.assertEqual(len(_staging(root)), 1)
            restart(case, name + "-with", *granted)
            answer = status(case, request["operation_id"])
            case.assertEqual(
                (answer["verdict"]["kind"], answer["verdict"]["reconciled"]), (kind, True)
            )
            case.assertEqual(_staging(root), [])
            case.assertEqual((root / "pkg").exists(), present)
            recovery = case.success("journal.status", {})["last_recovery"]
            case.assertGreaterEqual(recovery["removed_containers"], 1)
            if present:
                opened = case.mutate("scene.open", None, path="pkg", root="deliveries")
                case.assertEqual(opened["admission"], "workspace_registry")
