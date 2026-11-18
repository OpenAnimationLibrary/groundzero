"""Exercise native vector grouping and stacking through the real worker and PLI renderer."""

from __future__ import annotations

import base64
from itertools import pairwise
from typing import TYPE_CHECKING, Any

from mcp_png import Pixel, decode_png

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

DRAWING = {"number": 1, "suffix": ""}
COLORS: tuple[Pixel, ...] = ((210, 40, 60, 255), (40, 160, 220, 255), (40, 190, 80, 255))
SIZE = 256
SCALE = SIZE / (1.2 * 53.33333)


def _rectangle(x: float) -> dict[str, Any]:
    points = (
        (x - 5, -5),
        (x, -5),
        (x + 5, -5),
        (x + 5, 0),
        (x + 5, 5),
        (x, 5),
        (x - 5, 5),
        (x - 5, 0),
        (x - 5, -5),
    )
    return {
        "action": "add",
        "style_id": 1,
        "closed": True,
        "points": [{"x": px, "y": py, "thickness": 1} for px, py in points],
    }


def _new_scene(case: NativeAcceptance) -> tuple[str, dict[str, Any], list[int]]:
    document = case.mutate(
        "scene.create",
        None,
        name="Native groups",
        width_px=64,
        height_px=64,
        width_in=1.2,
        height_in=1.2,
        frame_rate=24,
    )["document_id"]
    level = case.mutate("level.create", document, name="Grouped artwork", format="vector")
    target = {"level_id": level["level_id"], "drawing_id": DRAWING}
    case.mutate("drawing.create", document, **target, format="vector")
    styles = []
    for color in COLORS:
        result = case.mutate(
            "palette.create_style",
            document,
            level_id=target["level_id"],
            name="Group paint",
            color=dict(zip("rgba", color, strict=True)),
        )
        styles.append(result["style_id"])
    column = case.mutate("column.create", document, name="Shared drawing")
    case.mutate(
        "column.expose", document, **target, column_id=column["column_id"], start_frame=0, length=2
    )
    return document, target, styles


def _edit(
    case: NativeAcceptance, document: str, target: dict[str, Any], edits: list[dict[str, Any]]
) -> None:
    case.mutate(
        "vector.edit",
        document,
        **target,
        coordinate_space="drawing_native",
        sharing="all_exposures",
        edits=edits,
    )


def _fill(
    case: NativeAcceptance, document: str, target: dict[str, Any], x: float, style: int
) -> None:
    case.mutate(
        "vector.fill",
        document,
        **target,
        coordinate_space="drawing_native",
        sharing="all_exposures",
        x=x,
        y=0,
        style_id=style,
    )


def _fixture(case: NativeAcceptance) -> tuple[str, dict[str, Any], list[int]]:
    document, target, styles = _new_scene(case)
    _edit(case, document, target, [_rectangle(x) for x in (-16, 0, 16)])
    for x, style in zip((-16, 0, 16), styles, strict=True):
        _fill(case, document, target, x, style)
    return document, target, styles


def _structure(case: NativeAcceptance, document: str, target: dict[str, Any]) -> dict[str, Any]:
    return case.success("vector.structure", {"document_id": document, **target})


def _geometry(
    case: NativeAcceptance, document: str, target: dict[str, Any]
) -> list[dict[str, Any]]:
    return case.success("vector.inspect", {"document_id": document, **target, "limit": 128})[
        "strokes"
    ]


def _preview(case: NativeAcceptance, document: str, frame: int = 0) -> list[Pixel]:
    job = case.mutate("render.preview", document, frame=frame, width=SIZE, height=SIZE)
    state = case.wait_job(job["job_id"])
    case.assertEqual(state["status"], "completed", state)
    case.assertIs(state["writers_closed"], True)
    artifact = case.success("job.result", {"job_id": job["job_id"]})["artifacts"][0]
    result = case.success("artifact.read", {"artifact_id": artifact["artifact_id"]})
    width, height, pixels = decode_png(base64.b64decode(result["image"]["data"], validate=True))
    case.assertEqual((width, height), (SIZE, SIZE))
    return pixels


def _sample(pixels: list[Pixel], x: float, y: float = 0) -> Pixel:
    return pixels[round(SIZE / 2 - y * SCALE) * SIZE + round(SIZE / 2 + x * SCALE)]


def _group(first: int, count: int, index: int = 0) -> dict[str, Any]:
    return {"index": index, "parent_index": None, "first_index": first, "count": count, "depth": 1}


def _mutate(
    case: NativeAcceptance,
    document: str,
    target: dict[str, Any],
    method: str,
    params: dict[str, int],
) -> dict[str, Any]:
    before = _geometry(case, document, target)
    result = case.mutate(method, document, **target, sharing="all_exposures", **params)
    after = _geometry(case, document, target)
    case.assertEqual(result["scope"], "detached")
    case.assertIs(result["undo_available"], False)
    case.assertEqual(result["geometry_indices_invalidated"], result["changed"])
    case.assertEqual(result["stroke_count"], len(before))
    case.assertEqual(len(result["order_mapping"]), len(before))
    for index, mapping in enumerate(result["order_mapping"]):
        case.assertEqual(mapping["previous_index"], index)
        case.assertEqual(mapping["native_id"], before[index]["native_id"])
        expected = {**before[index], "index": mapping["index"]}
        case.assertEqual(after[mapping["index"]], expected)
    case.assertEqual(result["groups"], _structure(case, document, target)["groups"])
    return result


def _reopen(
    case: NativeAcceptance, document: str, target: dict[str, Any], directory: str
) -> tuple[str, dict[str, Any]]:
    before = _structure(case, document, target)
    geometry = _geometry(case, document, target)
    pixels = _preview(case, document)
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / directory))
    case.mutate("scene.discard", document)
    opened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    summary = case.success("scene.summary", {"document_id": opened})
    target = {**target, "level_id": summary["levels"][0]["level_id"]}
    actual = _structure(case, opened, target)
    for key in ("stroke_count", "groups", "order", "inside_group_depth", "write_admission"):
        case.assertEqual(actual[key], before[key], key)
    for prior, current in zip(geometry, _geometry(case, opened, target), strict=True):
        case.assertEqual(
            {k: v for k, v in prior.items() if k != "native_id"},
            {k: v for k, v in current.items() if k != "native_id"},
        )
    case.assertEqual(_preview(case, opened), pixels)
    case.assertEqual(_preview(case, opened, 1), pixels)
    return opened, target


def _compound(
    case: NativeAcceptance, document: str, target: dict[str, Any], style: int
) -> tuple[str, dict[str, Any]]:
    groups = _structure(case, document, target)["groups"]
    case.mutate(
        "vector.transform",
        document,
        **target,
        coordinate_space="drawing_native",
        sharing="all_exposures",
        thickness_mode="preserve",
        matrix={"a11": 1, "a12": 0, "a13": 2, "a21": 0, "a22": 1, "a23": 0},
    )
    color = (230, 130, 30, 255)
    case.mutate(
        "palette.update_style",
        document,
        level_id=target["level_id"],
        style_id=style,
        color=dict(zip("rgba", color, strict=True)),
    )
    renamed = {"number": 7, "suffix": "ab"}
    case.mutate(
        "drawing.renumber",
        document,
        level_id=target["level_id"],
        sharing="all_exposures",
        mapping=[{"drawing_id": target["drawing_id"], "destination_drawing_id": renamed}],
    )
    target = {**target, "drawing_id": renamed}
    case.assertEqual(_structure(case, document, target)["groups"], groups)
    case.assertEqual(_sample(_preview(case, document), -14), color)
    return _reopen(case, document, target, "grouped-compound")


def vector_groups_preserve_paint_and_roundtrip(case: NativeAcceptance) -> None:
    """Preserve held drawings, native IDs and pixels through grouping and a compound save.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, target, styles = _fixture(case)
    clone_id = {"number": 2, "suffix": "b"}
    case.mutate("drawing.clone", document, **target, destination_drawing_id=clone_id)
    clone = {**target, "drawing_id": clone_id}
    clone_geometry = _geometry(case, document, clone)
    pixels = _preview(case, document)
    for x, color in zip((-16, 0, 16), COLORS, strict=True):
        case.assertEqual(_sample(pixels, x), color)
    result = _mutate(case, document, target, "vector.group.create", {"first_index": 0, "count": 2})
    case.assertEqual(result["groups"], [_group(0, 2)])
    result = _mutate(
        case,
        document,
        target,
        "vector.order.move",
        {"first_index": 0, "count": 2, "before_index": 3},
    )
    case.assertEqual(result["groups"], [_group(1, 2)])
    case.assertEqual([entry["index"] for entry in result["order_mapping"]], [1, 2, 0])
    case.assertEqual(_geometry(case, document, clone), clone_geometry)
    case.assertEqual(_structure(case, document, clone)["groups"], [])
    case.assertEqual(_preview(case, document), pixels)
    case.assertEqual(_preview(case, document, 1), pixels)
    document, target = _compound(case, document, target, styles[0])
    result = _mutate(case, document, target, "vector.group.remove", {"first_index": 1})
    case.assertEqual(result["groups"], [])
    _reopen(case, document, target, "ungrouped")


def _reject(
    case: NativeAcceptance,
    document: str,
    target: dict[str, Any],
    method: str,
    params: dict[str, Any],
    *,
    code: str,
) -> None:
    before = _structure(case, document, target)
    geometry = _geometry(case, document, target)
    request = case.write_params(document, {**target, "sharing": "all_exposures", **params})
    error = case.failure(method, request)
    case.assertEqual(error["code"], code, error)
    case.assertEqual(_structure(case, document, target), before)
    case.assertEqual(_geometry(case, document, target), geometry)


def vector_groups_reject_partial_boundaries(case: NativeAcceptance) -> None:
    """Reject a private native edit that would erase a multi-stroke painted region.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, target, styles = _new_scene(case)
    corners = ((-5, -5), (5, -5), (5, 5), (-5, 5), (-5, -5))
    edits = []
    for first, last in pairwise(corners):
        points = (first, ((first[0] + last[0]) / 2, (first[1] + last[1]) / 2), last)
        edits.append(
            {
                "action": "add",
                "style_id": 1,
                "closed": False,
                "points": [{"x": x, "y": y, "thickness": 1} for x, y in points],
            }
        )
    _edit(case, document, target, edits)
    _fill(case, document, target, 0, styles[0])
    pixels = _preview(case, document)
    case.assertEqual(_sample(pixels, 0), COLORS[0])
    _reject(
        case,
        document,
        target,
        "vector.group.create",
        {"first_index": 0, "count": 2},
        code="unsupported_grouping",
    )
    case.assertEqual(_preview(case, document), pixels)
    result = _mutate(case, document, target, "vector.group.create", {"first_index": 0, "count": 4})
    case.assertEqual(result["groups"], [_group(0, 4)])
    case.assertEqual(_preview(case, document), pixels)
    _reopen(case, document, target, "whole-boundary")


def vector_order_preserves_groups_and_stacking(case: NativeAcceptance) -> None:
    """Move independent overlapping painted groups and verify the intended visible overlap.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, target, styles = _new_scene(case)
    for index, x in enumerate((-3, 3)):
        _edit(case, document, target, [_rectangle(x)])
        _fill(case, document, target, x, styles[index])
        _mutate(case, document, target, "vector.group.create", {"first_index": index, "count": 1})
    before = _preview(case, document)
    case.assertEqual(_sample(before, 0), COLORS[1])
    result = _mutate(
        case,
        document,
        target,
        "vector.order.move",
        {"first_index": 0, "count": 1, "before_index": 2},
    )
    case.assertEqual(result["groups"], [_group(0, 1), _group(1, 1, 1)])
    after = _preview(case, document)
    case.assertNotEqual(after, before)
    case.assertEqual(_sample(after, 0), COLORS[0])
    for pixels in (before, after):
        case.assertEqual(_sample(pixels, -5), COLORS[0])
        case.assertEqual(_sample(pixels, 5), COLORS[1])
    _reopen(case, document, target, "overlap-order")


def _bad_ranges(case: NativeAcceptance, document: str, target: dict[str, Any]) -> None:
    cases = (
        ("vector.group.create", {"first_index": 0, "count": 1}, "unsupported_grouping"),
        ("vector.group.create", {"first_index": 0, "count": 3}, "unsupported_grouping"),
        ("vector.group.remove", {"first_index": 1}, "invalid_params"),
        ("vector.group.remove", {"first_index": 2}, "not_found"),
        (
            "vector.order.move",
            {"first_index": 0, "count": 1, "before_index": 3},
            "unsupported_grouping",
        ),
        (
            "vector.order.move",
            {"first_index": 2, "count": 1, "before_index": 1},
            "unsupported_grouping",
        ),
        ("vector.order.move", {"first_index": 0, "count": 2, "before_index": 4}, "invalid_params"),
        ("vector.group.create", {"first_index": 0, "count": 0}, case.schema_code()),
        ("vector.group.create", {"first_index": 0, "count": 4097}, case.schema_code()),
        (
            "vector.group.create",
            {"first_index": 0, "count": 1, "drawing_id": {"number": 999, "suffix": ""}},
            "not_found",
        ),
        (
            "vector.group.create",
            {"first_index": 0, "count": 1, "sharing": "one_exposure"},
            case.schema_code(),
        ),
        ("vector.group.create", {"first_index": 3, "count": 1}, "invalid_params"),
        ("vector.group.create", {"first_index": 0.5, "count": 1}, case.schema_code()),
        ("vector.group.create", {"first_index": 0, "count": 1, "extra": 1}, case.schema_code()),
    )
    for method, params, code in cases:
        with case.subTest(method=method, params=params):
            _reject(case, document, target, method, params, code=code)


def vector_group_noops_and_rejections(case: NativeAcceptance) -> None:
    """Keep revisions and native state unchanged for exact no-ops and invalid selections.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, target, _ = _fixture(case)
    stale = case.write_params(
        document, {**target, "sharing": "all_exposures", "first_index": 0, "count": 2}
    )
    _mutate(case, document, target, "vector.group.create", {"first_index": 0, "count": 2})
    before = _structure(case, document, target)
    revision = case.revisions[document]
    noops = [("vector.group.create", {"first_index": 0, "count": 2})]
    noops.extend(
        ("vector.order.move", {"first_index": 0, "count": 2, "before_index": index})
        for index in range(3)
    )
    for method, params in noops:
        result = _mutate(case, document, target, method, params)
        case.assertIs(result["changed"], False)
        case.assertEqual(case.revisions[document], revision)
    case.assertEqual(case.failure("vector.group.create", stale)["code"], "revision_conflict")
    case.assertEqual(_structure(case, document, target), before)
    _bad_ranges(case, document, target)
    _raster_rejection(case, document, target)
    _mutate(case, document, target, "vector.group.remove", {"first_index": 0})
    result = _mutate(
        case,
        document,
        target,
        "vector.order.move",
        {"first_index": 0, "count": 2, "before_index": 3},
    )
    case.assertEqual([entry["index"] for entry in result["order_mapping"]], [1, 2, 0])
    case.assertEqual(result["groups"], [])


def _raster_rejection(case: NativeAcceptance, document: str, target: dict[str, Any]) -> None:
    level = case.mutate("level.create", document, name="Raster target", format="rgba8")
    raster = {"level_id": level["level_id"], "drawing_id": DRAWING}
    case.mutate("drawing.create", document, **raster, format="rgba8", width=2, height=2, dpi=120)
    _reject(
        case,
        document,
        target,
        "vector.group.create",
        {**raster, "first_index": 0, "count": 1},
        code="unsupported_type",
    )
