"""Qualify native text creation, replay, refusal purity, and editable PLI output."""

from __future__ import annotations

import base64
import hashlib
from typing import TYPE_CHECKING, Any

from mcp_png import Pixel, decode_png

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

DRAWING = {"number": 1, "suffix": ""}


def _font(case: NativeAcceptance) -> dict[str, Any]:
    for family in ("Arial", "DejaVu Sans", "Liberation Sans"):
        page = case.success("text.fonts", {"family_prefix": family, "offset": 0, "limit": 64})
        for item in page["families"]:
            if item["family"] != family:
                continue
            for style in ("Regular", "Book"):
                if style in item["styles"]:
                    return {"family": family, "style": style, "size_points": 32}
    case.fail("Native text fixture needs installed Arial, DejaVu Sans or Liberation Sans")
    raise AssertionError("unreachable")


def _fixture(case: NativeAcceptance) -> tuple[str, dict[str, Any]]:
    document = case.mutate(
        "scene.create",
        None,
        name="Editable native text",
        width_px=64,
        height_px=64,
        width_in=1.2,
        height_in=1.2,
        frame_rate=24,
    )["document_id"]
    level = case.mutate("level.create", document, name="Text outlines", format="vector")
    return document, {
        "level_id": level["level_id"],
        "drawing_id": DRAWING,
        "text": "OB",
        "font": _font(case),
        "baseline": {"x": 0, "y": 0},
        "style_id": 1,
    }


def _geometry(case: NativeAcceptance, document: str, level: str) -> dict[str, Any]:
    return case.success(
        "vector.inspect",
        {"document_id": document, "level_id": level, "drawing_id": DRAWING, "limit": 128},
    )


def _pixels(case: NativeAcceptance, document: str) -> list[Pixel]:
    job = case.mutate("render.preview", document, frame=0, width=256, height=256)
    state = case.wait_job(job["job_id"])
    case.assertEqual(state["status"], "completed", state)
    case.assertIs(state["writers_closed"], True)
    artifact = case.success("job.result", {"job_id": job["job_id"]})["artifacts"][0]
    result = case.success("artifact.read", {"artifact_id": artifact["artifact_id"]})
    data = base64.b64decode(result["image"]["data"], validate=True)
    case.assertEqual(hashlib.sha256(data).hexdigest(), artifact["sha256"])
    width, height, pixels = decode_png(data)
    case.assertEqual((width, height), (256, 256))
    return pixels


def _reopen(case: NativeAcceptance, document: str) -> tuple[str, str]:
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "text-package"))
    case.mutate("scene.discard", document)
    opened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    summary = case.success("scene.summary", {"document_id": opened})
    case.assertEqual(len(summary["levels"]), 1)
    case.assertEqual(summary["levels"][0]["drawings"], [DRAWING])
    return opened, summary["levels"][0]["level_id"]


def _close(case: NativeAcceptance) -> None:
    case.worker.close()
    case.assertEqual(case.worker.process.returncode, 0, case.worker.log_path.read_text())


def text_outline_round_trip_and_edit(case: NativeAcceptance) -> None:
    """Create and replay actual text, save/reopen PLI, then edit its native geometry.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, target = _fixture(case)
    params = case.write_params(document, target)
    result = case.success("vector.text.create", params)
    case.revisions[document] = result["revision"]
    case.assertEqual(result["geometry_revision"], result["revision"])
    case.assertEqual(result["source_recipe"], {"text": target["text"], "font": target["font"]})
    case.assertIs(result["semantic_text_persisted"], False)
    case.assertEqual(result["editable_as"], "native_vector_strokes")
    case.assertEqual(case.success("vector.text.create", params), result)
    before = _geometry(case, document, target["level_id"])
    case.assertGreater(before["total"], 1)
    case.assertEqual(result["stroke_indices"], list(range(before["total"])))
    case.assertEqual(result["group_index"], 0)
    summary = case.success("scene.summary", {"document_id": document})
    case.assertEqual(summary["columns"], [])
    column = case.mutate("column.create", document, name="Text exposure")
    case.mutate(
        "column.expose",
        document,
        level_id=target["level_id"],
        drawing_id=DRAWING,
        column_id=column["column_id"],
        start_frame=0,
        length=1,
    )
    pixels = _pixels(case, document)
    case.assertTrue(any(pixel[3] for pixel in pixels), "Text rendered no visible pixels")
    opened, level = _reopen(case, document)
    after = _geometry(case, opened, level)
    case.assertEqual(after["total"], before["total"])
    for prior, current in zip(before["strokes"], after["strokes"], strict=True):
        for key in ("index", "style_id", "closed"):
            case.assertEqual(prior[key], current[key])
        case.assertEqual(len(prior["points"]), len(current["points"]))
    case.assertEqual(_pixels(case, opened), pixels)
    case.mutate(
        "vector.edit",
        opened,
        level_id=level,
        drawing_id=DRAWING,
        coordinate_space="drawing_native",
        sharing="all_exposures",
        edits=[{"action": "delete", "index": 0}],
    )
    case.assertEqual(_geometry(case, opened, level)["total"], before["total"] - 1)
    case.assertNotEqual(_pixels(case, opened), pixels)
    _close(case)


def _reject(case: NativeAcceptance, document: str, target: dict[str, Any], expected: str) -> None:
    before = case.success("scene.summary", {"document_id": document})
    palette = case.success(
        "palette.inspect", {"document_id": document, "level_id": target["level_id"]}
    )
    error = case.failure("vector.text.create", case.write_params(document, target))
    case.assertEqual(error["code"], expected, error)
    case.assertEqual(case.success("scene.summary", {"document_id": document}), before)
    case.assertEqual(
        case.success("palette.inspect", {"document_id": document, "level_id": target["level_id"]}),
        palette,
    )


def text_rejections_preserve_drawing(case: NativeAcceptance) -> None:
    """Reject invalid content and stale/reused writes without publishing partial art.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, target = _fixture(case)
    for text, code in ((" \t\n", "unsupported_outline"), ("\n" * 32, "resource_limit")):
        _reject(case, document, {**target, "text": text}, code)
    _reject(case, document, {**target, "text": "\r"}, "invalid_params")
    _reject(case, document, {**target, "text": "\U0010ffff"}, "missing_glyph")
    font = {**target["font"], "family": "MCP must refuse absent font"}
    _reject(case, document, {**target, "font": font}, "font_not_found")
    _reject(case, document, {**target, "style_id": 4095}, "invalid_params")
    _reject(case, document, {**target, "baseline": {"x": 32768, "y": 0}}, "resource_limit")
    params = case.write_params(document, target)
    result = case.success("vector.text.create", params)
    case.revisions[document] = result["revision"]
    before = _geometry(case, document, target["level_id"])
    _reject(case, document, target, "already_exists")
    stale = {**params, "operation_id": "fresh-stale-text"}
    case.assertEqual(case.failure("vector.text.create", stale)["code"], "revision_conflict")
    case.assertEqual(
        case.failure("vector.text.create", {**params, "text": "AB"})["code"], "operation_conflict"
    )
    case.assertEqual(_geometry(case, document, target["level_id"]), before)
    case.assertEqual(case.success("vector.text.create", params), result)
    _close(case)
