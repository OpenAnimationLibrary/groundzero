"""Sheet record cases: what a package saves of columns, cells, cameras and settings, and
that the worker's reopen and the editor's own loader both read it back unchanged."""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_png import decode_png

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

FRAMES = 120
DRAWING_1 = {"number": 1, "suffix": ""}
DRAWING_2 = {"number": 2, "suffix": ""}


def _read(case: NativeAcceptance, method: str, document: str, **params: Any) -> dict[str, Any]:
    return case.success(method, {"document_id": document, **params})


def _cells(case: NativeAcceptance, document: str, column: str, names: dict[str, str]) -> list:
    rows = _read(case, "column.cells", document, column_id=column, start_frame=0, length=FRAMES)
    return [
        None
        if cell["empty"]
        else (names[cell["level_id"]], cell["drawing_id"]["number"], cell["drawing_id"]["suffix"])
        for cell in rows["cells"]
    ]


def _column_row(column: dict[str, Any]) -> tuple:
    fields = ("kind", "name", "position", "visible", "locked", "opacity", "last_frame")
    return tuple(column[field] for field in fields)


def sheet_reads(case: NativeAcceptance, document: str) -> dict[str, Any]:
    """Return the handle-free reads a reopen must preserve: columns, cells, camera, rate."""
    summary = _read(case, "scene.summary", document)
    names = {level["level_id"]: level["name"] for level in summary["levels"]}
    camera = _read(case, "camera.inspect", document)
    keys = _read(
        case, "animation.keys", document, object_id=camera["object_id"], channel="x", unit="inch"
    )
    return {
        "frame_rate": summary["frame_rate"],
        "frame_count": summary["frame_count"],
        "levels": sorted(names.values()),
        "columns": [_column_row(column) for column in summary["columns"]],
        "cells": {
            column["name"]: _cells(case, document, column["column_id"], names)
            for column in summary["columns"]
            if column["kind"] == "level"
        },
        "camera": {key: camera[key] for key in ("width_px", "height_px", "width_in", "height_in")},
        "camera_x_keys": keys["keys"],
        "objects": sorted((row["kind"], row["name"]) for row in summary["objects"]),
    }


def _levels(case: NativeAcceptance, document: str) -> tuple[str, str]:
    ink = case.mutate("level.create", document, name="Ink", format="vector")["level_id"]
    for drawing in (DRAWING_1, DRAWING_2):
        case.mutate("drawing.create", document, level_id=ink, drawing_id=drawing, format="vector")
    paint = case.mutate("level.import", document, path=str(case.fixture), name="Paint", dpi=120)[
        "level_id"
    ]
    return ink, paint


def _rig(case: NativeAcceptance, document: str, column_object: str) -> None:
    root = case.mutate("object.create", document, name="Root", kind="pegbar")["object_id"]
    child = case.mutate("object.create", document, name="Child", kind="pegbar")["object_id"]
    for item, parent in ((child, root), (column_object, child)):
        case.mutate(
            "object.parent",
            document,
            object_id=item,
            parent_id=parent,
            placement_policy="preserve_local",
        )


def camera_move(case: NativeAcceptance, document: str) -> dict[str, Any]:
    """Key the output camera from x 0 at frame 0 to x 1 inch at the last frame."""
    camera = _read(case, "camera.inspect", document)["object_id"]
    keys = [
        {"frame": 0, "value": 0, "interpolation": "linear"},
        {"frame": FRAMES - 1, "value": 1, "interpolation": "linear"},
    ]
    params = {"object_id": camera, "channel": "x", "unit": "inch", "keys": keys}
    case.mutate("animation.set_keys", document, **params)
    return params


def journey_scene(case: NativeAcceptance) -> tuple[str, dict[str, Any]]:
    """Build the W2 journey scene: 120 frames, two levels, a camera move and a peg hierarchy.

    Returns:
        The document and the parameters of its last write, for a receipt retry.
    """
    document = case.mutate(
        "scene.create",
        None,
        name="Journey",
        width_px=64,
        height_px=36,
        width_in=64 / 120,
        height_in=36 / 120,
        frame_rate=24,
    )["document_id"]
    ink, paint = _levels(case, document)
    lines = case.mutate("column.create", document, name="Lines")
    fill = case.mutate("column.create", document, name="Fill")
    on_twos = [
        {"level_id": ink, "drawing_id": DRAWING_1 if start % 4 == 0 else DRAWING_2, "length": 2}
        for start in range(0, FRAMES, 2)
    ]
    held = [{"level_id": paint, "drawing_id": DRAWING_1, "length": FRAMES}]
    case.mutate(
        "cells.set",
        document,
        columns=[
            {"column_id": lines["column_id"], "start_frame": 0, "runs": on_twos},
            {"column_id": fill["column_id"], "start_frame": 0, "runs": held},
        ],
        overwrite=False,
    )
    _rig(case, document, lines["object_id"])
    return document, camera_move(case, document)


def editor_reads(case: NativeAcceptance, package: Path) -> dict[str, Any]:
    """Open the package's scene.tnz with the native loader of the editor and read it."""
    opened = case.success(
        "scene.open_read_only",
        {"path": str(package / "scene.tnz"), "project_id": "standalone", "operation_id": "editor"},
    )
    document = opened["document_id"]
    case.revisions[document] = opened["revision"]
    try:
        return sheet_reads(case, document)
    finally:
        case.mutate("scene.discard", document)


def _without_dropped(reads: dict[str, Any], dropped: int) -> dict[str, Any]:
    count = len(reads["columns"]) - dropped
    kept, gone = reads["columns"][:count], reads["columns"][count:]
    names = {row[1] for row in kept}
    cells = {name: rows for name, rows in reads["cells"].items() if name in names}
    objects = list(reads["objects"])
    for row in gone:
        objects.remove(("column", row[1]))
    return {**reads, "columns": kept, "cells": cells, "objects": objects}


def save_and_reopen(case: NativeAcceptance, document: str, name: str) -> tuple[dict[str, Any], str]:
    """Save a package, reopen it, and require both loaders to read what was saved: the
    document's reads without the trailing empty columns the save reports as dropped."""
    before = sheet_reads(case, document)
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / name))
    expected = _without_dropped(before, len(saved["trailing_empty_columns_dropped"]))
    reopened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    case.assertEqual(sheet_reads(case, reopened), expected)
    case.assertEqual(editor_reads(case, Path(saved["package_directory"])), expected)
    return saved, reopened


@acceptance_case
def journey_scene_reopens_unchanged_in_both_loaders(case: NativeAcceptance) -> None:
    """The W2 journey scene saves, reopens through the worker and the editor's loader, and
    renders the same pixels; a retried write returns its receipt and changes nothing."""
    document, last_write = journey_scene(case)
    spare = case.mutate("column.create", document, name="Spare")["column_id"]
    pixels = decode_png(case.preview(document))
    saved, reopened = save_and_reopen(case, document, "journey")
    case.assertEqual(saved["trailing_empty_columns_dropped"], [spare])
    manifest = case.success("scene.package_info", {"path": saved["package_directory"]})
    for section in ("sheet", "cameras", "scene_properties"):
        case.assertIn(section, manifest)
    case.assertEqual(len(manifest["sheet"]["columns"]), 2)
    case.assertEqual([column["cell_runs"] for column in manifest["sheet"]["columns"]], [60, 1])
    case.assertTrue(all("settings" in level for level in manifest["levels"]))
    case.assertEqual(decode_png(case.preview(reopened)), pixels)
    revision = case.revisions[document]
    retry = {**last_write, "document_id": document, "expected_revision": revision}
    first = case.success("animation.set_keys", {**retry, "operation_id": "journey-retry"})
    again = case.success("animation.set_keys", {**retry, "operation_id": "journey-retry"})
    case.assertEqual(again, first)
    case.assertEqual(case.success("scene.summary", {"document_id": document})["revision"], revision)


@acceptance_case
def sheet_record_keeps_middle_columns_and_reports_dropped_ones(case: NativeAcceptance) -> None:
    """An empty, named, locked column between used columns is saved and reopened with its
    name and lock; empty columns after the last used one are reported as dropped."""
    document, level, first = case.populated_scene()
    middle = case.mutate("column.create", document, name="Gap")["column_id"]
    case.mutate("column.update", document, column_id=middle, locked=True)
    last = case.mutate("column.create", document, name="Last")
    case.mutate(
        "column.expose",
        document,
        column_id=last["column_id"],
        level_id=level["level_id"],
        drawing_id=DRAWING_1,
        start_frame=3,
        length=2,
    )
    case.mutate("column.update", document, column_id=first["column_id"], visible=False)
    named = case.mutate("column.create", document, name="Named")["column_id"]
    locked = case.mutate("column.create", document, name="Locked")["column_id"]
    case.mutate("column.update", document, column_id=locked, locked=True)
    saved, reopened = save_and_reopen(case, document, "middle")
    case.assertEqual(saved["trailing_empty_columns_dropped"], [named, locked])
    columns = case.success("scene.summary", {"document_id": reopened})["columns"]
    case.assertEqual([column["name"] for column in columns], ["Artwork", "Gap", "Last"])
