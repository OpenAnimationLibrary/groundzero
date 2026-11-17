"""Qualify simultaneous native drawing renumbering and its held exposures."""

from __future__ import annotations

import base64
import hashlib
import json
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_png import Pixel, decode_png

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


FRAMES = [
    {"number": 1, "suffix": "a"},
    {"number": 7, "suffix": "ab"},
    {"number": 13, "suffix": ""},
    {"number": 21, "suffix": "z"},
]
DPI = 120.25
RENDER_FRAMES = (0, 2, 4, 6, 7)
RECTANGLE = {
    "coordinate_space": "raster_pixels_lower_left",
    "x": 0,
    "y": 0,
    "width": 2,
    "height": 2,
}


def _mapping(source: dict[str, Any], destination: dict[str, Any]) -> dict[str, Any]:
    return {"drawing_id": source, "destination_drawing_id": destination}


def _paint(test: NativeAcceptance, document: str, target: dict[str, Any], index: int) -> None:
    drawing = test.success("drawing.inspect", {"document_id": document, **target})
    if drawing["format"] == "vector":
        points = [{"x": x, "y": index * 3, "thickness": 1 + index} for x in (-8, 0, 8)]
        test.mutate(
            "vector.edit",
            document,
            **target,
            coordinate_space="drawing_native",
            sharing="all_exposures",
            edits=[{"action": "add", "style_id": 2, "closed": False, "points": points}],
        )
        return
    if drawing["format"] == "rgba8":
        pixel = {"r": 40 + index * 40, "g": 220 - index * 40, "b": index * 50, "a": 255}
    else:
        pixel = {"ink": 1, "paint": 2, "tone": 30 + index * 60}
    test.mutate(
        "raster.patch", document, **target, **RECTANGLE, sharing="all_exposures", pixels=[pixel] * 4
    )


def _make_scene(test: NativeAcceptance, image_format: str) -> tuple[str, str]:
    document = test.create(f"Renumber {image_format}")
    level = test.mutate("level.create", document, name="Mapped artwork", format=image_format)
    level_id = level["level_id"]
    if image_format != "rgba8":
        style = test.mutate(
            "palette.create_style",
            document,
            level_id=level_id,
            name="Preserved paint",
            color={"r": 80, "g": 140, "b": 220, "a": 255},
        )
        test.assertEqual(style["style_id"], 2)
        test.mutate(
            "palette.update_style",
            document,
            level_id=level_id,
            style_id=2,
            keyframe=7,
            color={"r": 220, "g": 60, "b": 30, "a": 255},
        )
    dimensions = {} if image_format == "vector" else {"width": 2, "height": 2, "dpi": DPI}
    for index, frame in enumerate(FRAMES):
        target = {"level_id": level_id, "drawing_id": frame}
        test.mutate("drawing.create", document, **target, format=image_format, **dimensions)
        _paint(test, document, target, index)
    for name in ("First held use", "Second held use"):
        column = test.mutate("column.create", document, name=name)
        for index, frame in enumerate(FRAMES):
            test.mutate(
                "column.expose",
                document,
                column_id=column["column_id"],
                level_id=level_id,
                drawing_id=frame,
                start_frame=index * 2,
                length=2,
            )
    return document, level_id


def _art(test: NativeAcceptance, document: str, target: dict[str, Any]) -> dict[str, Any]:
    result = test.success("drawing.inspect", {"document_id": document, **target})
    result.pop("drawing_id")
    if result["format"] == "vector":
        geometry = test.success("vector.inspect", {"document_id": document, **target})
        result["strokes"] = geometry["strokes"]
    else:
        pixels = test.success(
            "raster.read_pixels", {"document_id": document, **target, **RECTANGLE}
        )
        result["pixels"] = pixels["pixels"]
    return result


def _assert_art(test: NativeAcceptance, before: dict[str, Any], after: dict[str, Any]) -> None:
    if before["format"] == "vector":
        test.assertEqual(len(before["strokes"]), len(after["strokes"]))
        for original, restored in zip(before["strokes"], after["strokes"], strict=True):
            test.assertEqual(original["style_id"], restored["style_id"])
            test.assertEqual(original["closed"], restored["closed"])
            test.assertEqual(len(original["points"]), len(restored["points"]))
            for first, second in zip(original["points"], restored["points"], strict=True):
                for axis in ("x", "y"):
                    test.assertAlmostEqual(first[axis], second[axis], delta=3 / 16384)
                test.assertAlmostEqual(first["thickness"], second["thickness"], delta=4 / 255)
        test.assertEqual(
            {key: value for key, value in before.items() if key != "strokes"},
            {key: value for key, value in after.items() if key != "strokes"},
        )
        return
    for original, restored in zip(before["dpi"], after["dpi"], strict=True):
        test.assertAlmostEqual(original, restored, delta=0.0254)
    test.assertEqual(
        {key: value for key, value in before.items() if key != "dpi"},
        {key: value for key, value in after.items() if key != "dpi"},
    )


def _cells(test: NativeAcceptance, document: str, expected: list[dict[str, Any]]) -> None:
    summary = test.success("scene.summary", {"document_id": document})
    for column in summary["columns"]:
        cells = test.success(
            "column.cells",
            {
                "document_id": document,
                "column_id": column["column_id"],
                "start_frame": 0,
                "length": 8,
            },
        )
        test.assertEqual(
            [cell["drawing_id"] for cell in cells["cells"]],
            [frame for frame in expected for _ in range(2)],
        )


def _save(test: NativeAcceptance, document: str, name: str) -> Path:
    result = test.mutate("scene.save_copy", document, directory=str(test.workspace / name))
    return Path(result["package_directory"])


def _open(test: NativeAcceptance, package: Path) -> tuple[str, str]:
    document = test.mutate("scene.open", None, path=str(package))["document_id"]
    summary = test.success("scene.summary", {"document_id": document})
    test.assertEqual(len(summary["levels"]), 1)
    return document, summary["levels"][0]["level_id"]


def _files(package: Path) -> dict[str, str]:
    return {
        str(path.relative_to(package)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in package.rglob("*")
        if path.is_file()
    }


def _rendered_frames(test: NativeAcceptance, document: str) -> dict[int, list[Pixel]]:
    revision = test.revisions[document]
    job = test.mutate("render.submit", document, start_frame=0, end_frame=7, width=32, height=32)
    test.assertEqual(job["source_revision"], revision)
    status = test.wait_job(job["job_id"])
    test.assertEqual(status["status"], "completed", status)
    test.assertTrue(status["writers_closed"])
    test.assertEqual(status["completed_frames"], 8)
    artifacts = test.success("render.artifacts", {"job_id": job["job_id"]})["artifacts"]
    test.assertEqual(sorted(artifact["frame"] for artifact in artifacts), list(range(8)))
    rendered = {}
    for artifact in artifacts:
        frame = artifact["frame"]
        if frame not in RENDER_FRAMES:
            continue
        result = test.success("artifact.read", {"artifact_id": artifact["artifact_id"]})
        test.assertEqual(result["frame"], frame)
        test.assertEqual(result["source_revision"], revision)
        test.assertEqual(result["image"]["mimeType"], "image/png")
        data = base64.b64decode(result["image"]["data"], validate=True)
        test.assertEqual(hashlib.sha256(data).hexdigest(), artifact["sha256"])
        width, height, pixels = decode_png(data)
        test.assertEqual((width, height), (32, 32))
        rendered[frame] = pixels
    test.assertEqual(test.revisions[document], revision)
    return rendered


def _render_baseline(test: NativeAcceptance, document: str) -> dict[int, list[Pixel]]:
    rendered = _rendered_frames(test, document)
    test.assertEqual(set(rendered), set(RENDER_FRAMES))
    for frame, pixels in rendered.items():
        test.assertTrue(any(pixel[3] for pixel in pixels), f"Frame {frame} has no visible artwork")
        test.assertGreater(len(set(pixels)), 1, f"Frame {frame} has no visible artwork boundary")
    test.assertEqual(
        len({tuple(rendered[frame]) for frame in RENDER_FRAMES[:4]}),
        4,
        "Each exposed drawing must render a distinct image before renumbering",
    )
    return rendered


def _assert_mapping_art(
    test: NativeAcceptance,
    document: str,
    level_id: str,
    identities: list[dict[str, Any]],
    originals: list[dict[str, Any]],
) -> None:
    for frame, original in zip(identities, originals, strict=True):
        _assert_art(
            test, original, _art(test, document, {"level_id": level_id, "drawing_id": frame})
        )
    _cells(test, document, identities)


def check_renumber_roundtrip(test: NativeAcceptance, image_format: str) -> None:
    """Check cycles, swaps, boundary IDs, held uses and independent package copies.

    Args:
        test: The running native acceptance case.
        image_format: One of vector, rgba8 or toonzRaster.
    """
    document, level_id = _make_scene(test, image_format)
    original_package = _save(test, document, f"renumber-original-{image_format}")
    original_files = _files(original_package)
    document, level_id = _open(test, original_package)
    original_art = [
        _art(test, document, {"level_id": level_id, "drawing_id": frame}) for frame in FRAMES
    ]
    before_render = _render_baseline(test, document)
    palette = (
        None
        if image_format == "rgba8"
        else test.success("palette.inspect", {"document_id": document, "level_id": level_id})
    )
    rotated = [FRAMES[1], FRAMES[2], FRAMES[0], FRAMES[3]]
    result = test.mutate(
        "drawing.renumber",
        document,
        level_id=level_id,
        sharing="all_exposures",
        mapping=[
            _mapping(source, destination)
            for source, destination in zip(FRAMES[:3], rotated[:3], strict=True)
        ],
    )
    test.assertEqual(result["drawings_renumbered"], 3)
    test.assertEqual(result["exposures_updated"], 12)
    _assert_mapping_art(test, document, level_id, rotated, original_art)
    test.assertEqual(_rendered_frames(test, document), before_render)
    package = _save(test, document, f"renumber-cycle-{image_format}")
    document, level_id = _open(test, package)
    _assert_mapping_art(test, document, level_id, rotated, original_art)
    test.assertEqual(_rendered_frames(test, document), before_render)
    maximum = 65535 if image_format == "vector" else 999999
    boundary = {"number": maximum, "suffix": "abcdefgh"}
    swapped = [rotated[1], rotated[0], rotated[2], boundary]
    result = test.mutate(
        "drawing.renumber",
        document,
        level_id=level_id,
        sharing="all_exposures",
        mapping=[_mapping(rotated[index], swapped[index]) for index in (0, 1, 3)],
    )
    test.assertEqual(result["drawings_renumbered"], 3)
    test.assertEqual(result["exposures_updated"], 12)
    _assert_mapping_art(test, document, level_id, swapped, original_art)
    test.assertEqual(_rendered_frames(test, document), before_render)
    test.failure(
        "drawing.inspect", {"document_id": document, "level_id": level_id, "drawing_id": FRAMES[3]}
    )
    package = _save(test, document, f"renumber-boundary-{image_format}")
    manifest = json.loads((package / "manifest.json").read_text())
    expected = sorted(swapped, key=lambda frame: (frame["number"], frame["suffix"]))
    test.assertEqual(manifest["levels"][0]["drawings"], expected)
    if image_format == "rgba8":
        test.assertTrue(any(f"{maximum}abcdefgh" in path.name for path in package.rglob("*.png")))
    document, level_id = _open(test, package)
    _assert_mapping_art(test, document, level_id, swapped, original_art)
    test.assertEqual(_rendered_frames(test, document), before_render)
    if palette is not None:
        test.assertEqual(
            test.success("palette.inspect", {"document_id": document, "level_id": level_id}),
            palette,
        )
    test.assertEqual(_files(original_package), original_files)


def _rejected(test: NativeAcceptance, document: str, params: dict[str, Any]) -> None:
    before = test.success("scene.summary", {"document_id": document})
    test.failure("drawing.renumber", test.write_params(document, params))
    test.assertEqual(test.success("scene.summary", {"document_id": document}), before)
    _cells(test, document, FRAMES)


def check_renumber_rejections(test: NativeAcceptance) -> None:
    """Reject malformed, colliding and locked mappings without partial changes.

    Args:
        test: The running native acceptance case.
    """
    document, level_id = _make_scene(test, "vector")
    params = {"level_id": level_id, "sharing": "all_exposures"}
    first = _mapping(FRAMES[0], {"number": 40, "suffix": ""})
    mappings = [
        [],
        [first, first],
        [_mapping(FRAMES[0], FRAMES[1])],
        [first, _mapping(FRAMES[1], first["destination_drawing_id"])],
        [first, _mapping({"number": 99, "suffix": ""}, FRAMES[0])],
        [first, _mapping(FRAMES[1], {"number": 65536, "suffix": ""})],
        [_mapping(FRAMES[0], {"number": 0, "suffix": ""})],
        [_mapping(FRAMES[0], {"number": 1.5, "suffix": ""})],
        [_mapping(FRAMES[0], {"number": 1000000, "suffix": ""})],
        [_mapping(FRAMES[0], {"number": 40, "suffix": "UPPER"})],
        [_mapping(FRAMES[0], {"number": 40, "suffix": "abcdefghi"})],
        [{**first, "unknown": True}],
        [first] * 4097,
    ]
    original = [
        _art(test, document, {"level_id": level_id, "drawing_id": frame}) for frame in FRAMES
    ]
    for mapping in mappings:
        _rejected(test, document, {**params, "mapping": mapping})
    _rejected(test, document, {**params, "mapping": [first], "sharing": "selected_exposure"})
    summary = test.success("scene.summary", {"document_id": document})
    locked = summary["columns"][1]["column_id"]
    test.mutate("column.update", document, column_id=locked, locked=True)
    _rejected(test, document, {**params, "mapping": [first]})
    revision = test.revisions[document]
    noop = test.mutate(
        "drawing.renumber", document, **params, mapping=[_mapping(FRAMES[0], FRAMES[0])]
    )
    test.assertEqual(noop["drawings_renumbered"], 0)
    test.assertEqual(noop["exposures_updated"], 0)
    test.assertEqual(test.revisions[document], revision)
    _assert_mapping_art(test, document, level_id, FRAMES, original)
    stale = test.write_params(document, {**params, "mapping": [first]})
    test.failure("drawing.renumber", {**stale, "expected_revision": "0"})
    test.assertEqual(test.success("scene.summary", {"document_id": document})["revision"], revision)
    other = test.create("Unrelated document")
    test.failure("drawing.renumber", test.write_params(other, {**params, "mapping": [first]}))
    test.mutate("scene.discard", document)
    test.failure("drawing.renumber", test.write_params(document, {**params, "mapping": [first]}))


def check_renumber_import_preserves_source(test: NativeAcceptance) -> None:
    """Keep an imported PNG unchanged while its private drawing identity changes.

    Args:
        test: The running native acceptance case.
    """
    original = test.fixture.read_bytes()
    document, level, column = test.populated_scene()
    before = decode_png(test.preview(document))
    source = {"number": 1, "suffix": ""}
    destination = {"number": 999999, "suffix": "z"}
    result = test.mutate(
        "drawing.renumber",
        document,
        level_id=level["level_id"],
        sharing="all_exposures",
        mapping=[_mapping(source, destination)],
    )
    test.assertGreater(result["exposures_updated"], 0)
    test.assertEqual(decode_png(test.preview(document)), before)
    cells = test.success(
        "column.cells",
        {"document_id": document, "column_id": column["column_id"], "start_frame": 0, "length": 1},
    )
    test.assertEqual(cells["cells"][0]["drawing_id"], destination)
    _save(test, document, "renumber-import")
    test.assertEqual(test.fixture.read_bytes(), original)
