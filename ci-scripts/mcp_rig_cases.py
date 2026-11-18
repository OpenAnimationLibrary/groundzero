"""Qualify detached hooks, handles, pivots and offset layers through the native worker."""

from __future__ import annotations

from collections.abc import Sequence
from typing import TYPE_CHECKING, Any

from mcp_animation_cases import _pixels_match, _render
from mcp_png import Pixel

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

INCH = 53.33333
CAMERA_INCHES = 2.0
SIZE = 32
PIXELS_PER_INCH = SIZE / CAMERA_INCHES
RED: Pixel = (220, 40, 40, 255)
PRECISION = {"coordinate_policy": "native_roundtrip", "max_coordinate_error_inch": 1e-4}
Point = tuple[float, float]


def _scene(case: NativeAcceptance, name: str) -> str:
    return case.mutate(
        "scene.create",
        None,
        name=name,
        width_px=SIZE,
        height_px=SIZE,
        width_in=CAMERA_INCHES,
        height_in=CAMERA_INCHES,
        frame_rate=24,
    )["document_id"]


def _read(case: NativeAcceptance, method: str, document: str, **params: Any) -> dict[str, Any]:
    return case.success(
        method, {"document_id": document, "expected_revision": case.revisions[document], **params}
    )


def _revision(case: NativeAcceptance, document: str) -> str:
    documents = case.success("automation.list_documents", {})["documents"]
    return next(item["revision"] for item in documents if item["document_id"] == document)


def _refused(
    case: NativeAcceptance, method: str, document: str, code: str, **params: Any
) -> dict[str, Any]:
    error = case.failure(method, case.write_params(document, params))
    case.assertEqual(error["code"], code, error)
    case.assertEqual(_revision(case, document), case.revisions[document])
    return error


def _drawing(number: int) -> dict[str, Any]:
    return {"number": number, "suffix": ""}


def _level(case: NativeAcceptance, document: str, name: str, drawings: int) -> str:
    level = case.mutate("level.create", document, name=name, format="vector")["level_id"]
    for number in range(1, drawings + 1):
        case.mutate(
            "drawing.create", document, level_id=level, drawing_id=_drawing(number), format="vector"
        )
    return level


def _square(
    case: NativeAcceptance, document: str, level: str, center: Point, half: float = 0.1
) -> None:
    style = case.mutate(
        "palette.create_style",
        document,
        level_id=level,
        name="Marker",
        color=dict(zip(("r", "g", "b", "a"), RED, strict=True)),
    )["style_id"]
    left, bottom = (center[0] - half) * INCH, (center[1] - half) * INCH
    right, top = (center[0] + half) * INCH, (center[1] + half) * INCH
    middle_x, middle_y = (left + right) / 2, (bottom + top) / 2
    corners = (
        (left, bottom),
        (middle_x, bottom),
        (right, bottom),
        (right, middle_y),
        (right, top),
        (middle_x, top),
        (left, top),
        (left, middle_y),
        (left, bottom),
    )
    target = {
        "level_id": level,
        "drawing_id": _drawing(1),
        "coordinate_space": "drawing_native",
        "sharing": "all_exposures",
    }
    edit = {
        "action": "add",
        "style_id": style,
        "closed": True,
        "points": [{"x": x, "y": y, "thickness": 0.5} for x, y in corners],
    }
    case.mutate("vector.edit", document, **target, edits=[edit])
    case.mutate("vector.fill", document, **target, x=middle_x, y=middle_y, style_id=style)


def _column(
    case: NativeAcceptance, document: str, name: str, level: str, rows: Sequence[int]
) -> dict[str, Any]:
    column = case.mutate("column.create", document, name=name)
    for row, number in enumerate(rows):
        case.mutate(
            "column.expose",
            document,
            column_id=column["column_id"],
            level_id=level,
            drawing_id=_drawing(number),
            start_frame=row,
            length=1,
            overwrite=True,
        )
    return column


def _hook(
    case: NativeAcceptance,
    document: str,
    level: str,
    number: int,
    positions: dict[int, Point | tuple[Point, Point]],
) -> dict[str, Any]:
    records = []
    for drawing, position in positions.items():
        record: dict[str, Any] = {"drawing_id": _drawing(drawing)}
        a, b = position if isinstance(position[0], tuple) else (position, position)
        record["a"] = {"x": a[0], "y": a[1]}
        if b != a:
            record["b"] = {"x": b[0], "y": b[1]}
        records.append(record)
    return case.mutate(
        "hook.set",
        document,
        level_id=level,
        hook_number=number,
        unit="inch",
        positions=records,
        **PRECISION,
    )


def _parent(
    case: NativeAcceptance, document: str, child: str, parent: str, **params: Any
) -> dict[str, Any]:
    return case.mutate(
        "object.parent",
        document,
        object_id=child,
        parent_id=parent,
        placement_policy="preserve_local",
        **params,
    )


def _keys(
    case: NativeAcceptance, document: str, object_id: str, channel: str, unit: str, **values: float
) -> None:
    keys = [
        {"frame": int(frame.removeprefix("f")), "value": value, "interpolation": "linear"}
        for frame, value in values.items()
    ]
    case.mutate(
        "animation.set_keys", document, object_id=object_id, channel=channel, unit=unit, keys=keys
    )


def _evaluate(
    case: NativeAcceptance, document: str, object_id: str, frames: Sequence[int]
) -> list[dict[str, Any]]:
    result = _read(case, "rig.evaluate", document, object_ids=[object_id], frames=list(frames))
    case.assertEqual((result["unit"], result["space"]), ("inch", "stage"))
    return result["samples"]


def _stage_point(sample: dict[str, Any], local: Point) -> Point:
    placement = sample["placement"]
    return (
        placement["a11"] * local[0] + placement["a12"] * local[1] + placement["dx_inch"],
        placement["a21"] * local[0] + placement["a22"] * local[1] + placement["dy_inch"],
    )


def _pixel(point: Point) -> Point:
    return (
        SIZE / 2 + point[0] * PIXELS_PER_INCH,
        SIZE / 2 - point[1] * PIXELS_PER_INCH,
    )


def _centroid(case: NativeAcceptance, pixels: list[Pixel]) -> Point:
    weight = total_x = total_y = 0.0
    for index, (red, green, _blue, alpha) in enumerate(pixels):
        if alpha and red > 120 and green < 110:
            weight += alpha
            total_x += alpha * (index % SIZE + 0.5)
            total_y += alpha * (index // SIZE + 0.5)
    case.assertGreater(weight, 0, "The marker did not render")
    return total_x / weight, total_y / weight


def _xy(point: dict[str, float]) -> Point:
    return point["x"], point["y"]


def _assert_near(case: NativeAcceptance, actual: Point, expected: Point, tolerance: float) -> None:
    case.assertLessEqual(abs(actual[0] - expected[0]), tolerance, (actual, expected))
    case.assertLessEqual(abs(actual[1] - expected[1]), tolerance, (actual, expected))


def _marker_at(case: NativeAcceptance, document: str, frame: int, stage: Point) -> list[Pixel]:
    pixels = _render(case, document, frame)
    _assert_near(case, _centroid(case, pixels), _pixel(stage), 0.75)
    return pixels


def _reopen(case: NativeAcceptance, document: str, name: str) -> str:
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / name))
    manifest = case.success("scene.package_info", {"path": saved["package_directory"]})
    files = {entry["path"] for entry in manifest["files"]}
    for level in manifest["levels"]:
        sidecar = level["path"].removesuffix(".pli") + "_hooks.pli.xml"
        case.assertEqual(sidecar in files, level["hooks_sha256"] is not None, level)
    return case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]


def _object(case: NativeAcceptance, document: str, name: str) -> str:
    objects = case.success("scene.summary", {"document_id": document})["objects"]
    return next(item["object_id"] for item in objects if item["name"] == name)


def _arm_rig(case: NativeAcceptance) -> tuple[str, str, dict[str, Any], dict[str, Any]]:
    document = _scene(case, "Hook attach")
    body = _level(case, document, "Body", 2)
    arm = _level(case, document, "Arm", 1)
    _square(case, document, arm, (0.25, 0))
    body_column = _column(case, document, "Body", body, [1, 1, 1, 2, 2, 2])
    arm_column = _column(case, document, "Arm", arm, [1] * 6)
    _hook(case, document, body, 1, {1: (0, 0.25), 2: (0, 0.25)})
    _hook(case, document, arm, 2, {1: (-0.25, 0)})
    parented = _parent(
        case,
        document,
        arm_column["object_id"],
        body_column["object_id"],
        parent_handle={"kind": "hook", "hook_number": 1},
        report_frames=[0],
    )
    case.assertEqual(parented["parent_handle"], {"kind": "hook", "native": "H1", "hook_number": 1})
    case.assertEqual(len(parented["world_changes"]), 1)
    handles = case.mutate(
        "object.handles.set",
        document,
        object_id=arm_column["object_id"],
        handle={"kind": "hook", "hook_number": 2},
        placement_policy="native_no_compensation",
        report_frames=[0],
    )
    case.assertTrue(handles["pivot_reset"]["occurred"])
    case.assertEqual(handles["keys_rewritten"], 0)
    _keys(case, document, arm_column["object_id"], "rotation", "degree", f0=0, f1=45, f2=90)
    return document, body, body_column, arm_column


def rig_hook_attach_renders_and_reopens(case: NativeAcceptance) -> None:
    """Render an arm attached at body hook H1 and pivoting about its own hook H2."""
    document, body, body_column, arm_column = _arm_rig(case)
    inspected = _read(case, "rig.inspect", document, root_object_id=body_column["object_id"])
    records = {item["object_id"]: item for item in inspected["objects"]}
    arm_record = records[arm_column["object_id"]]
    case.assertEqual(arm_record["parent_id"], body_column["object_id"])
    case.assertEqual(arm_record["handle"]["native"], "H2")
    bindings = {(item["role"], item["hook_number"]) for item in arm_record["hook_bindings"]}
    case.assertEqual(bindings, {("own_handle", 2), ("parent_handle", 1)})
    case.assertTrue(all(item["hook_defined"] for item in arm_record["hook_bindings"]))
    samples = _evaluate(case, document, arm_column["object_id"], [0, 1, 2])
    renders = []
    for sample in samples:
        _assert_near(case, _xy(sample["parent_attachment_local"]), (0, 0.25), 1e-5)
        _assert_near(case, _xy(sample["own_handle_local"]), (-0.25, 0), 1e-5)
        renders.append(_marker_at(case, document, sample["frame"], _stage_point(sample, (0.25, 0))))
    _assert_near(case, _stage_point(samples[2], (0.25, 0)), (0, 0.75), 1e-5)
    reopened = _reopen(case, document, "hook-attach")
    for frame, expected in enumerate(renders):
        _pixels_match(case, _render(case, reopened, frame), expected)
    source = _read(case, "hook.inspect", document, level_id=body)["hooks"]
    hooks = _read(case, "hook.inspect", reopened, level_id=_level_id(case, reopened, "Body"))
    case.assertEqual(
        [key["a"] for key in hooks["hooks"][0]["keys"]],
        [key["a"] for key in source[0]["keys"]],
    )
    _hook(case, document, body, 1, {2: (-0.5, -0.25)})
    _pixels_match(case, _render(case, document, 1), renders[1])
    moved = _evaluate(case, document, arm_column["object_id"], [4])[0]
    _marker_at(case, document, 4, _stage_point(moved, (0.25, 0)))
    _assert_near(case, _stage_point(moved, (0.25, 0)), (-0.5, 0.25), 1e-5)


def _level_id(case: NativeAcceptance, document: str, name: str) -> str:
    levels = case.success("scene.summary", {"document_id": document})["levels"]
    return next(item["level_id"] for item in levels if item["name"] == name)


def rig_pass_hook_matches_render(case: NativeAcceptance) -> None:
    """Accumulate a pass hook natively and agree with the rendered attachment."""
    document = _scene(case, "Pass hook")
    body = _level(case, document, "Body", 3)
    marker = _level(case, document, "Marker", 1)
    _square(case, document, marker, (0, 0))
    body_column = _column(case, document, "Body", body, [1, 2, 3])
    child = _column(case, document, "Child", marker, [1, 1, 1])
    _hook(
        case,
        document,
        body,
        1,
        {1: (0, 0), 2: ((0.25, 0), (0, 0.25)), 3: (0.25, -0.25)},
    )
    keys = _read(case, "hook.inspect", document, level_id=body)["hooks"][0]["keys"]
    case.assertEqual([key["pass"] for key in keys], [False, True, False])
    _parent(
        case,
        document,
        child["object_id"],
        body_column["object_id"],
        parent_handle={"kind": "hook", "hook_number": 1},
    )
    expected = [(0, 0), (0.25, 0), (0.5, -0.5)]
    for sample, point in zip(
        _evaluate(case, document, child["object_id"], [0, 1, 2]), expected, strict=True
    ):
        _assert_near(case, _xy(sample["parent_attachment_local"]), point, 1e-5)
        _marker_at(case, document, sample["frame"], point)


def _width(pixels: list[Pixel]) -> int:
    row = pixels[(SIZE // 2) * SIZE : (SIZE // 2 + 1) * SIZE]
    return sum(1 for red, green, _blue, alpha in row if alpha == 255 and red > 120 and green < 110)


def _scale_sample(case: NativeAcceptance, document: str, peg: str, **layer: str) -> dict[str, Any]:
    return case.success(
        "animation.sample",
        {
            "document_id": document,
            "object_id": peg,
            "channel": "scale",
            "unit": "scalar",
            "frames": [0],
            **layer,
        },
    )["values"][0]


def rig_additive_scale_renders_and_reopens(case: NativeAcceptance) -> None:
    """Render a 1 + 0.5 offset-layer scale, its disabled state, and both after reopen."""
    document = _scene(case, "Additive scale")
    level = _level(case, document, "Square", 1)
    _square(case, document, level, (0, 0), half=0.25)
    column = _column(case, document, "Square", level, [1])
    peg = case.mutate("object.create", document, name="Breathing peg", kind="pegbar")["object_id"]
    _parent(case, document, column["object_id"], peg)
    baseline = _render(case, document, 0)
    case.assertEqual(_width(baseline), 8)
    layer = case.mutate("offset_layer.create", document, object_id=peg, name="Breath ✨")
    layer_id = layer["layer"]["layer_id"]
    case.assertEqual(layer["layer"]["weight"], {"default_value": 1, "key_count": 0})
    case.mutate(
        "animation.set_keys",
        document,
        object_id=peg,
        layer_id=layer_id,
        channel="scale",
        unit="scalar",
        keys=[{"frame": 0, "value": 0.5, "interpolation": "constant"}],
    )
    case.assertEqual(
        _scale_sample(case, document, peg),
        {"frame": 0, "value": 1.5, "base_value": 1, "offset_value": 0.5},
    )
    case.assertEqual(
        _scale_sample(case, document, peg, layer_id=layer_id),
        {"frame": 0, "value": 0.5, "weighted_value": 0.5},
    )
    channels = _evaluate(case, document, peg, [0])[0]["channels"]
    scale = next(item for item in channels if item["channel"] == "scale")
    case.assertEqual((scale["base"], scale["offset"], scale["effective"]), (1, 0.5, 1.5))
    layered = _render(case, document, 0)
    case.assertEqual(_width(layered), 12)
    reopened = _reopen(case, document, "layer-enabled")
    _pixels_match(case, _render(case, reopened, 0), layered)
    case.mutate("offset_layer.update", document, layer_id=layer_id, enabled=False)
    _pixels_match(case, _render(case, document, 0), baseline)
    case.assertEqual(_scale_sample(case, document, peg, layer_id=layer_id)["weighted_value"], 0)
    disabled = _reopen(case, document, "layer-disabled")
    _pixels_match(case, _render(case, disabled, 0), baseline)
    summary = _read(
        case, "rig.inspect", disabled, root_object_id=_object(case, disabled, "Breathing peg")
    )
    case.assertEqual(
        [(item["name"], item["enabled"]) for item in summary["objects"][0]["offset_layers"]],
        [("Breath ✨", False)],
    )


def _shift(case: NativeAcceptance, change: dict[str, Any], before: Point, after: Point) -> None:
    measured = (after[0] - before[0], after[1] - before[1])
    reported = (
        (change["after"]["dx_inch"] - change["before"]["dx_inch"]) * PIXELS_PER_INCH,
        -(change["after"]["dy_inch"] - change["before"]["dy_inch"]) * PIXELS_PER_INCH,
    )
    _assert_near(case, measured, reported, 0.75)


def rig_handles_and_pivot_report_world_changes(case: NativeAcceptance) -> None:
    """Match handle and pivot world deltas with measured render shifts."""
    document = _scene(case, "Handles and pivot")
    level = _level(case, document, "Part", 1)
    _square(case, document, level, (0.25, 0))
    column = _column(case, document, "Part", level, [1, 1, 1])
    object_id = column["object_id"]
    _hook(case, document, level, 1, {1: (0.25, 0)})
    _keys(case, document, object_id, "rotation", "degree", f0=0, f2=90)
    frames = [0, 1, 2]
    before = [_centroid(case, _render(case, document, frame)) for frame in frames]
    handles = case.mutate(
        "object.handles.set",
        document,
        object_id=object_id,
        handle={"kind": "hook", "hook_number": 1},
        placement_policy="native_no_compensation",
        report_frames=frames,
    )
    case.assertFalse(handles["world_placement_preserved"])
    renders = [_render(case, document, frame) for frame in frames]
    moved = [_centroid(case, pixels) for pixels in renders]
    for change, first, second in zip(handles["world_changes"], before, moved, strict=True):
        _shift(case, change, first, second)
    pivot = case.mutate(
        "object.pivot.set",
        document,
        object_id=object_id,
        unit="inch",
        center={"x": 0.1, "y": 0.05},
        policy={"kind": "preserve_world_at_frame", "frame": 1},
        report_frames=frames,
        **PRECISION,
    )
    case.assertEqual(pivot["center"], {"x": 0.1, "y": 0.05})
    case.assertEqual(pivot["keys_rewritten"], 0)
    changes = pivot["world_changes"]
    case.assertEqual([item["preserved"] for item in changes], [False, True, False])
    _pixels_match(case, _render(case, document, 1), renders[1])
    for index in (0, 2):
        _shift(case, changes[index], moved[index], _centroid(case, _render(case, document, index)))
    record = _read(case, "rig.inspect", document, root_object_id=object_id)["objects"][0]
    case.assertEqual(record["pivot"]["center"], {"x": 0.1, "y": 0.05})
    case.assertEqual(record["pivot"]["offset"], pivot["offset"])


def _users(case: NativeAcceptance, document: str, level: str) -> list[dict[str, Any]]:
    return _read(case, "hook.inspect", document, level_id=level, hook_numbers=[1])["hooks"][0][
        "users"
    ]


def rig_hook_lifecycle_and_drawing_edits(case: NativeAcceptance) -> None:
    """Exercise hook writes, users, locks, receipts and hook-aware drawing edits."""
    document = _scene(case, "Hook lifecycle")
    level = _level(case, document, "Body", 3)
    column = _column(case, document, "Body", level, [1, 2])
    child = case.mutate("column.create", document, name="Child")["object_id"]
    request = case.write_params(
        document,
        {
            "level_id": level,
            "hook_number": 1,
            "unit": "inch",
            "positions": [
                {"drawing_id": _drawing(1), "a": {"x": 0.1, "y": 0.2}},
                {"drawing_id": _drawing(2), "a": {"x": 0.3, "y": 0.4}},
            ],
            **PRECISION,
        },
    )
    created = case.success("hook.set", request)
    case.revisions[document] = created["revision"]
    case.assertEqual(case.success("hook.set", request), created)
    conflict = dict(request, hook_number=2)
    case.assertEqual(case.failure("hook.set", conflict)["code"], "operation_conflict")
    case.assertEqual(created["columns_affected"], [column["column_id"]])
    _hook(case, document, level, 1, {3: (0, 0)})
    revision = case.revisions[document]
    _hook(case, document, level, 1, {1: (0.1, 0.2)})
    case.assertEqual(case.revisions[document], revision)
    _parent(
        case, document, child, column["object_id"], parent_handle={"kind": "hook", "hook_number": 1}
    )
    case.assertEqual(
        _users(case, document, level),
        [
            {
                "object_id": column["object_id"],
                "column_id": column["column_id"],
                "role": "child_parent_handle",
                "child_object_id": child,
            }
        ],
    )
    error = _refused(
        case,
        "hook.delete",
        document,
        "has_dependents",
        level_id=level,
        hook_number=1,
        scope={"kind": "hook"},
    )
    case.assertEqual(len(error["details"]["users"]), 1)
    _hook_edit_refusals(case, document, level, column)
    _drawing_edits(case, document, level, child)


def _hook_edit_refusals(
    case: NativeAcceptance, document: str, level: str, column: dict[str, Any]
) -> None:
    base = {"level_id": level, "hook_number": 1, "unit": "inch", **PRECISION}
    snap = [{"drawing_id": _drawing(1), "a": {"x": 0, "y": 0}, "b": {"x": 0.01, "y": 0}}]
    lenient = dict(base, max_coordinate_error_inch=1)
    _refused(case, "hook.set", document, "invalid_params", **lenient, positions=snap)
    missing = [{"drawing_id": _drawing(9), "a": {"x": 0, "y": 0}}]
    _refused(case, "hook.set", document, "missing_drawing", **base, positions=missing)
    suffixed = [{"drawing_id": {"number": 1, "suffix": "a"}, "a": {"x": 0, "y": 0}}]
    _refused(case, "hook.set", document, case.schema_code(), **base, positions=suffixed)
    precise = [{"drawing_id": _drawing(1), "a": {"x": 0.123456789, "y": 0}}]
    strict = dict(base, max_coordinate_error_inch=0)
    _refused(case, "hook.set", document, "invalid_params", **strict, positions=precise)
    case.mutate("column.update", document, column_id=column["column_id"], locked=True)
    _refused(case, "hook.set", document, "locked", **base, positions=precise)
    _refused(
        case,
        "hook.delete",
        document,
        "locked",
        level_id=level,
        hook_number=1,
        scope={"kind": "drawings", "drawing_ids": [_drawing(3)]},
    )
    case.mutate("column.update", document, column_id=column["column_id"], locked=False)
    _refused(
        case,
        "hook.delete",
        document,
        "not_found",
        level_id=level,
        hook_number=2,
        scope={"kind": "hook"},
    )


def _drawing_edits(case: NativeAcceptance, document: str, level: str, child: str) -> None:
    cloned = case.mutate(
        "drawing.clone",
        document,
        level_id=level,
        drawing_id=_drawing(3),
        destination_drawing_id=_drawing(4),
    )
    case.assertEqual(cloned["source_hook_keys_not_copied"], 1)
    removed = case.mutate("drawing.remove", document, level_id=level, drawing_id=_drawing(3))
    case.assertEqual(removed["hook_keys_removed"], 1)
    suffixed = [{"drawing_id": _drawing(2), "destination_drawing_id": {"number": 2, "suffix": "b"}}]
    _refused(
        case,
        "drawing.renumber",
        document,
        "unsupported_drawing_id",
        level_id=level,
        mapping=suffixed,
        sharing="all_exposures",
    )
    swap = [
        {"drawing_id": _drawing(1), "destination_drawing_id": _drawing(2)},
        {"drawing_id": _drawing(2), "destination_drawing_id": _drawing(1)},
    ]
    renumbered = case.mutate(
        "drawing.renumber", document, level_id=level, mapping=swap, sharing="all_exposures"
    )
    case.assertEqual(renumbered["hook_keys_renumbered"], 2)
    keys = _read(case, "hook.inspect", document, level_id=level)["hooks"][0]["keys"]
    case.assertEqual([key["drawing_id"]["number"] for key in keys], [1, 2])
    _assert_near(case, _xy(keys[0]["a"]), (0.3, 0.4), 1e-5)
    _assert_near(case, _xy(keys[1]["a"]), (0.1, 0.2), 1e-5)
    first = {"kind": "drawings", "drawing_ids": [_drawing(1)]}
    case.mutate("hook.delete", document, level_id=level, hook_number=1, scope=first)
    _refused(
        case, "hook.delete", document, "missing_key", level_id=level, hook_number=1, scope=first
    )
    _hook(case, document, level, 3, {4: (0, 0)})
    case.mutate(
        "object.handles.set",
        document,
        object_id=child,
        parent_handle={"kind": "hook", "hook_number": 3},
        placement_policy="native_no_compensation",
        report_frames=[0],
    )
    error = _refused(
        case, "drawing.remove", document, "has_dependents", level_id=level, drawing_id=_drawing(4)
    )
    case.assertEqual(error["details"]["hooks"][0]["hook_number"], 3)


def rig_layer_lifecycle_and_animation_keys(case: NativeAcceptance) -> None:
    """Create, rename, reorder, disable and delete layers with keys through animation tools."""
    document = _scene(case, "Layer lifecycle")
    peg = case.mutate("object.create", document, name="Peg", kind="pegbar")["object_id"]
    first = case.mutate("offset_layer.create", document, object_id=peg, name="Walk")["layer"]
    second = case.mutate(
        "offset_layer.create", document, object_id=peg, name="Bob", index=0, weight=-2
    )["layer"]
    case.assertEqual((second["index"], second["weight"]["default_value"]), (0, -2))
    _refused(case, "offset_layer.create", document, "name_conflict", object_id=peg, name="Walk")
    _refused(
        case,
        "offset_layer.update",
        document,
        "name_conflict",
        layer_id=second["layer_id"],
        name="Walk",
    )
    moved = case.mutate("offset_layer.update", document, layer_id=first["layer_id"], index=0)
    case.assertEqual((moved["layer"]["layer_id"], moved["layer"]["index"]), (first["layer_id"], 0))
    revision = case.revisions[document]
    case.mutate("offset_layer.update", document, layer_id=first["layer_id"], index=0)
    case.assertEqual(case.revisions[document], revision)
    channels = case.success(
        "animation.channels",
        {"document_id": document, "object_id": peg, "layer_id": first["layer_id"]},
    )
    case.assertEqual(channels["value_space"], "offset_layer")
    case.assertEqual(
        [item["channel"] for item in channels["channels"]],
        ["rotation", "scale", "scale_x", "scale_y", "x", "y", "z", "weight"],
    )
    weight = {
        "object_id": peg,
        "layer_id": first["layer_id"],
        "channel": "weight",
        "unit": "scalar",
    }
    case.mutate(
        "animation.set_keys",
        document,
        **weight,
        keys=[{"frame": 0, "value": 2, "interpolation": "linear"}],
    )
    keys = case.success("animation.keys", {"document_id": document, **weight})
    case.assertEqual(keys["value_space"], "offset_layer_weight")
    too_heavy = [{"frame": 1, "value": 11, "interpolation": "linear"}]
    _refused(case, "animation.set_keys", document, "invalid_argument", **weight, keys=too_heavy)
    path = {
        "object_id": peg,
        "layer_id": first["layer_id"],
        "channel": "path_position",
        "unit": "percent",
    }
    _refused(
        case,
        "animation.set_keys",
        document,
        "unsupported_channel",
        **path,
        keys=[{"frame": 0, "value": 1, "interpolation": "linear"}],
    )
    no_layer = {"object_id": peg, "channel": "weight", "unit": "scalar"}
    _refused(case, "animation.delete_keys", document, "invalid_params", **no_layer, frames=[0])
    copy = {"object_id": peg, "channel": "x", "unit": "inch"}
    _refused(
        case,
        "animation.copy_curve",
        document,
        "unsupported_interpolation",
        source=copy,
        destination=copy,
        destination_policy="replace_all",
    )
    _layer_targets(case, document, peg)
    deleted = case.mutate("offset_layer.delete", document, layer_id=first["layer_id"])
    case.assertEqual((deleted["removed_name"], deleted["removed_key_count"]), ("Walk", 1))
    _refused(
        case,
        "offset_layer.update",
        document,
        "invalid_handle",
        layer_id=first["layer_id"],
        enabled=False,
    )


def _layer_targets(case: NativeAcceptance, document: str, peg: str) -> None:
    table = next(
        item["object_id"]
        for item in case.success("scene.summary", {"document_id": document})["objects"]
        if item["kind"] == "table"
    )
    _refused(case, "offset_layer.create", document, "unsupported_target", object_id=table, name="T")
    for index in range(14):
        case.mutate("offset_layer.create", document, object_id=peg, name=f"Layer {index}")
    _refused(case, "offset_layer.create", document, "resource_limit", object_id=peg, name="Extra")
    _refused(
        case, "offset_layer.create", document, "invalid_params", object_id=peg, name="Bad\nname"
    )


def rig_parent_policy_and_refusals(case: NativeAcceptance) -> None:
    """Require the parent placement policy, validate hook strings and bound depth."""
    document = _scene(case, "Parent policy")
    level = _level(case, document, "Body", 1)
    column = _column(case, document, "Body", level, [1])
    child = case.mutate("column.create", document, name="Child")["object_id"]
    peg = case.mutate("object.create", document, name="Peg", kind="pegbar")["object_id"]
    params = {"object_id": child, "parent_id": column["object_id"]}
    _refused(case, "object.parent", document, case.schema_code(), **params)
    hook = {"kind": "hook", "hook_number": 1}
    _refused(
        case,
        "object.parent",
        document,
        "invalid_params",
        **params,
        placement_policy="preserve_local",
        parent_handle=hook,
    )
    _hook(case, document, level, 1, {1: (0.5, 0)})
    result = _parent(
        case, document, child, column["object_id"], parent_handle=hook, report_frames=[0]
    )
    change = result["world_changes"][0]
    case.assertAlmostEqual(change["after"]["dx_inch"] - change["before"]["dx_inch"], 0.5, 5)
    case.assertFalse(result["world_placement_preserved"])
    _refused(
        case,
        "object.parent",
        document,
        "invalid_params",
        object_id=child,
        parent_id=peg,
        placement_policy="preserve_local",
    )
    letter = _parent(case, document, child, peg, parent_handle={"kind": "peg_hole", "letter": "c"})
    case.assertEqual(letter["parent_handle"]["static_offset_inch"], {"x": 4, "y": 0})
    pegs = [peg]
    for index in range(63):
        pegs.append(
            case.mutate("object.create", document, name=f"Chain {index}", kind="pegbar")[
                "object_id"
            ]
        )
        _parent(case, document, pegs[-1], pegs[-2])
    _refused(
        case,
        "object.parent",
        document,
        "resource_limit",
        object_id=child,
        parent_id=pegs[-1],
        placement_policy="preserve_local",
    )
    page = _read(case, "rig.inspect", document, limit=8)
    case.assertEqual((page["start_index"], page["next_index"], len(page["objects"])), (0, 8, 8))
    case.assertEqual(page["total_objects"], len(pegs) + 4)
