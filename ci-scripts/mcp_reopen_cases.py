"""Packages reopen by their registered content, wherever they were moved, in a later worker."""

from __future__ import annotations

import base64
import shutil
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_journal_cases import CRASH_EXIT_CODE, restart, status
from mcp_macro_cases import _chain, _macro
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


def _frames(case: NativeAcceptance, document: str) -> list[bytes]:
    """Three rendered frames of the document, as PNG bytes."""
    frames = []
    for frame in (0, 6, 12):
        job = case.mutate("render.preview", document, frame=frame, width=32, height=32)
        case.assertEqual(case.wait_job(job["job_id"])["status"], "completed")
        artifact = case.success("job.result", {"job_id": job["job_id"]})["artifacts"][0]
        image = case.success("artifact.read", {"artifact_id": artifact["artifact_id"]})
        frames.append(base64.b64decode(image["image"]["data"]))
    return frames


def _without_ids(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            key: _without_ids(item)
            for key, item in value.items()
            # The scene name is the package's file name after any reopen.
            if not key.endswith("_id") and key not in {"revision", "operation_id", "name"}
        }
    if isinstance(value, list):
        return [_without_ids(item) for item in value]
    return value


def _shape(case: NativeAcceptance, document: str) -> Any:
    return _without_ids(case.success("scene.summary", {"document_id": document}))


def _macro_scene(case: NativeAcceptance) -> str:
    document, _artwork, blur, glow = _chain(case)
    _macro(case, document, [glow, blur])
    return document


@acceptance_case
def relocated_package_reopens_in_a_new_worker(case: NativeAcceptance) -> None:
    """A package saved by one worker, moved elsewhere in the workspace and then into an input
    root, opens in a later worker by its registered content, from a private copy, with the
    same structure and pixels; its save is still answered from the journal."""
    document = _macro_scene(case)
    frames, shape = _frames(case, document), _shape(case, document)
    save = case.write_params(document, {"directory": str(case.workspace / "saved")})
    saved = case.success("scene.save_copy", save)
    case.assertIs(saved["registered"], True)
    case.assertRegex(saved["package_id"], "^package:[0-9a-f]{64}$")
    restart(case, "moved")
    moved = case.workspace / "elsewhere" / "moved-package"
    moved.parent.mkdir()
    shutil.move(saved["package_directory"], moved)
    case.assertNotIn(str(case.workspace), (moved / "scene.tnz").read_text(errors="replace"))
    opened = case.mutate("scene.open", None, path=str(moved))
    case.assertEqual(
        (opened["admission"], opened["package_id"], opened["relocated"]),
        ("workspace_registry", saved["package_id"], True),
    )
    case.assertIs(opened["published_by"]["current"], False)
    reopened = opened["document_id"]
    case.assertEqual(_shape(case, reopened), shape)
    case.assertEqual(_frames(case, reopened), frames)
    case.assertEqual(len(list(case.workspace.glob("runtime_*/opened-*"))), 1)
    case.mutate("scene.discard", reopened)
    case.assertEqual(list(case.workspace.glob("runtime_*/opened-*")), [])
    case.assertIs(case.success("scene.save_copy", save)["receipt"]["handles_expired"], True)
    inside = case.inputs / "relocated"
    shutil.move(str(moved), inside)
    info = case.success("scene.package_info", {"path": str(inside)})
    case.assertEqual((info["admission"], info["relocated"]), ("workspace_registry", True))
    again = case.mutate("scene.open", None, path=str(inside / "manifest.json"))
    case.assertEqual(_frames(case, again["document_id"]), frames)


def _refused(case: NativeAcceptance, path: Path) -> dict[str, Any]:
    return case.failure("scene.open", case.write_params(None, {"path": str(path)}))


@acceptance_case
def changed_and_foreign_packages_are_refused(case: NativeAcceptance) -> None:
    """Admission is by registered content: an edited manifest, an edited or extra file, and a
    package another workspace registered are refused and say why; nothing is loaded."""
    document, _level, _column = case.populated_scene()
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "original"))
    original = Path(saved["package_directory"])
    restart(case, "checker")
    copies = {name: case.workspace / name for name in ("copy", "asset", "extra")}
    for path in copies.values():
        shutil.copytree(original, path)
    case.assertEqual(case.mutate("scene.open", None, path=str(copies["copy"]))["relocated"], True)
    asset = next(path for path in (copies["asset"] / "assets").iterdir() if path.is_file())
    asset.write_bytes(asset.read_bytes() + b"\0")
    case.assertEqual(_refused(case, copies["asset"])["code"], "invalid_package")
    (copies["extra"] / "extra.txt").write_text("not listed")
    case.assertEqual(_refused(case, copies["extra"])["code"], "invalid_package")
    manifest = original / "manifest.json"
    manifest.write_bytes(manifest.read_bytes() + b"\n")
    changed = _refused(case, original)
    case.assertEqual(
        (changed["code"], changed["details"]["reason"], changed["details"]["next"]),
        ("unregistered_package", "manifest_changed", "scene.import_editable"),
    )
    foreign = _foreign_package(case)
    case.assertEqual(_refused(case, foreign)["details"]["reason"], "not_registered")
    case.assertEqual(len(case.success("automation.list_documents", {})["documents"]), 1)
    case.assertEqual(len(list(case.workspace.glob("runtime_*/opened-*"))), 1)


def _foreign_package(case: NativeAcceptance) -> Path:
    """A package saved and registered by a worker of another workspace, copied in."""
    other = case.directory / "other-workspace"
    other.mkdir()
    worker = Worker(case.executable, other, case.inputs, case.directory / "other.stderr.log")
    case.addCleanup(worker.close)
    created = worker.call("scene.create", {"name": "Other", "operation_id": "foreign-create"})
    document = created["result"]["document_id"]
    worker.call(
        "level.create",
        {
            "document_id": document,
            "expected_revision": "1",
            "operation_id": "foreign-level",
            "name": "Ink",
            "format": "vector",
        },
    )
    summary = worker.call("scene.summary", {"document_id": document})["result"]
    level = summary["levels"][0]["level_id"]
    params = {"document_id": document, "expected_revision": "2", "operation_id": "foreign-draw"}
    worker.call(
        "drawing.create",
        {
            **params,
            "level_id": level,
            "format": "vector",
            "drawing_id": {"number": 1, "suffix": ""},
        },
    )
    params = {"document_id": document, "expected_revision": "3", "operation_id": "foreign-save"}
    saved = worker.call("scene.save_copy", {**params, "directory": str(other / "foreign")})
    case.assertIn("result", saved, saved)
    worker.close()
    target = case.inputs / "foreign"
    shutil.copytree(other / "foreign", target)
    return target


@acceptance_case(worker_only="needs --test-write-fault, which the adapter never passes")
def crash_during_copy_in_leaves_the_package(case: NativeAcceptance) -> None:
    """A worker killed while copying a package in leaves the package as it was; the next
    worker removes the partial copy, reports the open interrupted and opens it anew."""
    document, _level, _column = case.populated_scene()
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "kept"))
    package = Path(saved["package_directory"])
    before = sorted((path.name, path.stat().st_size) for path in package.rglob("*"))
    restart(case, "copy-in", "--test-write-fault", "reopen:copy:crash")
    request = case.write_params(None, {"path": str(package)})
    try:
        case.worker.call("scene.open", request)
    except RuntimeError:
        pass
    case.assertEqual(case.worker.process.wait(timeout=30), CRASH_EXIT_CODE)
    restart(case, "after-copy-in")
    status(case, request["operation_id"])
    case.assertEqual(list(case.workspace.glob("runtime_*/opened-*")), [])
    case.assertEqual(
        sorted((path.name, path.stat().st_size) for path in package.rglob("*")), before
    )
    answer = status(case, request["operation_id"])
    case.assertEqual(
        (answer["state"], answer["verdict"]["kind"]), ("interrupted", "lost_with_worker")
    )
    case.assertEqual(
        case.mutate("scene.open", None, path=str(package))["admission"], "workspace_registry"
    )


@acceptance_case(worker_only="needs --test-journal-limits, which the adapter never passes")
def expired_registration_says_so(case: NativeAcceptance) -> None:
    """Past the registry's limit the oldest package registration keeps only an expired mark:
    opening that package says registry_expired and points to the import."""
    small = ("--test-journal-limits", "small")
    restart(case, "registry", *small)
    document, _level, _column = case.populated_scene()
    saved = []
    for index in range(6):
        # Each save differs, so each is a package of its own.
        case.mutate("scene.settings", document, frame_rate=12 + index)
        path = str(case.workspace / f"p{index}")
        saved.append(case.mutate("scene.save_copy", document, directory=path))
    for index in range(70):
        case.mutate("scene.settings", document, width_px=32 + index % 2)
    restart(case, "after-registry", *small)
    expired = _refused(case, Path(saved[0]["package_directory"]))
    case.assertEqual(
        (expired["code"], expired["details"]["reason"]),
        ("unregistered_package", "registry_expired"),
    )
    newest = case.mutate("scene.open", None, path=saved[-1]["package_directory"])
    case.assertEqual(newest["admission"], "workspace_registry")
    case.assertLessEqual(case.success("journal.status", {})["retained"]["packages"], 4)
