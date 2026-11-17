"""Verify animated paint after palette copying, drawing renumbering, and vector transforms."""

from __future__ import annotations

import base64
import hashlib
from typing import TYPE_CHECKING, Any

from mcp_png import Pixel, decode_png

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


DRAWING = {"number": 1, "suffix": ""}
RENAMED = {"number": 42, "suffix": "b"}
SOURCE_COLORS: dict[int, Pixel] = {
    0: (220, 40, 60, 255),
    6: (130, 60, 140, 255),
    12: (40, 80, 220, 255),
}
COPY_COLORS: dict[int, Pixel] = {
    0: (220, 40, 60, 255),
    6: (130, 130, 80, 255),
    12: (40, 220, 100, 255),
}
MATRIX = {"a11": 0, "a12": -1, "a13": 3, "a21": 1, "a22": 0, "a23": 2}


def _color(pixel: Pixel) -> dict[str, int]:
    return dict(zip(("r", "g", "b", "a"), pixel, strict=True))


def _rectangle(left: int, right: int) -> dict[str, Any]:
    middle = (left + right) / 2
    points = (
        (left, -8),
        (middle, -8),
        (right, -8),
        (right, 0),
        (right, 8),
        (middle, 8),
        (left, 8),
        (left, 0),
        (left, -8),
    )
    return {
        "action": "add",
        "closed": True,
        "style_id": 1,
        "points": [{"x": x, "y": y, "thickness": 1} for x, y in points],
    }


def _fill(
    case: NativeAcceptance,
    document: str,
    target: dict[str, Any],
    point: tuple[int, int],
    style: int,
) -> dict[str, Any]:
    return case.mutate(
        "vector.fill",
        document,
        **target,
        coordinate_space="drawing_native",
        sharing="all_exposures",
        x=point[0],
        y=point[1],
        style_id=style,
    )


def _animated_styles(case: NativeAcceptance, document: str, level: str) -> tuple[int, int]:
    source = case.mutate(
        "palette.create_style",
        document,
        level_id=level,
        name="Source animation",
        color=_color(SOURCE_COLORS[0]),
    )["style_id"]
    for frame in (0, 12):
        case.mutate(
            "palette.update_style",
            document,
            level_id=level,
            style_id=source,
            keyframe=frame,
            color=_color(SOURCE_COLORS[frame]),
        )
    copied = case.mutate(
        "palette.clone_style",
        document,
        level_id=level,
        style_id=source,
        page_index=0,
        name="Independent copied animation",
    )["style_id"]
    case.assertNotEqual(source, copied)
    case.mutate(
        "palette.update_style",
        document,
        level_id=level,
        style_id=copied,
        keyframe=12,
        color=_color(COPY_COLORS[12]),
    )
    return source, copied


def _fixture(case: NativeAcceptance) -> tuple[str, dict[str, Any], tuple[int, int]]:
    document = case.mutate(
        "scene.create",
        None,
        name="Combined editable artwork",
        width_px=64,
        height_px=64,
        width_in=1.2,
        height_in=1.2,
        frame_rate=24,
    )["document_id"]
    level = case.mutate("level.create", document, name="Two animated regions", format="vector")
    target = {"level_id": level["level_id"], "drawing_id": DRAWING}
    case.mutate("drawing.create", document, **target, format="vector")
    case.mutate(
        "vector.edit",
        document,
        **target,
        coordinate_space="drawing_native",
        sharing="all_exposures",
        edits=[_rectangle(-22, -6), _rectangle(4, 20)],
    )
    styles = _animated_styles(case, document, level["level_id"])
    _fill(case, document, target, (-14, 0), styles[0])
    _fill(case, document, target, (12, 0), styles[1])
    column = case.mutate("column.create", document, name="Held painted drawing")
    case.mutate(
        "column.expose",
        document,
        **target,
        column_id=column["column_id"],
        start_frame=0,
        length=13,
    )
    return document, target, styles


def _preview(case: NativeAcceptance, document: str, frame: int) -> list[Pixel]:
    job = case.mutate("render.preview", document, frame=frame, width=64, height=64)
    status = case.wait_job(job["job_id"])
    case.assertEqual(status["status"], "completed", status)
    case.assertIs(status["writers_closed"], True)
    artifacts = case.success("render.artifacts", {"job_id": job["job_id"]})["artifacts"]
    case.assertEqual(len(artifacts), 1)
    result = case.success("artifact.read", {"artifact_id": artifacts[0]["artifact_id"]})
    case.assertEqual(result["source_revision"], case.revisions[document])
    case.assertEqual(result["frame"], frame)
    data = base64.b64decode(result["image"]["data"], validate=True)
    case.assertEqual(hashlib.sha256(data).hexdigest(), artifacts[0]["sha256"])
    width, height, pixels = decode_png(data)
    case.assertEqual((width, height), (64, 64))
    return pixels


def _assert_colors(
    case: NativeAcceptance, pixels: list[Pixel], frame: int, transformed: bool
) -> None:
    # At this camera size one native unit is effectively one pixel; use flat interior blocks.
    centers = ((35, 44), (35, 18)) if transformed else ((18, 32), (44, 32))
    for (x, y), color in zip(centers, (SOURCE_COLORS[frame], COPY_COLORS[frame]), strict=True):
        for row in range(y - 1, y + 2):
            for column in range(x - 1, x + 2):
                case.assertEqual(pixels[row * 64 + column], color, (frame, column, row))
    case.assertEqual(pixels[2 * 64 + 2][3], 0)
    if transformed:
        for x in (18, 44):
            case.assertEqual(pixels[32 * 64 + x][3], 0, "Old region placement remained visible")


def _samples(
    case: NativeAcceptance, document: str, *, transformed: bool = False
) -> dict[int, list[Pixel]]:
    frames = {}
    for frame in SOURCE_COLORS:
        pixels = _preview(case, document, frame)
        _assert_colors(case, pixels, frame, transformed)
        frames[frame] = pixels
    case.assertNotEqual(frames[0], frames[6])
    case.assertNotEqual(frames[6], frames[12])
    return frames


def _assert_exposures(case: NativeAcceptance, document: str, target: dict[str, Any]) -> None:
    summary = case.success("scene.summary", {"document_id": document})
    case.assertEqual(len(summary["columns"]), 1)
    cells = case.success(
        "column.cells",
        {
            "document_id": document,
            "column_id": summary["columns"][0]["column_id"],
            "start_frame": 0,
            "length": 13,
        },
    )["cells"]
    case.assertEqual([cell["drawing_id"] for cell in cells], [target["drawing_id"]] * 13)
    case.assertEqual({cell["level_id"] for cell in cells}, {target["level_id"]})


def _assert_geometry(case: NativeAcceptance, before: dict[str, Any], after: dict[str, Any]) -> None:
    case.assertEqual(before["total"], 2)
    case.assertEqual(after["total"], 2)
    for source, transformed in zip(before["strokes"], after["strokes"], strict=True):
        for key in ("index", "native_id", "style_id", "closed"):
            case.assertEqual(transformed[key], source[key])
        for original, point in zip(source["points"], transformed["points"], strict=True):
            case.assertEqual(point["x"], -original["y"] + 3)
            case.assertEqual(point["y"], original["x"] + 2)
            case.assertEqual(point["thickness"], original["thickness"])


def _assert_fill_ids(
    case: NativeAcceptance, document: str, target: dict[str, Any], styles: tuple[int, int]
) -> None:
    revision = case.revisions[document]
    for point, style in zip(((3, -12), (3, 14)), styles, strict=True):
        result = _fill(case, document, target, point, style)
        case.assertEqual(result["previous_style_id"], style)
        case.assertIs(result["changed"], False)
    case.assertEqual(case.revisions[document], revision)


def artwork_transform_renumber_palette_roundtrip(case: NativeAcceptance) -> None:
    """Render independently edited animated paint after renumber, transform, and PLI reopen.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, target, styles = _fixture(case)
    originals = _samples(case, document)
    palette = case.success(
        "palette.inspect", {"document_id": document, "level_id": target["level_id"]}
    )
    renamed = case.mutate(
        "drawing.renumber",
        document,
        level_id=target["level_id"],
        sharing="all_exposures",
        mapping=[{"drawing_id": DRAWING, "destination_drawing_id": RENAMED}],
    )
    case.assertEqual(renamed["drawings_renumbered"], 1)
    case.assertEqual(renamed["exposures_updated"], 13)
    case.assertEqual(
        case.failure("drawing.inspect", {"document_id": document, **target})["code"], "not_found"
    )
    target = {**target, "drawing_id": RENAMED}
    _assert_exposures(case, document, target)
    case.assertEqual(_samples(case, document), originals)
    before = case.success("vector.inspect", {"document_id": document, **target})
    transformed = case.mutate(
        "vector.transform",
        document,
        **target,
        coordinate_space="drawing_native",
        sharing="all_exposures",
        matrix=MATRIX,
        thickness_mode="preserve",
    )
    case.assertIs(transformed["changed"], True)
    after = case.success("vector.inspect", {"document_id": document, **target})
    _assert_geometry(case, before, after)
    _assert_fill_ids(case, document, target, styles)
    _assert_exposures(case, document, target)
    expected = _samples(case, document, transformed=True)
    saved = case.mutate(
        "scene.save_copy", document, directory=str(case.workspace / "compound-artwork")
    )
    case.mutate("scene.discard", document)
    opened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    case.assertNotEqual(opened, document)
    summary = case.success("scene.summary", {"document_id": opened})
    case.assertEqual(len(summary["levels"]), 1)
    case.assertEqual(summary["levels"][0]["drawings"], [RENAMED])
    target = {"level_id": summary["levels"][0]["level_id"], "drawing_id": RENAMED}
    _assert_exposures(case, opened, target)
    _assert_fill_ids(case, opened, target, styles)
    case.assertEqual(
        case.success("palette.inspect", {"document_id": opened, "level_id": target["level_id"]}),
        palette,
    )
    case.assertEqual(_samples(case, opened, transformed=True), expected)
