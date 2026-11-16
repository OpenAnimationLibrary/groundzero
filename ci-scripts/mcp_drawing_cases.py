"""Compare editable artwork after native PLI, TLV, and PNG package round trips."""

from __future__ import annotations

import math
import struct
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_png import decode_png

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


DRAWING = {"number": 7, "suffix": "ab"}
DPI = 120.25
RECTANGLE = {
    "coordinate_space": "raster_pixels_lower_left",
    "x": 0,
    "y": 0,
    "width": 3,
    "height": 2,
}
RGBA = [
    {"r": 128, "g": 17, "b": 1, "a": 254},
    {"r": 64, "g": 32, "b": 0, "a": 128},
    {"r": 255, "g": 0, "b": 64, "a": 255},
    {"r": 0, "g": 0, "b": 0, "a": 0},
    {"r": 1, "g": 1, "b": 1, "a": 1},
    {"r": 0, "g": 127, "b": 0, "a": 127},
]
INDEXED = [
    {"ink": 1, "paint": 2, "tone": 127},
    {"ink": 0, "paint": 2, "tone": 255},
    {"ink": 2, "paint": 1, "tone": 0},
    {"ink": 0, "paint": 0, "tone": 255},
    {"ink": 2, "paint": 2, "tone": 64},
    {"ink": 1, "paint": 0, "tone": 254},
]


def _level(test: NativeAcceptance, document: str, image_format: str) -> dict[str, Any]:
    level = test.mutate("level.create", document, name="Artwork", format=image_format)
    target = {"level_id": level["level_id"], "drawing_id": DRAWING}
    dimensions = {} if image_format == "vector" else {"width": 3, "height": 2, "dpi": DPI}
    test.mutate("drawing.create", document, **target, format=image_format, **dimensions)
    column = test.mutate("column.create", document, name="Held artwork")
    test.mutate(
        "column.expose",
        document,
        **target,
        column_id=column["column_id"],
        start_frame=0,
        length=3,
    )
    return target


def _palette(test: NativeAcceptance, document: str, level_id: str) -> dict[str, Any]:
    return test.success("palette.inspect", {"document_id": document, "level_id": level_id})


def _style(test: NativeAcceptance, document: str, level_id: str) -> int:
    style = test.mutate(
        "palette.create_style",
        document,
        level_id=level_id,
        name="Editable paint",
        color={"r": 23, "g": 41, "b": 73, "a": 255},
    )
    return style["style_id"]


def _reopen(test: NativeAcceptance, document: str, label: str) -> tuple[str, dict[str, Any]]:
    revision = test.revisions[document]
    saved = test.mutate(
        "scene.save_copy", document, directory=str(test.workspace / f"artwork-{label}")
    )
    test.assertEqual(test.revisions[document], revision)
    opened = test.mutate("scene.open", None, path=saved["package_directory"])
    other = opened["document_id"]
    summary = test.success("scene.summary", {"document_id": other})
    test.assertEqual(len(summary["levels"]), 1)
    level = summary["levels"][0]
    test.assertEqual(level["drawings"], [DRAWING])
    cells = test.success(
        "column.cells",
        {
            "document_id": other,
            "column_id": summary["columns"][0]["column_id"],
            "start_frame": 0,
            "length": 3,
        },
    )
    test.assertEqual([cell["drawing_id"] for cell in cells["cells"]], [DRAWING] * 3)
    return other, {"level_id": level["level_id"], "drawing_id": DRAWING}


def _geometry(test: NativeAcceptance, document: str, target: dict[str, Any]) -> dict[str, Any]:
    return test.success("vector.inspect", {"document_id": document, **target})


def _assert_geometry(
    test: NativeAcceptance, source: dict[str, Any], opened: dict[str, Any]
) -> None:
    test.assertEqual(opened["total"], source["total"])
    test.assertEqual(opened["coordinate_space"], source["coordinate_space"])
    for before, after in zip(source["strokes"], opened["strokes"], strict=True):
        test.assertEqual(after["style_id"], before["style_id"])
        test.assertEqual(after["closed"], before["closed"])
        test.assertEqual(len(after["points"]), len(before["points"]))
        # PLI stores fixed-point deltas at 1/16384 and widths in 255 steps.
        coordinate_tolerance = len(before["points"]) / 16384
        width_tolerance = math.ceil(max(point["thickness"] for point in before["points"])) / 255
        for original, restored in zip(before["points"], after["points"], strict=True):
            for axis in ("x", "y"):
                test.assertAlmostEqual(original[axis], restored[axis], delta=coordinate_tolerance)
            test.assertAlmostEqual(
                original["thickness"], restored["thickness"], delta=width_tolerance
            )


def _reject_unserializable_geometry(
    test: NativeAcceptance,
    document: str,
    target: dict[str, Any],
    stroke: dict[str, Any],
) -> None:
    original = _geometry(test, document, target)
    revision = test.revisions[document]
    for bad_point in (
        {"x": 32769, "y": 0, "thickness": 1},
        {"x": 0, "y": -32769, "thickness": 1},
        {"x": 0, "y": 0, "thickness": 256},
        {"x": 0, "y": 0, "thickness": -1},
    ):
        points = [dict(point) for point in stroke["points"]]
        points[1] = bad_point
        test.failure(
            "vector.edit",
            test.write_params(
                document,
                {
                    **target,
                    "coordinate_space": "drawing_native",
                    "sharing": "all_exposures",
                    "edits": [{**stroke, "action": "update", "index": 0, "points": points}],
                },
            ),
        )
        test.assertEqual(_geometry(test, document, target), original)
        test.assertEqual(
            test.success("scene.summary", {"document_id": document})["revision"],
            revision,
        )


def check_vector_roundtrip(test: NativeAcceptance) -> None:
    """Check native geometry, region paint, and animated palette persistence.

    Args:
        test: The running acceptance case and its disposable native worker.
    """
    document = test.create("Vector roundtrip")
    target = _level(test, document, "vector")
    style_id = _style(test, document, target["level_id"])
    colors = (
        (0, {"r": 23, "g": 41, "b": 73, "a": 255}),
        (12, {"r": 180, "g": 80, "b": 25, "a": 160}),
        (24, {"r": 15, "g": 210, "b": 90, "a": 255}),
    )
    for frame, color in colors:
        test.mutate(
            "palette.update_style",
            document,
            level_id=target["level_id"],
            style_id=style_id,
            keyframe=frame,
            color=color,
        )
    coordinates = (
        (-8.12345, -6.25),
        (0, -6.25),
        (8.125, -6.25),
        (8.125, 0),
        (8.125, 6.25),
        (0, 6.25),
        (-8.12345, 6.25),
        (-8.12345, 0),
        (-8.12345, -6.25),
    )
    widths = (0.6, 1.2, 2.4, 3.1, 0.9, 1.7, 2.5, 1.1, 0.6)
    points = [
        {"x": x, "y": y, "thickness": width}
        for (x, y), width in zip(coordinates, widths, strict=True)
    ]
    stroke = {"action": "add", "style_id": 1, "closed": True, "points": points}
    edit = {**target, "coordinate_space": "drawing_native", "sharing": "all_exposures"}
    test.mutate("vector.edit", document, **edit, edits=[stroke])
    _reject_unserializable_geometry(test, document, target, stroke)
    test.mutate("vector.fill", document, **edit, x=0, y=0, style_id=style_id)
    original_geometry = _geometry(test, document, target)
    original_palette = _palette(test, document, target["level_id"])
    other, restored = _reopen(test, document, "vector")
    _assert_geometry(test, original_geometry, _geometry(test, other, restored))
    test.assertEqual(_palette(test, other, restored["level_id"]), original_palette)
    revision = test.revisions[other]
    filled = test.mutate(
        "vector.fill",
        other,
        **restored,
        coordinate_space="drawing_native",
        sharing="all_exposures",
        x=0,
        y=0,
        style_id=style_id,
    )
    test.assertEqual(filled["previous_style_id"], style_id)
    test.assertFalse(filled["changed"])
    test.assertEqual(test.revisions[other], revision)
    test.mutate(
        "palette.update_style",
        other,
        level_id=restored["level_id"],
        style_id=style_id,
        keyframe=12,
        color={"r": 240, "g": 20, "b": 30, "a": 255},
    )
    test.assertEqual(_palette(test, document, target["level_id"]), original_palette)


def _pixels(test: NativeAcceptance, document: str, target: dict[str, Any]) -> dict[str, Any]:
    return test.success("raster.read_pixels", {"document_id": document, **target, **RECTANGLE})


def _assert_pixels(
    test: NativeAcceptance,
    image_format: str,
    source: dict[str, Any],
    restored: dict[str, Any],
) -> None:
    test.assertEqual(restored["row_order"], source["row_order"])
    test.assertEqual(restored["channels"], source["channels"])
    if image_format == "toonzRaster":
        test.assertEqual(restored["pixels"], source["pixels"])
        return
    for before, after in zip(source["pixels"], restored["pixels"], strict=True):
        test.assertEqual(after["a"], before["a"])
        # Native PNG straight-alpha encoding can change premultiplied RGB by one byte.
        tolerance = 0 if before["a"] in (0, 255) else 1
        for channel in ("r", "g", "b"):
            test.assertAlmostEqual(after[channel], before[channel], delta=tolerance)


def _assert_png_dpi(test: NativeAcceptance, path: Path) -> None:
    data = path.read_bytes()
    decode_png(data)
    offset = 8
    while offset < len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4 : offset + 8]
        if kind == b"pHYs":
            x, y, unit = struct.unpack_from(">IIB", data, offset + 8)
            test.assertEqual(unit, 1)
            for value in (x, y):
                test.assertAlmostEqual(value * 0.0254, DPI, delta=0.0254)
            return
        offset += 12 + length
    test.fail("Native PNG export omitted the authored image DPI")


def _png_roundtrip(test: NativeAcceptance, document: str, target: dict[str, Any]) -> None:
    source = _pixels(test, document, target)
    revision = test.revisions[document]
    exported = test.mutate(
        "raster.export_png",
        document,
        **target,
        path=str(test.workspace / "fractional-dpi.png"),
    )
    test.assertEqual(test.revisions[document], revision)
    _assert_png_dpi(test, Path(exported["path"]))
    imported = {
        "level_id": target["level_id"],
        "drawing_id": {"number": 9, "suffix": "c"},
    }
    test.mutate("raster.import_png", document, **imported, path=exported["path"], dpi=DPI)
    _assert_pixels(test, "rgba8", source, _pixels(test, document, imported))
    description = test.success("drawing.inspect", {"document_id": document, **imported})
    test.assertEqual(description["dpi"], [DPI, DPI])


def _raster_roundtrip(test: NativeAcceptance, image_format: str) -> None:
    document = test.create(f"{image_format} roundtrip")
    target = _level(test, document, image_format)
    if image_format == "toonzRaster":
        test.assertEqual(_style(test, document, target["level_id"]), 2)
    pixels = RGBA if image_format == "rgba8" else INDEXED
    test.mutate(
        "raster.patch",
        document,
        **target,
        **RECTANGLE,
        sharing="all_exposures",
        pixels=pixels,
    )
    original = _pixels(test, document, target)
    test.assertEqual(original["pixels"], pixels)
    other, restored = _reopen(test, document, image_format)
    _assert_pixels(test, image_format, original, _pixels(test, other, restored))
    description = test.success("drawing.inspect", {"document_id": other, **restored})
    test.assertEqual(description["dpi"], [DPI, DPI])
    test.assertEqual((description["width"], description["height"]), (3, 2))
    test.mutate(
        "drawing.create",
        other,
        level_id=restored["level_id"],
        drawing_id={"number": 8, "suffix": "c"},
        format=image_format,
        width=3,
        height=2,
        dpi=DPI,
    )
    if image_format == "toonzRaster":
        test.assertEqual(
            _palette(test, other, restored["level_id"]),
            _palette(test, document, target["level_id"]),
        )
    else:
        _png_roundtrip(test, other, restored)


def check_rgba_roundtrip(test: NativeAcceptance) -> None:
    """Check premultiplied RGBA8 pixels and authored DPI across PNG persistence.

    Args:
        test: The running acceptance case and its disposable native worker.
    """
    _raster_roundtrip(test, "rgba8")


def check_toonz_raster_roundtrip(test: NativeAcceptance) -> None:
    """Check exact native ink, paint, tone, palette, and DPI after TLV persistence.

    Args:
        test: The running acceptance case and its disposable native worker.
    """
    _raster_roundtrip(test, "toonzRaster")
