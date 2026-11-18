"""Native render acceptance cases for snapshot isolation and artifact ownership."""

from __future__ import annotations

import base64
import hashlib
from typing import TYPE_CHECKING, Any

from mcp_png import decode_png, fixture_pixel

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


def _artifacts(case: NativeAcceptance, job: dict[str, Any]) -> list[dict[str, Any]]:
    status = case.wait_job(job["job_id"])
    case.assertEqual(status["status"], "completed", status)
    case.assertIs(status["writers_closed"], True)
    case.assertEqual(status["completed_frames"], status["frame_count"])
    case.assertEqual(status["failed_frames"], 0)
    return case.success("job.result", {"job_id": job["job_id"]})["artifacts"]


def _read(case: NativeAcceptance, artifact: dict[str, Any]) -> tuple[dict[str, Any], bytes]:
    result = case.success("artifact.read", {"artifact_id": artifact["artifact_id"]})
    data = base64.b64decode(result["image"]["data"], validate=True)
    case.assertEqual(result["image"]["mimeType"], "image/png")
    case.assertEqual(hashlib.sha256(data).hexdigest(), artifact["sha256"])
    return result, data


def blank_dimensions_and_alpha(case: NativeAcceptance) -> None:
    """Render transparent blanks at odd dimensions without changing their source.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Blank")
    before = case.success("scene.summary", {"document_id": document})
    job = case.mutate("render.preview", document, frame=7, width=31, height=17)
    artifacts = _artifacts(case, job)
    case.assertEqual(len(artifacts), 1)
    result, data = _read(case, artifacts[0])
    width, height, pixels = decode_png(data)
    case.assertEqual((width, height), (31, 17))
    case.assertEqual(result["frame"], 7)
    case.assertEqual(result["source_revision"], before["revision"])
    case.assertTrue(all(pixel[3] == 0 for pixel in pixels))
    case.assertEqual(case.success("scene.summary", {"document_id": document}), before)


def preview_fit_preserves_camera(case: NativeAcceptance) -> None:
    """Fit a default preview to its bound while retaining the document camera.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Large blank")
    case.mutate("scene.settings", document, width_px=1920, height_px=1080)
    before = case.success("camera.inspect", {"document_id": document})
    revision = case.revisions[document]
    job = case.mutate("render.preview", document, frame=0)
    artifact = _artifacts(case, job)[0]
    result, data = _read(case, artifact)
    width, height, pixels = decode_png(data)
    case.assertEqual((width, height), (1024, 576))
    case.assertTrue(all(pixel[3] == 0 for pixel in pixels))
    case.assertEqual(result["source_revision"], revision)
    case.assertEqual(case.revisions[document], revision)
    case.assertEqual(case.success("camera.inspect", {"document_id": document}), before)


def generator_straight_alpha(case: NativeAcceptance) -> None:
    """Verify native color-card RGB and alpha survive the PNG writer boundary.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Color card")
    effect = case.mutate("fx.create", document, type="colorCardFx", frame_count=2)
    case.mutate(
        "fx.parameter.set",
        document,
        fx_id=effect["fx_id"],
        parameter="color",
        value=[255, 0, 0, 128],
    )
    case.mutate("fx.terminal.set", document, fx_id=effect["fx_id"], enabled=True)
    graph = case.success("fx.graph", {"document_id": document})
    job = case.mutate("render.preview", document, frame=1, width=17, height=9)
    artifact = _artifacts(case, job)[0]
    result, data = _read(case, artifact)
    width, height, pixels = decode_png(data)
    case.assertEqual((width, height), (17, 9))
    case.assertEqual(result["alpha"], "straight")
    case.assertEqual(set(pixels), {(255, 0, 0, 128)})
    case.assertEqual(case.success("fx.graph", {"document_id": document}), graph)


def snapshot_survives_edits_and_discard(case: NativeAcceptance) -> None:
    """Keep rendered frames bound to their original document and drawing revision.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document, level, _column = case.populated_scene()
    revision = case.revisions[document]
    input_hash = hashlib.sha256(case.fixture.read_bytes()).hexdigest()
    job = case.mutate(
        "render.submit", document, start_frame=0, end_frame=4, step=2, width=32, height=32
    )
    case.mutate(
        "raster.patch",
        document,
        level_id=level["level_id"],
        drawing_id={"number": 1, "suffix": ""},
        coordinate_space="raster_pixels_lower_left",
        x=0,
        y=0,
        width=32,
        height=32,
        sharing="all_exposures",
        pixels=[{"r": 255, "g": 255, "b": 255, "a": 255}] * 1024,
    )
    case.assertNotEqual(case.revisions[document], revision)
    stale = case.write_params(document, {"frame": 0})
    stale["expected_revision"] = revision
    case.assertEqual(case.failure("render.preview", stale)["code"], "revision_conflict")
    other = case.create("Acceptance")
    before = case.success("scene.summary", {"document_id": other})
    case.mutate("scene.discard", document)
    artifacts = _artifacts(case, job)
    case.assertEqual([artifact["frame"] for artifact in artifacts], [0, 2, 4])
    for artifact in artifacts:
        result, data = _read(case, artifact)
        case.assertEqual(result["document_id"], document)
        case.assertEqual(result["source_revision"], revision)
        width, height, pixels = decode_png(data)
        case.assertEqual((width, height), (32, 32))
        for x, y in ((8, 8), (24, 8), (8, 24), (24, 24)):
            case.assertEqual(pixels[y * width + x], fixture_pixel(x, y, width))
    case.assertEqual(case.success("scene.summary", {"document_id": other}), before)
    case.assertEqual(hashlib.sha256(case.fixture.read_bytes()).hexdigest(), input_hash)
    case.failure("scene.summary", {"document_id": document})
    later = case.mutate("render.preview", other, frame=0, width=8, height=8)
    _result, blank = _read(case, _artifacts(case, later)[0])
    case.assertTrue(all(pixel[3] == 0 for pixel in decode_png(blank)[2]))
    _result, retained = _read(case, artifacts[0])
    case.assertEqual(decode_png(retained)[2][8 * 32 + 8], fixture_pixel(8, 8, 32))


def changed_artifact_is_rejected(case: NativeAcceptance) -> None:
    """Reject changed output bytes even when the opaque artifact ID is valid.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Artifact ownership")
    job = case.mutate("render.preview", document, frame=0, width=8, height=8)
    artifact = _artifacts(case, job)[0]
    _result, original = _read(case, artifact)
    path = case.workspace / "renders" / job["job_id"] / "frames" / "frame.0001.png"
    case.assertTrue(path.is_file())
    path.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
    error = case.failure("artifact.read", {"artifact_id": artifact["artifact_id"]})
    case.assertEqual(error["code"], "invalid_artifact")


def eof_cancels_without_forced_kill(case: NativeAcceptance) -> None:
    """Close input during a render and require cooperative worker termination.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document, _level, _column = case.populated_scene()
    job = case.mutate(
        "render.submit", document, start_frame=0, end_frame=119, width=256, height=256
    )
    case.assertEqual(job["status"], "preparing")
    stream = case.worker.process.stdin
    case.assertIsNotNone(stream)
    if stream is None:
        case.fail("The native worker did not retain its input pipe")
    stream.close()
    case.worker.process.wait(timeout=30)
    diagnostics = case.worker.log_path.read_text(encoding="utf-8", errors="replace")
    case.assertEqual(case.worker.process.returncode, 0, diagnostics)
