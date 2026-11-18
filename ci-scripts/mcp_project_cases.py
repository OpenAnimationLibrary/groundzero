"""Exercise the project inspection read: its shape, its bound and what it refuses."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

DOCUMENT_LIMIT = 32
SHAPE = {
    "mode": "private_workspace",
    "arbitrary_scene_open": True,
    "live_gui_mutation": False,
}
READ_ONLY_LIMIT = 4


def _shape(result: dict[str, Any], open_read_only: int) -> dict[str, Any]:
    counts = {"open": open_read_only, "max": READ_ONLY_LIMIT}
    return {**SHAPE, "read_only_documents": counts, "documents": result["documents"]}


def project_inspect_reports_the_private_workspace(case: NativeAcceptance) -> None:
    """Report the fixed capabilities and exactly the documents the worker lists."""
    empty = case.success("project.inspect", {})
    case.assertEqual(empty, _shape({"documents": []}, 0))
    document = case.create()
    inspected = case.success("project.inspect", {})
    case.assertEqual(inspected, _shape(inspected, 0))
    listed = case.success("automation.list_documents", {})["documents"]
    case.assertEqual(inspected["documents"], listed)
    case.assertEqual([entry["document_id"] for entry in listed], [document])
    case.assertEqual(listed[0]["revision"], case.revisions[document])
    case.mutate("scene.settings", document, frame_rate=25)
    case.assertEqual(case.success("project.inspect", {})["documents"][0]["revision"], "2")
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "inspect"))
    opened = case.success(
        "scene.open_read_only",
        case.write_params(None, {"path": saved["scene_path"], "project_id": "standalone"}),
    )
    after = case.success("project.inspect", {})
    case.assertEqual(after, _shape(after, 1))
    access = {row["document_id"]: row["access"] for row in after["documents"]}
    case.assertEqual(access[opened["document_id"]], "read_only")


def project_inspect_is_bounded_and_read_only(case: NativeAcceptance) -> None:
    """Never list more documents than a worker admits, and never change one by reading."""
    documents = [case.create(f"Scene {index}") for index in range(DOCUMENT_LIMIT)]
    listed = case.success("project.inspect", {})["documents"]
    case.assertEqual(sorted(entry["document_id"] for entry in listed), sorted(documents))
    before = case.success("automation.list_documents", {})
    for _repeat in range(3):
        case.assertEqual(case.success("project.inspect", {})["documents"], listed)
    case.assertEqual(case.success("automation.list_documents", {}), before)
    refused = case.failure("scene.create", case.write_params(None, {"name": "Over the limit"}))
    case.assertEqual(refused["code"], "resource_limit", refused)
    case.assertEqual(case.success("project.inspect", {})["documents"], listed)


def project_inspect_refuses_arguments(case: NativeAcceptance) -> None:
    """Accept no arguments: any named property fails the schema and changes nothing."""
    document = case.create()
    before = case.success("project.inspect", {})
    for params in (
        {"document_id": document},
        {"path": "/"},
        {"expected_revision": case.revisions[document]},
    ):
        case.schema_failure("project.inspect", params)
        case.assertEqual(case.success("project.inspect", {}), before)
    case.assertEqual(case.success("scene.summary", {"document_id": document})["revision"], "1")
