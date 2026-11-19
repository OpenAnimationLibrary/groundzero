"""Exercise document-owned native motion paths through the actual worker."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_animation_cases import _pixels_match, _render, _sample, _simple
from mcp_case import acceptance_case

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


def _points(x: float = 2 / 120, y: float = 0) -> list[dict[str, float]]:
    return [{"x": 0, "y": 0}, {"x": x / 2, "y": y / 2}, {"x": x, "y": y}]


def _geometry(points: Sequence[Mapping[str, float]]) -> dict[str, Any]:
    return {
        "unit": "inch",
        "control_points": points,
        "coordinate_policy": "native_roundtrip",
        "max_coordinate_error_inch": 1e-4,
    }


def _target(object_id: str, channel: str = "path_position") -> dict[str, str]:
    return {
        "object_id": object_id,
        "channel": channel,
        "unit": "percent" if channel == "path_position" else "inch",
    }


def _read(case: NativeAcceptance, method: str, document: str, **params: Any) -> dict[str, Any]:
    return case.success(
        method, {"document_id": document, "expected_revision": case.revisions[document], **params}
    )


def _create(case: NativeAcceptance, document: str, name: str = "Motion") -> dict[str, Any]:
    return case.mutate("motion_path.create", document, name=name, **_geometry(_points()))


def _attach(
    case: NativeAcceptance, document: str, object_id: str, path_id: str, aim: bool = False
) -> dict[str, Any]:
    return case.mutate(
        "object.path.attach",
        document,
        object_id=object_id,
        path_id=path_id,
        aim=aim,
        position_policy="use_path_percentage",
        update_position_keys=False,
    )


def _detach(case: NativeAcceptance, document: str, object_id: str) -> dict[str, Any]:
    return case.mutate(
        "object.path.detach", document, object_id=object_id, position_policy="restore_existing_xy"
    )


@acceptance_case
def motion_path_lifecycle_handles_and_receipts(case: NativeAcceptance) -> None:
    """Exercise every path lifecycle tool, shared locks, no-ops and retired handles."""
    document, _level, column = case.populated_scene()
    path = _create(case, document, "Motion \U0001f680")
    path_id = path["path_id"]
    case.assertEqual(path["consumers"], [])
    case.assertLessEqual(path["max_input_error_inch"], 1e-4)
    listed = _read(case, "motion_path.list", document)["paths"]
    case.assertEqual(
        listed,
        [
            {
                "path_id": path_id,
                "name": "Motion \U0001f680",
                "control_point_count": 3,
                "consumer_count": 0,
            }
        ],
    )
    _attach(case, document, column["object_id"], path_id)
    peg = case.mutate("object.create", document, name="Shared", kind="pegbar")
    _attach(case, document, peg["object_id"], path_id)
    inspection = _read(case, "motion_path.inspect", document, path_id=path_id)
    case.assertEqual(len(inspection["consumers"]), 2)
    revision = case.revisions[document]
    same = _attach(case, document, peg["object_id"], path_id)
    case.assertEqual(same["revision"], revision)
    request = case.write_params(document, {"path_id": path_id, "name": "Renamed"})
    renamed = case.success("motion_path.rename", request)
    case.revisions[document] = renamed["revision"]
    case.assertEqual(case.success("motion_path.rename", request), renamed)
    copied = case.mutate("motion_path.duplicate", document, path_id=path_id, name="Independent")
    case.assertNotEqual(copied["path_id"], path_id)
    case.assertEqual(copied["control_points"], renamed["control_points"])
    case.assertEqual(copied["consumers"], [])
    case.mutate("column.update", document, column_id=column["column_id"], locked=True)
    before = _read(case, "motion_path.inspect", document, path_id=path_id)
    params = case.write_params(
        document, {"path_id": path_id, "sharing": "all_attached", **_geometry(_points(0, 4 / 120))}
    )
    case.assertEqual(case.failure("motion_path.update", params)["code"], "locked")
    case.assertEqual(_read(case, "motion_path.inspect", document, path_id=path_id), before)
    case.mutate("column.update", document, column_id=column["column_id"], locked=False)
    case.mutate(
        "motion_path.update",
        document,
        path_id=path_id,
        sharing="all_attached",
        **_geometry(_points(0, 4 / 120)),
    )
    case.assertEqual(
        _read(case, "motion_path.inspect", document, path_id=copied["path_id"])["control_points"],
        copied["control_points"],
    )
    case.assertEqual(
        case.failure("motion_path.delete", case.write_params(document, {"path_id": path_id}))[
            "code"
        ],
        "invalid_params",
    )
    _detach(case, document, peg["object_id"])
    _detach(case, document, column["object_id"])
    revision = case.revisions[document]
    case.assertEqual(_detach(case, document, column["object_id"])["revision"], revision)
    deleted = case.mutate("motion_path.delete", document, path_id=path_id)
    case.assertTrue(deleted["deleted"])
    error = case.failure(
        "motion_path.inspect",
        {
            "document_id": document,
            "expected_revision": case.revisions[document],
            "path_id": path_id,
        },
    )
    case.assertEqual(error["code"], "invalid_handle")


@acceptance_case
def motion_path_unused_geometry_and_name_roundtrip(case: NativeAcceptance) -> None:
    """Preserve unused native path geometry and quoted Unicode names through package reopen."""
    document = case.create()
    name = '中 "quoted" <path> 🚀\nSecond line'
    path = _create(case, document, name)
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "unused-path"))
    reopened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    listed = _read(case, "motion_path.list", reopened)["paths"]
    case.assertEqual(len(listed), 1)
    case.assertEqual(listed[0]["name"], name)
    case.assertNotEqual(listed[0]["path_id"], path["path_id"])
    restored = _read(case, "motion_path.inspect", reopened, path_id=listed[0]["path_id"])
    case.assertEqual(restored["control_points"], path["control_points"])
    case.assertEqual(restored["consumers"], [])
    native_file = Path(saved["package_directory"]) / "scene.tnz"
    original = native_file.read_bytes()
    documents = case.success("automation.list_documents", {})
    native_file.write_bytes(b"<scene><splines><malformed-native-path")
    error = case.failure(
        "scene.open", case.write_params(None, {"path": saved["package_directory"]})
    )
    case.assertEqual(error["code"], "invalid_package")
    case.assertEqual(case.success("automation.list_documents", {}), documents)
    native_file.write_bytes(original)
    recovered = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    case.assertEqual(len(_read(case, "motion_path.list", recovered)["paths"]), 1)


@acceptance_case
def motion_path_animation_render_and_reopen(case: NativeAcceptance) -> None:
    """Compare native path motion and aim with ordinary XY/rotation across saved generations."""
    document, _level, column = case.populated_scene()
    path = _create(case, document)
    target = _target(column["object_id"])
    case.mutate("animation.set_keys", document, **target, keys=[_simple(0, 0), _simple(12, 100)])
    case.assertEqual(_sample(case, document, target, [0, 3.5, 6, 12]), [0, 100 * 3.5 / 12, 50, 100])
    _attach(case, document, column["object_id"], path["path_id"])
    reference, _other_level, other = case.populated_scene()
    end = path["control_points"][-1]["x"]
    expected = []
    for frame in (0, 6, 12):
        case.mutate(
            "animation.set_keys",
            reference,
            **_target(other["object_id"], "x"),
            keys=[_simple(0, end * frame / 12)],
        )
        pixels = _render(case, reference, 0)
        expected.append(pixels)
        _pixels_match(case, _render(case, document, frame), pixels)
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "path-package"))
    reopened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    new_path = _read(case, "motion_path.list", reopened)["paths"][0]["path_id"]
    case.assertNotEqual(new_path, path["path_id"])
    inspected = _read(case, "motion_path.inspect", reopened, path_id=new_path)
    case.assertEqual(inspected["control_points"], path["control_points"])
    new_object = inspected["consumers"][0]["object_id"]
    case.mutate("scene.discard", document)
    for frame, pixels in zip((0, 6, 12), expected, strict=True):
        _pixels_match(case, _render(case, reopened, frame), pixels)
    vertical = case.mutate(
        "motion_path.update",
        reopened,
        path_id=new_path,
        sharing="all_attached",
        **_geometry(_points(0, 2 / 120)),
    )
    _attach(case, reopened, new_object, new_path, True)
    case.mutate(
        "animation.set_keys", reference, **_target(other["object_id"], "x"), keys=[_simple(0, 0)]
    )
    case.mutate(
        "animation.set_keys",
        reference,
        **_target(other["object_id"], "y"),
        keys=[_simple(0, vertical["control_points"][-1]["y"] / 2)],
    )
    case.mutate(
        "animation.set_keys",
        reference,
        object_id=other["object_id"],
        channel="rotation",
        unit="degree",
        keys=[_simple(0, 90)],
    )
    _pixels_match(case, _render(case, reopened, 6), _render(case, reference, 0))
    _detach(case, reopened, new_object)
    case.assertEqual(_sample(case, reopened, _target(new_object), [6]), [50])


@acceptance_case
def motion_path_geometry_percentage_and_revision_rejections(case: NativeAcceptance) -> None:
    """Refuse malformed geometry, precision loss, unsupported percentages and stale reads."""
    document = case.create()
    peg = case.mutate("object.create", document, kind="pegbar", name="Target")
    for controls in (
        _points(0, 0),
        [{"x": 0, "y": 0}, {"x": 2, "y": 0}, {"x": 1, "y": 0}],
        _points()[:2],
        [{"x": 999999.9, "y": 0}, {"x": 999999.95, "y": 0}, {"x": 1000000, "y": 0}],
    ):
        revision = case.revisions[document]
        case.failure(
            "motion_path.create",
            case.write_params(document, {"name": "Invalid", **_geometry(controls)}),
        )
        case.assertEqual(_read(case, "motion_path.list", document)["paths"], [])
        case.assertEqual(
            case.success("scene.summary", {"document_id": document})["revision"], revision
        )
    precision = {"name": "Exact", **_geometry(_points(1.234567891, 0))}
    precision["max_coordinate_error_inch"] = 0
    case.assertEqual(
        case.failure("motion_path.create", case.write_params(document, precision))["code"],
        "invalid_params",
    )
    path = _create(case, document)
    target = _target(peg["object_id"])
    for key in (
        _simple(0, -1),
        _simple(0, 101),
        {**_simple(0, 50), "step": 2},
        {"frame": 0, "value": 50, "interpolation": "ease_in_out", "outgoing": {"frames": 1}},
    ):
        revision = case.revisions[document]
        case.failure("animation.set_keys", case.write_params(document, {**target, "keys": [key]}))
        case.assertEqual(
            case.success("animation.keys", {"document_id": document, **target})["keys"], []
        )
        case.assertEqual(
            case.success("scene.summary", {"document_id": document})["revision"], revision
        )
    _attach(case, document, peg["object_id"], path["path_id"])
    case.assertEqual(
        case.failure(
            "motion_path.inspect",
            {"document_id": document, "expected_revision": "0", "path_id": path["path_id"]},
        )["code"],
        "revision_conflict",
    )
    foreign = case.create()
    case.assertEqual(
        case.failure(
            "motion_path.inspect",
            {
                "document_id": foreign,
                "expected_revision": case.revisions[foreign],
                "path_id": path["path_id"],
            },
        )["code"],
        "invalid_handle",
    )
