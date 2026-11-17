"""Qualify whole-drawing vector transforms through geometry, paint, and native PLI pixels."""

from __future__ import annotations

import base64
import hashlib
import math
from typing import TYPE_CHECKING, Any

from mcp_png import Pixel, decode_png

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


DRAWING = {"number": 1, "suffix": ""}
IDENTITY: dict[str, float] = {"a11": 1, "a12": 0, "a13": 0, "a21": 0, "a22": 1, "a23": 0}
RED: Pixel = (210, 40, 60, 255)
BLUE: Pixel = (40, 160, 220, 255)
GREEN: Pixel = (40, 190, 80, 255)


def _style(case: NativeAcceptance, document: str, level: str, color: Pixel) -> int:
    return case.mutate(
        "palette.create_style",
        document,
        level_id=level,
        name="Transform paint",
        color=dict(zip(("r", "g", "b", "a"), color, strict=True)),
    )["style_id"]


def _rectangle(box: tuple[int, int, int, int]) -> dict[str, Any]:
    left, bottom, right, top = box
    middle_x, middle_y = (left + right) / 2, (bottom + top) / 2
    points = (
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
    return {
        "action": "add",
        "style_id": 1,
        "closed": True,
        "points": [{"x": x, "y": y, "thickness": 1} for x, y in points],
    }


def _fixture(case: NativeAcceptance) -> tuple[str, dict[str, Any], list[int]]:
    document = case.mutate(
        "scene.create",
        None,
        name="Vector transforms",
        width_px=64,
        height_px=64,
        width_in=1.2,
        height_in=1.2,
        frame_rate=24,
    )["document_id"]
    level = case.mutate("level.create", document, name="Nested artwork", format="vector")
    target = {"level_id": level["level_id"], "drawing_id": DRAWING}
    case.mutate("drawing.create", document, **target, format="vector")
    styles = [_style(case, document, level["level_id"], color) for color in (RED, BLUE, GREEN)]
    line = {
        "action": "add",
        "style_id": styles[2],
        "closed": False,
        "points": [{"x": x, "y": -10, "thickness": 2} for x in (6, 10, 14)],
    }
    near_closed = _rectangle((6, 4, 18, 18))
    near_closed["closed"] = False
    near_closed["points"][-1]["y"] += 0.5
    case.mutate(
        "vector.edit",
        document,
        **target,
        coordinate_space="drawing_native",
        sharing="all_exposures",
        edits=[_rectangle((-20, -10, -4, 10)), _rectangle((-15, -4, -9, 4)), line, near_closed],
    )
    for x, y, style in ((-18, 0, styles[0]), (-12, 0, styles[1]), (12, 11, styles[2])):
        case.mutate(
            "vector.fill",
            document,
            **target,
            coordinate_space="drawing_native",
            sharing="all_exposures",
            x=x,
            y=y,
            style_id=style,
        )
    column = case.mutate("column.create", document, name="All shared exposures")
    case.mutate(
        "column.expose",
        document,
        **target,
        column_id=column["column_id"],
        start_frame=0,
        length=2,
    )
    return document, target, styles


def _geometry(case: NativeAcceptance, document: str, target: dict[str, Any]) -> dict[str, Any]:
    return case.success("vector.inspect", {"document_id": document, **target})


def _preview(case: NativeAcceptance, document: str, frame: int = 0) -> list[Pixel]:
    job = case.mutate("render.preview", document, frame=frame, width=64, height=64)
    status = case.wait_job(job["job_id"])
    case.assertEqual(status["status"], "completed", status)
    case.assertIs(status["writers_closed"], True)
    artifacts = case.success("render.artifacts", {"job_id": job["job_id"]})["artifacts"]
    case.assertEqual(len(artifacts), 1)
    result = case.success("artifact.read", {"artifact_id": artifacts[0]["artifact_id"]})
    case.assertEqual(result["frame"], frame)
    case.assertEqual(result["source_revision"], case.revisions[document])
    data = base64.b64decode(result["image"]["data"], validate=True)
    case.assertEqual(hashlib.sha256(data).hexdigest(), artifacts[0]["sha256"])
    width, height, pixels = decode_png(data)
    case.assertEqual((width, height), (64, 64))
    return pixels


def _point(matrix: dict[str, float], x: float, y: float) -> tuple[float, float]:
    return (
        matrix["a11"] * x + matrix["a12"] * y + matrix["a13"],
        matrix["a21"] * x + matrix["a22"] * y + matrix["a23"],
    )


def _assert_geometry(
    case: NativeAcceptance,
    before: dict[str, Any],
    after: dict[str, Any],
    matrix: dict[str, float],
    scale: float,
    *,
    native_ids: bool = True,
) -> None:
    case.assertEqual(before["total"], after["total"])
    for original, transformed in zip(before["strokes"], after["strokes"], strict=True):
        for key in ("index", "style_id", "closed"):
            case.assertEqual(transformed[key], original[key], key)
        if native_ids:
            case.assertEqual(transformed["native_id"], original["native_id"])
        for source, point in zip(original["points"], transformed["points"], strict=True):
            expected = _point(matrix, source["x"], source["y"])
            case.assertEqual((point["x"], point["y"]), expected)
            case.assertEqual(point["thickness"], source["thickness"] * scale)


def _transform(
    case: NativeAcceptance,
    document: str,
    target: dict[str, Any],
    matrix: dict[str, float],
    mode: str = "preserve",
) -> dict[str, Any]:
    before = _geometry(case, document, target)
    result = case.mutate(
        "vector.transform",
        document,
        **target,
        coordinate_space="drawing_native",
        sharing="all_exposures",
        matrix=matrix,
        thickness_mode=mode,
    )
    determinant = matrix["a11"] * matrix["a22"] - matrix["a12"] * matrix["a21"]
    scale = math.sqrt(abs(determinant)) if mode == "scale_geometric_mean" else 1
    _assert_geometry(case, before, _geometry(case, document, target), matrix, scale)
    case.assertEqual(result["geometry_indices_invalidated"], result["changed"])
    return result


def _paint_probes(
    case: NativeAcceptance, pixels: list[Pixel], points: list[tuple[float, float]]
) -> None:
    for (x, y), color in zip(points, (RED, BLUE, GREEN, GREEN), strict=True):
        # The 1.2-inch camera spans native 63.999996 units; samples stay inside flat regions.
        index = round(32 - y) * 64 + round(32 + x)
        case.assertEqual(pixels[index], color, (x, y))
    case.assertEqual(pixels[2 * 64 + 2][3], 0)


def _check_fills(
    case: NativeAcceptance,
    document: str,
    target: dict[str, Any],
    samples: list[tuple[float, float]],
    styles: list[int],
) -> None:
    revision = case.revisions[document]
    fills = ((samples[0], styles[0]), (samples[1], styles[1]), (samples[3], styles[2]))
    for (x, y), style in fills:
        result = case.mutate(
            "vector.fill",
            document,
            **target,
            coordinate_space="drawing_native",
            sharing="all_exposures",
            x=x,
            y=y,
            style_id=style,
        )
        case.assertEqual(result["previous_style_id"], style)
        case.assertIs(result["changed"], False)
        case.assertEqual(case.revisions[document], revision)


def _reopen(case: NativeAcceptance, document: str) -> tuple[str, dict[str, Any]]:
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "transformed"))
    case.mutate("scene.discard", document)
    opened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    summary = case.success("scene.summary", {"document_id": opened})
    case.assertEqual(len(summary["levels"]), 1)
    case.assertEqual(summary["levels"][0]["drawings"], [DRAWING])
    return opened, {"level_id": summary["levels"][0]["level_id"], "drawing_id": DRAWING}


def vector_affine_preserves_paint_and_roundtrip(case: NativeAcceptance) -> None:
    """Transform nested paint and an open stroke, then verify native PLI geometry and pixels.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, target, styles = _fixture(case)
    samples = [(-18.0, 0.0), (-12.0, 0.0), (10.0, -10.0), (12.0, 11.0)]
    _paint_probes(case, _preview(case, document), samples)
    matrices = (
        {**IDENTITY, "a13": 8, "a23": 4},
        {**IDENTITY, "a11": 0, "a12": -1, "a21": 1, "a22": 0},
        {**IDENTITY, "a11": -1, "a12": 0.5},
    )
    for matrix in matrices:
        before = case.revisions[document]
        case.assertIs(_transform(case, document, target, matrix)["changed"], True)
        case.assertNotEqual(case.revisions[document], before)
        samples = [_point(matrix, x, y) for x, y in samples]
        _check_fills(case, document, target, samples, styles)
        _paint_probes(case, _preview(case, document), samples)
    geometry = _geometry(case, document, target)
    pixels = _preview(case, document)
    case.assertEqual(_preview(case, document, 1), pixels)
    opened, restored = _reopen(case, document)
    after = _geometry(case, opened, restored)
    _assert_geometry(case, geometry, after, IDENTITY, 1, native_ids=False)
    _check_fills(case, opened, restored, samples, styles)
    case.assertEqual(_preview(case, opened), pixels)
    case.assertEqual(_preview(case, opened, 1), pixels)


def _reject(
    case: NativeAcceptance,
    document: str,
    target: dict[str, Any],
    overrides: dict[str, Any],
    code: str,
) -> None:
    original = _geometry(case, document, target)
    summary = case.success("scene.summary", {"document_id": document})
    params = {
        **target,
        "coordinate_space": "drawing_native",
        "sharing": "all_exposures",
        "matrix": IDENTITY,
        "thickness_mode": "preserve",
        **overrides,
    }
    case.assertEqual(
        case.failure("vector.transform", case.write_params(document, params))["code"], code
    )
    case.assertEqual(_geometry(case, document, target), original)
    case.assertEqual(case.success("scene.summary", {"document_id": document}), summary)


def _radius_boundaries(case: NativeAcceptance, document: str, target: dict[str, Any]) -> None:
    target = {**target, "drawing_id": {"number": 3, "suffix": ""}}
    case.mutate("drawing.create", document, **target, format="vector")
    case.mutate(
        "vector.edit",
        document,
        **target,
        coordinate_space="drawing_native",
        sharing="all_exposures",
        edits=[
            {
                "action": "add",
                "style_id": 1,
                "points": [{"x": x, "y": 0, "thickness": 127.5} for x in (0, 0.5, 1)],
            }
        ],
    )
    _transform(case, document, target, {**IDENTITY, "a11": 4}, "scale_geometric_mean")
    _reject(
        case,
        document,
        target,
        {
            "matrix": {**IDENTITY, "a11": 1.01, "a22": 1.01},
            "thickness_mode": "scale_geometric_mean",
        },
        "unsupported_geometry",
    )


def _reject_raster(case: NativeAcceptance, document: str) -> None:
    level = case.mutate("level.create", document, name="Raster", format="rgba8")
    target = {"level_id": level["level_id"], "drawing_id": DRAWING}
    case.mutate("drawing.create", document, **target, format="rgba8", width=2, height=2, dpi=120)
    summary = case.success("scene.summary", {"document_id": document})
    params = {
        **target,
        "coordinate_space": "drawing_native",
        "sharing": "all_exposures",
        "matrix": IDENTITY,
        "thickness_mode": "preserve",
    }
    error = case.failure("vector.transform", case.write_params(document, params))
    case.assertEqual(error["code"], "unsupported_type")
    case.assertEqual(case.success("scene.summary", {"document_id": document}), summary)


def vector_transform_rejections_and_noops(case: NativeAcceptance) -> None:
    """Reject unsupported matrices and output bounds without publishing a partial drawing.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, target, _styles = _fixture(case)
    before = case.revisions[document]
    case.assertIs(_transform(case, document, target, IDENTITY)["changed"], False)
    case.assertEqual(case.revisions[document], before)
    invalid = (
        ({"matrix": {**IDENTITY, "a11": 0}}, "unsupported_transform"),
        ({"matrix": {**IDENTITY, "a11": 0.0009}}, "unsupported_transform"),
        ({"matrix": {**IDENTITY, "a13": 32768}}, "unsupported_geometry"),
        ({"matrix": {**IDENTITY, "a23": -32768}}, "unsupported_geometry"),
        (
            {
                "matrix": {**IDENTITY, "a11": 256, "a22": 256},
                "thickness_mode": "scale_geometric_mean",
            },
            "unsupported_geometry",
        ),
        ({"matrix": {**IDENTITY, "a11": "1"}}, "invalid_params"),
        ({"matrix": {"a11": 1}}, "invalid_params"),
        ({"matrix": {**IDENTITY, "a13": 32769}}, "invalid_params"),
        ({"sharing": "one_exposure"}, "invalid_params"),
        ({"coordinate_space": "camera_pixels"}, "invalid_params"),
        ({"thickness_mode": "maximum_axis"}, "invalid_params"),
    )
    for overrides, code in invalid:
        _reject(case, document, target, overrides, code)
    _transform(case, document, target, {**IDENTITY, "a13": 32750})
    _transform(case, document, target, {**IDENTITY, "a13": -32750})
    _transform(case, document, target, {**IDENTITY, "a13": -32748})
    _transform(case, document, target, {**IDENTITY, "a13": 32748})
    empty = {**target, "drawing_id": {"number": 2, "suffix": ""}}
    case.mutate("drawing.create", document, **empty, format="vector")
    revision = case.revisions[document]
    result = _transform(case, document, empty, {**IDENTITY, "a11": 0.001, "a13": 10})
    case.assertIs(result["changed"], False)
    case.assertEqual(case.revisions[document], revision)
    _radius_boundaries(case, document, target)
    _reject_raster(case, document)


def vector_transform_scales_radii_explicitly(case: NativeAcceptance) -> None:
    """Distinguish preserved widths from native determinant-scaled widths in geometry and pixels.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, target, _styles = _fixture(case)
    original = _geometry(case, document, target)
    shrink = {**IDENTITY, "a11": 0.5, "a22": 0.5}
    grow = {**IDENTITY, "a11": 2, "a22": 2}
    _transform(case, document, target, shrink)
    _transform(case, document, target, grow)
    _assert_geometry(case, original, _geometry(case, document, target), IDENTITY, 1)
    preserved = _preview(case, document)
    _transform(case, document, target, shrink)
    _transform(case, document, target, grow, "scale_geometric_mean")
    scaled = _preview(case, document)
    case.assertNotEqual(scaled, preserved)
    _paint_probes(case, scaled, [(-17, 7), (-12, 0), (10, -10), (12, 11)])
    geometry = _geometry(case, document, target)
    opened, restored = _reopen(case, document)
    _assert_geometry(
        case, geometry, _geometry(case, opened, restored), IDENTITY, 1, native_ids=False
    )
    case.assertEqual(_preview(case, opened), scaled)


def _assert_pli_precision(
    case: NativeAcceptance, before: dict[str, Any], after: dict[str, Any]
) -> None:
    case.assertEqual(after["total"], before["total"])
    for original, restored in zip(before["strokes"], after["strokes"], strict=True):
        case.assertEqual(restored["style_id"], original["style_id"])
        case.assertEqual(restored["closed"], original["closed"])
        # PLI encodes coordinate deltas at 1/16384 and each stroke's radii in 255 steps.
        coordinate_error = len(original["points"]) / 16384
        radius_error = math.ceil(max(point["thickness"] for point in original["points"])) / 255
        for source, point in zip(original["points"], restored["points"], strict=True):
            for axis in ("x", "y"):
                case.assertAlmostEqual(point[axis], source[axis], delta=coordinate_error)
            case.assertAlmostEqual(point["thickness"], source["thickness"], delta=radius_error)


def vector_transform_fractional_pli_precision(case: NativeAcceptance) -> None:
    """Qualify transformed fractional coordinates and radii against actual PLI precision.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, target, styles = _fixture(case)
    matrix = {**IDENTITY, "a11": 0.9, "a22": 0.9, "a13": 0.12345, "a23": 0.23456}
    _transform(case, document, target, matrix, "scale_geometric_mean")
    samples = [_point(matrix, x, y) for x, y in ((-18, 0), (-12, 0), (10, -10), (12, 11))]
    geometry = _geometry(case, document, target)
    _paint_probes(case, _preview(case, document), samples)
    opened, restored = _reopen(case, document)
    _assert_pli_precision(case, geometry, _geometry(case, opened, restored))
    _check_fills(case, opened, restored, samples, styles)
    _paint_probes(case, _preview(case, opened), samples)
