"""Compare editable artwork after native PLI, TLV, and PNG package round trips."""

from __future__ import annotations

import hashlib
import math
import struct
import subprocess
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


def _lzo_call(
    executable: Path, arguments: list[str], payload: bytes
) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [str(executable), *arguments],
        input=payload,
        capture_output=True,
        timeout=15,
        check=False,
    )


def check_lzo_roundtrip(test: NativeAcceptance) -> None:
    """Exercise the shipped LZO helpers with expanding packets and invalid input.

    Args:
        test: The running acceptance case and exact shipped executable path.
    """
    native = test.native_executable
    compressor = native.with_name("lzocompress" + native.suffix)
    decompressor = native.with_name("lzodecompress" + native.suffix)
    test.assertTrue(compressor.is_file(), "Native TLV compressor is missing")
    test.assertTrue(decompressor.is_file(), "Native TLV decompressor is missing")
    entropy = b"".join(hashlib.sha256(struct.pack("<I", index)).digest() for index in range(2048))
    for payload in (bytes(4), bytes(range(24)), entropy[:1024], entropy):
        with test.subTest(uncompressed_bytes=len(payload)):
            encoded = _lzo_call(compressor, [str(len(payload))], payload)
            test.assertEqual(encoded.returncode, 0, encoded.stderr.decode(errors="replace"))
            test.assertGreater(len(encoded.stdout), len(payload))
            test.assertLessEqual(len(encoded.stdout), len(payload) + len(payload) // 16 + 67)
            decoded = _lzo_call(
                decompressor, [str(len(payload)), str(len(encoded.stdout))], encoded.stdout
            )
            test.assertEqual(decoded.returncode, 0, decoded.stderr.decode(errors="replace"))
            test.assertEqual(decoded.stdout, payload)
    for size in ("", "-1", "+4", " 4", "4tail", "18446744073709551615", "9" * 40):
        with test.subTest(invalid_size=size):
            result = _lzo_call(compressor, [size], b"")
            test.assertNotEqual(result.returncode, 0)
            test.assertEqual(result.stdout, b"")
            test.assertIn(b"Invalid input buffer size", result.stderr)
    truncated = _lzo_call(compressor, ["24"], b"short")
    test.assertNotEqual(truncated.returncode, 0)
    test.assertEqual(truncated.stdout, b"")
    test.assertIn(b"Could not read", truncated.stderr)


def _reject_vector_numbers(test: NativeAcceptance, document: str, target: dict[str, Any]) -> None:
    original = test.success("scene.summary", {"document_id": document})
    for method, params, code in (
        (
            "drawing.create",
            {
                "level_id": target["level_id"],
                "drawing_id": {"number": 65536, "suffix": "a"},
                "format": "vector",
            },
            "unsupported_drawing_id",
        ),
        (
            "drawing.create",
            {
                "level_id": target["level_id"],
                "drawing_id": {"number": 1, "suffix": "z"},
                "format": "vector",
                "width": 2,
                "height": 2,
                "dpi": 120,
            },
            "invalid_params",
        ),
        (
            "drawing.clone",
            {**target, "destination_drawing_id": {"number": 65536, "suffix": "b"}},
            "unsupported_drawing_id",
        ),
    ):
        error = test.failure(method, test.write_params(document, params))
        test.assertEqual(error["code"], code)
        test.assertEqual(test.success("scene.summary", {"document_id": document}), original)


def _drawing_number_roundtrip(test: NativeAcceptance, image_format: str, maximum: int) -> None:
    document = test.create(f"{image_format} drawing number boundary")
    level = test.mutate("level.create", document, name="Number boundary", format=image_format)
    target = {"level_id": level["level_id"], "drawing_id": {"number": maximum, "suffix": "a"}}
    dimensions = {} if image_format == "vector" else {"width": 3, "height": 2, "dpi": DPI}
    test.mutate("drawing.create", document, **target, format=image_format, **dimensions)
    if image_format == "vector":
        test.mutate(
            "vector.edit",
            document,
            **target,
            coordinate_space="drawing_native",
            sharing="all_exposures",
            edits=[
                {
                    "action": "add",
                    "style_id": 1,
                    "points": [{"x": x, "y": 0, "thickness": 1} for x in (0, 1, 2)],
                }
            ],
        )
        _reject_vector_numbers(test, document, target)
    clone = {"number": maximum, "suffix": "b"}
    test.mutate("drawing.clone", document, **target, destination_drawing_id=clone)
    saved = test.mutate(
        "scene.save_copy",
        document,
        directory=str(test.workspace / f"number-limit-{image_format}"),
    )
    opened = test.mutate("scene.open", None, path=saved["package_directory"])
    other = opened["document_id"]
    summary = test.success("scene.summary", {"document_id": other})
    restored = summary["levels"][0]
    test.assertEqual(restored["drawings"], [target["drawing_id"], clone])
    for drawing in restored["drawings"]:
        reopened_target = {"level_id": restored["level_id"], "drawing_id": drawing}
        if image_format == "vector":
            _assert_geometry(
                test, _geometry(test, document, target), _geometry(test, other, reopened_target)
            )
        else:
            _assert_pixels(
                test,
                image_format,
                _pixels(test, document, target),
                _pixels(test, other, reopened_target),
            )


def check_drawing_number_limits(test: NativeAcceptance) -> None:
    """Persist native frame-number boundaries and reject overflowing PLI IDs.

    Args:
        test: The running acceptance case and its disposable native worker.
    """
    for image_format, maximum in (("vector", 65535), ("rgba8", 999999), ("toonzRaster", 999999)):
        with test.subTest(format=image_format, maximum_drawing_number=maximum):
            _drawing_number_roundtrip(test, image_format, maximum)


def _reject_palette_names(test: NativeAcceptance, document: str, level_id: str) -> None:
    palette = _palette(test, document, level_id)
    revision = test.revisions[document]
    for name in ("|paint", "paint\0hidden"):
        with test.subTest(invalid_style_name=repr(name)):
            error = test.failure(
                "palette.create_style",
                test.write_params(
                    document,
                    {
                        "level_id": level_id,
                        "name": name,
                        "color": {"r": 23, "g": 41, "b": 73, "a": 255},
                    },
                ),
            )
            test.assertEqual(error["code"], test.schema_code())
            test.assertEqual(_palette(test, document, level_id), palette)
            test.assertEqual(
                test.success("scene.summary", {"document_id": document})["revision"], revision
            )


def _palette_serialization_roundtrip(test: NativeAcceptance, image_format: str) -> None:
    document = test.create(f"{image_format} palette serialization")
    target = _level(test, document, image_format)
    level_id = target["level_id"]
    color = {"r": 23, "g": 41, "b": 73, "a": 255}
    style = test.mutate(
        "palette.create_style", document, level_id=level_id, name="色 Ω", color=color
    )
    test.assertEqual(style["name"], "色 Ω")
    _reject_palette_names(test, document, level_id)
    maximum = 65535 if image_format == "vector" else 999999
    test.mutate(
        "palette.update_style",
        document,
        level_id=level_id,
        style_id=style["style_id"],
        keyframe=maximum,
        color=color,
    )
    palette = _palette(test, document, level_id)
    test.assertEqual(palette["styles"][style["style_id"]]["keyframes"], [maximum])
    if image_format == "vector":
        revision = test.revisions[document]
        error = test.failure(
            "palette.update_style",
            test.write_params(
                document,
                {
                    "level_id": level_id,
                    "style_id": style["style_id"],
                    "keyframe": 65536,
                    "color": color,
                },
            ),
        )
        test.assertEqual(error["code"], "invalid_params")
        test.assertEqual(_palette(test, document, level_id), palette)
        test.assertEqual(
            test.success("scene.summary", {"document_id": document})["revision"], revision
        )
    other, restored = _reopen(test, document, f"palette-limit-{image_format}")
    test.assertEqual(_palette(test, other, restored["level_id"]), palette)


def check_palette_serialization_limits(test: NativeAcceptance) -> None:
    """Keep names and palette keyframes within their native serialized ranges.

    Args:
        test: The running acceptance case and its disposable native worker.
    """
    for image_format in ("vector", "toonzRaster"):
        with test.subTest(format=image_format):
            _palette_serialization_roundtrip(test, image_format)
