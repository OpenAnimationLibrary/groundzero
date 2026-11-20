"""Camera lifecycle cases: create, select, duplicate, update and remove cameras, with the
camera list read back after a save by the worker's reopen and the editor's loader."""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_sheet_record_cases import save_and_reopen

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

HANDLES = ("camera_id", "parent_id")


def camera_rows(case: NativeAcceptance, document: str) -> list[dict[str, Any]]:
    """Return the cameras of a document without their handles, in native order."""
    listed = case.success(
        "camera.list",
        {"document_id": document, "expected_revision": case.revisions[document]},
    )
    case.assertIsNone(listed["next_offset"])
    return [{k: v for k, v in row.items() if k not in HANDLES} for row in listed["cameras"]]


def _create(case: NativeAcceptance, document: str, name: str, width: int, height: int) -> str:
    return case.mutate(
        "camera.create",
        document,
        name=name,
        width_px=width,
        height_px=height,
        width_in=width / 120,
        height_in=height / 120,
    )["camera"]["camera_id"]


def _reopened_rows(case: NativeAcceptance, document: str, name: str) -> None:
    rows = camera_rows(case, document)
    saved, reopened = save_and_reopen(case, document, name)
    case.assertEqual(camera_rows(case, reopened), rows)
    opened = case.success(
        "scene.open_read_only",
        {
            "path": str(Path(saved["package_directory"]) / "scene.tnz"),
            "project_id": "standalone",
            "operation_id": f"{name}-cameras",
        },
    )
    case.revisions[opened["document_id"]] = opened["revision"]
    case.assertEqual(camera_rows(case, opened["document_id"]), rows)
    case.mutate("scene.discard", opened["document_id"])


def _refused(case: NativeAcceptance, method: str, document: str, code: str, **params: Any) -> dict:
    return case.handler_failure(method, case.write_params(document, params), code)


@acceptance_case
def landscape_output_and_portrait_preview_cameras_reopen(case: NativeAcceptance) -> None:
    """A landscape output camera and a portrait preview camera keep their roles and sizes
    through a save, in both loaders (SCN-10 #1)."""
    document = case.create("Cameras")
    landscape = _create(case, document, "Landscape", 64, 36)
    portrait = _create(case, document, "Portrait", 36, 64)
    case.mutate("camera.select", document, camera_id=landscape, roles=["output"])
    roles = case.mutate("camera.select", document, camera_id=portrait, roles=["preview"])
    case.assertEqual((roles["output_camera_id"], roles["preview_camera_id"]), (landscape, portrait))
    revision = case.revisions[document]
    case.mutate("camera.select", document, camera_id=portrait, roles=["preview"])
    case.assertEqual(case.revisions[document], revision)
    inspected = case.success("camera.inspect", {"document_id": document})
    case.assertEqual((inspected["camera_id"], inspected["width_px"]), (landscape, 64))
    case.assertEqual(
        [row["roles"] for row in camera_rows(case, document)], [[], ["output"], ["preview"]]
    )
    _reopened_rows(case, document, "roles")


def _x_keys(case: NativeAcceptance, document: str, camera: str) -> list:
    return case.success(
        "animation.keys",
        {"document_id": document, "object_id": camera, "channel": "x", "unit": "inch"},
    )["keys"]


@acceptance_case
def duplicated_camera_has_independent_curves(case: NativeAcceptance) -> None:
    """A duplicate of an animated, parented camera keeps the parent and the keys, and later
    edits of the copy leave the source alone (SCN-10 #2)."""
    document = case.create("Camera copy")
    source = case.success("camera.inspect", {"document_id": document})["camera_id"]
    peg = case.mutate("object.create", document, name="Rig", kind="pegbar")["object_id"]
    case.mutate(
        "object.parent",
        document,
        object_id=source,
        parent_id=peg,
        placement_policy="preserve_local",
    )
    keys = [
        {"frame": 0, "value": 0, "interpolation": "linear"},
        {"frame": 10, "value": 1, "interpolation": "linear"},
    ]
    case.mutate(
        "animation.set_keys", document, object_id=source, channel="x", unit="inch", keys=keys
    )
    result = case.mutate(
        "camera.duplicate", document, camera_id=source, name="Copy", parent_policy="same_parent"
    )
    copy = result["camera"]
    case.assertEqual((copy["parent_id"], copy["animated_channels"]), (peg, 1))
    case.assertEqual(_x_keys(case, document, copy["camera_id"]), _x_keys(case, document, source))
    original = _x_keys(case, document, source)
    case.mutate(
        "animation.set_keys",
        document,
        object_id=copy["camera_id"],
        channel="x",
        unit="inch",
        keys=[{"frame": 0, "value": 5, "interpolation": "linear"}],
    )
    case.assertEqual(_x_keys(case, document, source), original)
    loose = case.mutate(
        "camera.duplicate", document, camera_id=source, name="Loose", parent_policy="none"
    )["camera"]
    case.assertIsNone(loose["parent_id"])
    _reopened_rows(case, document, "copies")


@acceptance_case
def camera_interest_rect_and_dpi_are_explicit(case: NativeAcceptance) -> None:
    """DPI follows pixels per inch on each axis; a resolution change never drops an interest
    rectangle silently; names are checked (SCN-10 #3, F1)."""
    document = case.create("Interest")
    camera = case.success("camera.inspect", {"document_id": document})["camera_id"]
    updated = case.mutate(
        "camera.update",
        document,
        camera_id=camera,
        width_px=60,
        height_px=30,
        width_in=1,
        height_in=0.5,
        interest_rect={"x": 10, "y": 5, "width": 20, "height": 10},
    )
    case.assertEqual((updated["dpi_x"], updated["dpi_y"], updated["square_pixels"]), (60, 60, True))
    case.assertEqual(updated["interest_rect"], {"x": 10, "y": 5, "width": 20, "height": 10})
    error = _refused(
        case, "camera.update", document, "invalid_params", camera_id=camera, width_px=64
    )
    case.assertEqual(error["details"]["reason"], "interest_rect_reset")
    error = _refused(
        case,
        "camera.update",
        document,
        "invalid_params",
        camera_id=camera,
        interest_rect={"x": 50, "y": 0, "width": 20, "height": 10},
    )
    case.assertEqual(error["details"]["reason"], "interest_rect_bounds")
    cleared = case.mutate(
        "camera.update", document, camera_id=camera, width_px=64, interest_rect=None
    )
    case.assertIsNone(cleared["interest_rect"])
    stretched = case.mutate("camera.update", document, camera_id=camera, width_in=2)
    case.assertFalse(stretched["square_pixels"])
    error = _refused(
        case,
        "camera.create",
        document,
        "invalid_params",
        name="Camera7",
        width_px=8,
        height_px=8,
        width_in=1,
        height_in=1,
    )
    case.assertEqual(error["details"]["reason"], "reserved_name")
    _create(case, document, "Side", 8, 8)
    _refused(
        case,
        "camera.create",
        document,
        "name_conflict",
        name="Side",
        width_px=8,
        height_px=8,
        width_in=1,
        height_in=1,
    )
    channels = case.success("animation.channels", {"document_id": document, "object_id": camera})
    case.assertNotIn("focus", [row["channel"] for row in channels["channels"]])
    _reopened_rows(case, document, "interest")


@acceptance_case
def removing_the_output_camera_needs_a_replacement(case: NativeAcceptance) -> None:
    """The output camera is removed only with a replacement; the numbers stay packed, so the
    scene reopens with no phantom camera; the removed handle is stale (SCN-10 #4, F2, F3)."""
    document = case.create("Removal")
    first = case.success("camera.inspect", {"document_id": document})["camera_id"]
    middle = _create(case, document, "Middle", 32, 32)
    last = _create(case, document, "Last", 48, 32)
    case.mutate("camera.select", document, camera_id=middle, roles=["output", "preview"])
    error = _refused(
        case, "camera.remove", document, "in_use", camera_id=middle, children_policy="refuse"
    )
    case.assertEqual(error["details"]["reason"], "role")
    error = _refused(
        case,
        "camera.remove",
        document,
        "invalid_params",
        camera_id=last,
        children_policy="refuse",
        replacement={"output": first},
    )
    case.assertEqual(error["details"]["reason"], "replacement")
    result = case.mutate(
        "camera.remove",
        document,
        camera_id=middle,
        children_policy="refuse",
        replacement={"output": last, "preview": first},
    )
    case.assertEqual((result["output_camera_id"], result["preview_camera_id"]), (last, first))
    case.assertEqual(result["cameras_renumbered"], 1)
    case.assertEqual([row["name"] for row in camera_rows(case, document)], ["Camera1", "Last"])
    case.failure("camera.inspect", {"document_id": document, "camera_id": middle})
    case.assertEqual(case.success("camera.inspect", {"document_id": document})["width_px"], 48)
    case.preview(document)
    case.mutate(
        "camera.remove",
        document,
        camera_id=first,
        children_policy="refuse",
        replacement={"preview": last},
    )
    error = _refused(
        case, "camera.remove", document, "in_use", camera_id=last, children_policy="refuse"
    )
    case.assertEqual(error["details"]["reason"], "last_camera")
    _reopened_rows(case, document, "removal")


@acceptance_case
def a_removed_parent_or_camera_leaves_no_usable_handle(case: NativeAcceptance) -> None:
    """A camera under a column keeps working when the column is deleted, now parented as the
    column was; a peg under a camera moves to the camera's parent when the camera goes; the
    deleted object's handle is refused as a parent afterwards (SCN-10 F2)."""
    document, _level, column = case.populated_scene()
    summary = case.success("scene.summary", {"document_id": document})
    column_object = next(
        row["object_id"] for row in summary["columns"] if row["column_id"] == column["column_id"]
    )
    side = _create(case, document, "Side", 32, 32)
    case.mutate(
        "object.parent",
        document,
        object_id=side,
        parent_id=column_object,
        placement_policy="preserve_local",
    )
    inspected = case.success("camera.inspect", {"document_id": document, "camera_id": side})
    case.assertEqual(inspected["parent_id"], column_object)
    case.mutate(
        "column.delete",
        document,
        column_id=column["column_id"],
        children_policy="reattach_to_parent",
    )
    inspected = case.success("camera.inspect", {"document_id": document, "camera_id": side})
    case.assertNotEqual(inspected["parent_id"], column_object)
    stale = case.write_params(
        document,
        {"object_id": side, "parent_id": column_object, "placement_policy": "preserve_local"},
    )
    case.assertEqual(case.failure("object.parent", stale)["code"], "invalid_handle")
    peg = case.mutate("object.create", document, name="Rider", kind="pegbar")["object_id"]
    case.mutate(
        "object.parent",
        document,
        object_id=peg,
        parent_id=side,
        placement_policy="preserve_local",
    )
    error = _refused(
        case, "camera.remove", document, "has_dependents", camera_id=side, children_policy="refuse"
    )
    case.assertEqual(error["details"]["children"], [peg])
    case.mutate("camera.remove", document, camera_id=side, children_policy="reattach_to_parent")
    stale = case.write_params(
        document, {"object_id": peg, "parent_id": side, "placement_policy": "preserve_local"}
    )
    case.assertEqual(case.failure("object.parent", stale)["code"], "invalid_handle")
    objects = {
        o["object_id"]: o
        for o in case.success("scene.summary", {"document_id": document})["objects"]
    }
    case.assertNotIn(side, objects)
    case.assertNotEqual(objects[peg]["parent_id"], side)
