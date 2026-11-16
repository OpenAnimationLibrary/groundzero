"""Additional native scene cases shared by the real-worker acceptance suite."""

from __future__ import annotations

import base64
import struct
import zlib
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_png import Pixel, decode_png, fixture_pixel

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


def _summary(case: NativeAcceptance, document: str) -> dict[str, Any]:
    return case.success("scene.summary", {"document_id": document})


def _channel(object_id: str) -> dict[str, Any]:
    return {"object_id": object_id, "channel": "x", "unit": "inch"}


def _key(frame: int, value: float, interpolation: str = "linear") -> dict[str, Any]:
    return {"frame": frame, "value": value, "interpolation": interpolation}


def parenting_preserves_local_keys_and_changes_rendered_placement(case: NativeAcceptance) -> None:
    """Distinguish local curve values from the composed parent transform."""
    document, _level, column = case.populated_scene()
    target = _channel(column["object_id"])
    case.mutate("animation.set_keys", document, **target, keys=[_key(0, 2 / 120)])
    keys_before = case.success("animation.keys", {"document_id": document, **target})
    before = decode_png(case.preview(document))
    peg = case.mutate("object.create", document, name="Translate", kind="pegbar")
    case.mutate(
        "animation.set_keys", document, **_channel(peg["object_id"]), keys=[_key(0, 4 / 120)]
    )
    case.mutate(
        "object.parent", document, object_id=column["object_id"], parent_id=peg["object_id"]
    )
    case.assertEqual(
        case.success("animation.keys", {"document_id": document, **target}), keys_before
    )
    after = decode_png(case.preview(document))
    case.assertEqual(after[:2], before[:2])
    case.assertNotEqual(after[2], before[2])
    width = before[0]
    for x, y in ((10, 8), (24, 8), (10, 24), (24, 24)):
        case.assertEqual(after[2][y * width + x], before[2][y * width + x - 4], (x, y))
    case.assertEqual(after[2][16 * width + 2][3], 0)
    revision = case.revisions[document]
    repeated = case.mutate(
        "object.parent", document, object_id=column["object_id"], parent_id=peg["object_id"]
    )
    case.assertEqual(repeated["revision"], revision)
    table = next(item for item in _summary(case, document)["objects"] if item["kind"] == "table")
    case.mutate(
        "object.parent", document, object_id=column["object_id"], parent_id=table["object_id"]
    )
    case.assertEqual(decode_png(case.preview(document)), before)


def parenting_rejections_preserve_every_object(case: NativeAcceptance) -> None:
    """Reject self links, longer cycles, table parents and peg-to-column links."""
    document, _level, column = case.populated_scene()
    pegs = [
        case.mutate("object.create", document, name=name, kind="pegbar")["object_id"]
        for name in ("First", "Second", "Third")
    ]
    case.mutate("object.parent", document, object_id=pegs[0], parent_id=pegs[1])
    case.mutate("object.parent", document, object_id=pegs[1], parent_id=pegs[2])
    before = _summary(case, document)
    table = next(item["object_id"] for item in before["objects"] if item["kind"] == "table")
    rejected = (
        (pegs[2], pegs[0]),
        (pegs[0], pegs[0]),
        (table, pegs[0]),
        (pegs[2], column["object_id"]),
    )
    for child, parent in rejected:
        with case.subTest(child=child, parent=parent):
            case.failure(
                "object.parent",
                case.write_params(document, {"object_id": child, "parent_id": parent}),
            )
            case.assertEqual(_summary(case, document), before)


def locked_columns_and_deleted_handles_remain_consistent(case: NativeAcceptance) -> None:
    """Exercise lock-only writes, deletion dependencies and stale object/FX handles."""
    document, _level, column = case.populated_scene()
    column_id = column["column_id"]
    case.mutate("column.update", document, column_id=column_id, locked=True)
    revision = case.revisions[document]
    case.mutate("column.update", document, column_id=column_id, locked=True)
    case.assertEqual(case.revisions[document], revision)
    before = _summary(case, document)
    for method, fields in (
        ("column.update", {"name": "Changed", "locked": False}),
        ("column.delete", {}),
    ):
        case.failure(method, case.write_params(document, {"column_id": column_id, **fields}))
        case.assertEqual(_summary(case, document), before)
    case.mutate("column.update", document, column_id=column_id, locked=False)
    revision = case.revisions[document]
    case.mutate("column.update", document, column_id=column_id, locked=False)
    case.assertEqual(case.revisions[document], revision)
    child = case.mutate("column.create", document, name="Child")
    case.mutate(
        "object.parent", document, object_id=child["object_id"], parent_id=column["object_id"]
    )
    case.failure("column.delete", case.write_params(document, {"column_id": column_id}))
    table = next(item for item in _summary(case, document)["objects"] if item["kind"] == "table")
    case.mutate(
        "object.parent", document, object_id=child["object_id"], parent_id=table["object_id"]
    )
    graph = case.success("fx.graph", {"document_id": document})
    effect = next(item["fx_id"] for item in graph["effects"] if item.get("column_id") == column_id)
    case.mutate("column.delete", document, column_id=column_id)
    replacement = case.mutate("column.create", document, name="Replacement")
    case.assertNotEqual(replacement["column_id"], column_id)
    case.assertNotEqual(replacement["object_id"], column["object_id"])
    _assert_retired_handles(case, document, column, effect)
    case.success("animation.keys", {"document_id": document, **_channel(child["object_id"])})
    graph_after = case.success("fx.graph", {"document_id": document})
    case.assertNotIn(effect, [item["fx_id"] for item in graph_after["effects"]])
    case.assertEqual([item["position"] for item in _summary(case, document)["columns"]], [0, 1])


def _assert_retired_handles(
    case: NativeAcceptance, document: str, column: dict[str, Any], effect: str
) -> None:
    before = _summary(case, document)
    case.failure(
        "animation.sample",
        {"document_id": document, **_channel(column["object_id"]), "frames": [0]},
    )
    case.failure("fx.parameter.get", {"document_id": document, "fx_id": effect})
    case.failure("column.update", case.write_params(document, {"column_id": column["column_id"]}))
    case.assertEqual(_summary(case, document), before)


def animation_precision_and_batch_errors_preserve_existing_keys(case: NativeAcceptance) -> None:
    """Check constant/linear endpoints, integer frames, units and all-or-nothing deletion."""
    document = case.create()
    peg = case.mutate("object.create", document, name="Motion", kind="pegbar")
    target = _channel(peg["object_id"])
    keys = [_key(0, 3, "constant"), _key(1000000, 7)]
    case.mutate("animation.set_keys", document, **target, keys=keys)
    sampled = case.success(
        "animation.sample", {"document_id": document, **target, "frames": [0, 1, 999999, 1000000]}
    )
    case.assertEqual([item["value"] for item in sampled["values"]], [3, 3, 3, 7])
    revision = case.revisions[document]
    case.mutate("animation.set_keys", document, **target, keys=keys)
    case.assertEqual(case.revisions[document], revision)
    case.mutate("animation.set_keys", document, **target, keys=[_key(0, 3)])
    sampled = case.success(
        "animation.sample", {"document_id": document, **target, "frames": [0, 500000, 1000000]}
    )
    case.assertEqual([item["value"] for item in sampled["values"]], [3, 5, 7])
    _reject_animation_batches(case, document, target)
    case.mutate("animation.delete_keys", document, **target, frames=[0])
    remaining = case.success("animation.keys", {"document_id": document, **target})
    case.assertEqual(remaining["keys"], [_key(1000000, 7)])
    z_target = {"object_id": peg["object_id"], "channel": "z", "unit": "native_z"}
    case.mutate("animation.set_keys", document, **z_target, keys=[_key(0, 25)])
    case.assertEqual(
        case.success("animation.sample", {"document_id": document, **z_target, "frames": [0]})[
            "values"
        ],
        [{"frame": 0, "value": 25}],
    )


def _reject_animation_batches(
    case: NativeAcceptance, document: str, target: dict[str, Any]
) -> None:
    before = case.success("animation.keys", {"document_id": document, **target})
    revision = case.revisions[document]
    invalid = (
        ("animation.set_keys", {"keys": [_key(2, 10), _key(2, 20)]}),
        ("animation.set_keys", {"keys": [_key(2, 10), _key(3, 20, "unspecified")]}),
        ("animation.set_keys", {"keys": [_key(2, 10)], "unit": "degree"}),
        ("animation.set_keys", {"keys": [{**_key(2, 10), "frame": 2.5}]}),
        ("animation.set_keys", {"keys": [_key(1000001, 10)]}),
        ("animation.delete_keys", {"frames": [0, 99]}),
        ("animation.delete_keys", {"frames": [0, 0]}),
    )
    for method, changes in invalid:
        with case.subTest(method=method, changes=changes):
            case.failure(method, case.write_params(document, {**target, **changes}))
            case.assertEqual(
                case.success("animation.keys", {"document_id": document, **target}), before
            )
            case.assertEqual(_summary(case, document)["revision"], revision)


def custom_dpi_survives_reopen_and_remains_independent_of_camera(case: NativeAcceptance) -> None:
    """Preserve drawing DPI, compatible new frames and image size after a camera change."""
    document = case.create()
    case.mutate("camera.update", document, width_in=32 / 180, height_in=32 / 180)
    level = case.mutate("level.import", document, path=str(case.fixture), name="Custom", dpi=180)
    column = case.mutate("column.create", document, name="Artwork")
    case.mutate(
        "column.expose",
        document,
        column_id=column["column_id"],
        level_id=level["level_id"],
        drawing_id={"number": 1, "suffix": ""},
        start_frame=0,
        length=1,
    )
    source = case.success(
        "drawing.inspect",
        {
            "document_id": document,
            "level_id": level["level_id"],
            "drawing_id": {"number": 1, "suffix": ""},
        },
    )
    case.assertEqual(source["dpi"], [180, 180])
    before = decode_png(case.preview(document))
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "custom-dpi"))
    reopened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    summary = _summary(case, reopened)
    # Native camera serialization retains six significant digits at this size.
    for dimension in ("width_in", "height_in"):
        case.assertAlmostEqual(summary["camera"][dimension], 32 / 180, delta=5e-7)
    case.assertEqual(decode_png(case.preview(reopened)), before)
    target = {
        "level_id": summary["levels"][0]["level_id"],
        "drawing_id": {"number": 1, "suffix": ""},
    }
    drawing = case.success("drawing.inspect", {"document_id": reopened, **target})
    for dpi in drawing["dpi"]:
        case.assertAlmostEqual(dpi, 180, delta=0.02)
    case.mutate(
        "drawing.create",
        reopened,
        level_id=target["level_id"],
        drawing_id={"number": 2, "suffix": "a"},
        format="rgba8",
        width=32,
        height=32,
        dpi=180,
    )
    case.mutate("camera.update", reopened, width_in=64 / 180, height_in=64 / 180)
    width, height, pixels = decode_png(case.preview(reopened))
    case.assertEqual((width, height), (32, 32))
    case.assertEqual(pixels[2 * width + 2][3], 0)
    for x, y in ((12, 12), (20, 12), (12, 20), (20, 20)):
        case.assertEqual(pixels[y * width + x], fixture_pixel((x - 8) * 2, (y - 8) * 2, 32))
    case.assertEqual(decode_png(case.preview(document)), before)
    _resave_custom_dpi(case, reopened)


def _resave_custom_dpi(case: NativeAcceptance, document: str) -> None:
    summary = _summary(case, document)
    level_id = summary["levels"][0]["level_id"]
    drawings = [{"number": 1, "suffix": ""}, {"number": 2, "suffix": "a"}]
    rectangle = {
        "coordinate_space": "raster_pixels_lower_left",
        "x": 0,
        "y": 0,
        "width": 32,
        "height": 32,
    }
    edited_pixels = [{"r": 80, "g": 160, "b": 240, "a": 255}] * 1024
    case.mutate(
        "raster.patch",
        document,
        level_id=level_id,
        drawing_id=drawings[1],
        **rectangle,
        sharing="all_exposures",
        pixels=edited_pixels,
    )
    case.mutate(
        "column.expose",
        document,
        column_id=summary["columns"][0]["column_id"],
        level_id=level_id,
        drawing_id=drawings[1],
        start_frame=1,
        length=1,
    )
    before_pixels = [
        case.success(
            "raster.read_pixels",
            {"document_id": document, "level_id": level_id, "drawing_id": drawing, **rectangle},
        )["pixels"]
        for drawing in drawings
    ]
    case.assertEqual(before_pixels[1], edited_pixels)
    before_render = _render_dpi_frames(case, document)
    case.assertNotEqual(before_render[0], before_render[1])
    saved = case.mutate(
        "scene.save_copy", document, directory=str(case.workspace / "custom-dpi-edited")
    )
    reopened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    case.assertNotEqual(reopened, document)
    restored = _summary(case, reopened)
    case.assertEqual(restored["levels"][0]["drawings"], drawings)
    for dimension in ("width_in", "height_in"):
        case.assertAlmostEqual(restored["camera"][dimension], 64 / 180, delta=5e-7)
    cells = case.success(
        "column.cells",
        {
            "document_id": reopened,
            "column_id": restored["columns"][0]["column_id"],
            "start_frame": 0,
            "length": 2,
        },
    )
    case.assertEqual([cell["drawing_id"] for cell in cells["cells"]], drawings)
    for drawing, expected in zip(drawings, before_pixels, strict=True):
        target = {
            "document_id": reopened,
            "level_id": restored["levels"][0]["level_id"],
            "drawing_id": drawing,
        }
        description = case.success("drawing.inspect", target)
        case.assertEqual((description["width"], description["height"]), (32, 32))
        for dpi in description["dpi"]:
            case.assertAlmostEqual(dpi, 180, delta=0.02)
        case.assertEqual(
            case.success("raster.read_pixels", {**target, **rectangle})["pixels"], expected
        )
    case.assertEqual(_render_dpi_frames(case, reopened), before_render)


def _render_dpi_frames(case: NativeAcceptance, document: str) -> list[tuple[int, int, list[Pixel]]]:
    revision = case.revisions[document]
    job = case.mutate("render.submit", document, start_frame=0, end_frame=1, width=32, height=32)
    case.assertEqual(job["source_revision"], revision)
    status = case.wait_job(job["job_id"])
    case.assertEqual(status["status"], "completed", status)
    artifacts = case.success("render.artifacts", {"job_id": job["job_id"]})["artifacts"]
    case.assertEqual([artifact["frame"] for artifact in artifacts], [0, 1])
    frames = []
    for artifact in artifacts:
        result = case.success("artifact.read", {"artifact_id": artifact["artifact_id"]})
        case.assertEqual(result["source_revision"], revision)
        frames.append(decode_png(base64.b64decode(result["image"]["data"], validate=True)))
    case.assertEqual(case.revisions[document], revision)
    return frames


def png_import_dimension_bounds_preserve_scene(case: NativeAcceptance) -> None:
    """Admit savable per-axis bounds and reject oversized strips before native writes.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create()
    for width, height in ((8192, 1), (1, 8192)):
        path = case.inputs / f"valid-{width}x{height}.png"
        _write_strip_png(path, width, height)
        level = case.mutate("level.import", document, path=str(path), name=path.stem, dpi=120)
        drawing = case.success(
            "drawing.inspect",
            {
                "document_id": document,
                "level_id": level["level_id"],
                "drawing_id": {"number": 1, "suffix": ""},
            },
        )
        case.assertEqual((drawing["width"], drawing["height"]), (width, height))
    before = _summary(case, document)
    for width, height in ((8193, 1), (1, 8193)):
        path = case.inputs / f"oversized-{width}x{height}.png"
        _write_strip_png(path, width, height)
        error = case.failure(
            "level.import",
            case.write_params(document, {"path": str(path), "name": path.stem, "dpi": 120}),
        )
        case.assertEqual(error["code"], "resource_limit")
        case.assertEqual(_summary(case, document), before)
    saved = case.mutate(
        "scene.save_copy", document, directory=str(case.workspace / "dimension-boundaries")
    )
    reopened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    levels = _summary(case, reopened)["levels"]
    case.assertEqual(len(levels), 2)
    dimensions = set()
    for level in levels:
        drawing = case.success(
            "drawing.inspect",
            {
                "document_id": reopened,
                "level_id": level["level_id"],
                "drawing_id": {"number": 1, "suffix": ""},
            },
        )
        dimensions.add((drawing["width"], drawing["height"]))
    case.assertEqual(dimensions, {(8192, 1), (1, 8192)})


def _write_strip_png(path: Path, width: int, height: int) -> None:
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    row = b"\x00" + bytes([80, 160, 240, 255]) * width
    chunks = ((b"IHDR", header), (b"IDAT", zlib.compress(row * height)), (b"IEND", b""))
    data = bytearray(b"\x89PNG\r\n\x1a\n")
    for kind, payload in chunks:
        data.extend(struct.pack(">I", len(payload)))
        data.extend(kind + payload)
        data.extend(struct.pack(">I", zlib.crc32(kind + payload)))
    path.write_bytes(data)


def level_capacity_rejection_preserves_scene(case: NativeAcceptance) -> None:
    """Reject level creation and PNG intake beyond the native package level limit.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create()
    for index in range(256):
        case.mutate("level.create", document, name=f"Level {index}", format="rgba8")
    before = _summary(case, document)
    case.assertEqual(len(before["levels"]), 256)
    for method, fields in (
        ("level.create", {"format": "vector"}),
        ("level.import", {"path": str(case.fixture), "dpi": 120}),
    ):
        with case.subTest(method=method):
            case.failure(method, case.write_params(document, {"name": "Overflow", **fields}))
            case.assertEqual(_summary(case, document), before)
