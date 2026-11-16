"""Verify native MP4 delivery with independent metadata and decoded-pixel oracles."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
from fractions import Fraction
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


WIDTH = 320
HEIGHT = 240
FRAME_RATE = 12
START_FRAME = 10
COLORS = (
    (240, 32, 16, 128),
    (16, 240, 64, 128),
    (32, 48, 240, 128),
    (240, 192, 32, 128),
    (16, 224, 240, 128),
    (208, 32, 224, 128),
)


def _media_directory(case: NativeAcceptance) -> Path:
    directory = case.media_tools
    if directory is None:
        case.fail(
            "Movie qualification requires --media-tools with real ffmpeg and ffprobe binaries"
        )
    return directory


def _binary(directory: Path, name: str) -> Path:
    return directory / (name + ".exe" if os.name == "nt" else name)


def _close_worker(case: NativeAcceptance, worker: Worker) -> None:
    worker.close()
    diagnostics = worker.log_path.read_text(encoding="utf-8", errors="replace")
    case.assertEqual(worker.process.returncode, 0, diagnostics)


def _start_configured_worker(case: NativeAcceptance, directory: Path) -> None:
    case.workspace = case.directory / "movie-workspace"
    case.workspace.mkdir()
    case.worker = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / "movie-worker.stderr.log",
        media_tools=directory,
    )
    case.addCleanup(_close_worker, case, case.worker)


def _fixture(case: NativeAcceptance) -> tuple[str, str]:
    document = case.mutate(
        "scene.create",
        None,
        name="Movie temporal colors",
        width_px=WIDTH,
        height_px=HEIGHT,
        width_in=2.5,
        height_in=1.875,
        frame_rate=FRAME_RATE,
    )["document_id"]
    effect = case.mutate(
        "fx.create", document, type="colorCardFx", frame_count=START_FRAME + len(COLORS)
    )["fx_id"]
    for frame, color in enumerate(COLORS, START_FRAME):
        case.mutate(
            "fx.parameter.set_key",
            document,
            fx_id=effect,
            parameter="color",
            frame=frame,
            value=list(color),
        )
    case.mutate("fx.terminal.set", document, fx_id=effect, enabled=True)
    return document, effect


def _options() -> dict[str, Any]:
    return {
        "format": "mp4",
        "start_frame": START_FRAME,
        "end_frame": START_FRAME + len(COLORS) - 1,
        "step": 1,
        "width": WIDTH,
        "height": HEIGHT,
    }


def _movie_artifact(case: NativeAcceptance, job: dict[str, Any]) -> dict[str, Any]:
    status = case.wait_job(job["job_id"])
    case.assertEqual(status["status"], "completed", status)
    case.assertIs(status["writers_closed"], True)
    case.assertEqual(status["completed_frames"], len(COLORS))
    case.assertEqual(status["failed_frames"], 0)
    case.assertEqual(status["format"], "mp4")
    case.assertEqual(status["profile"], "opaque8-mp4")
    case.assertEqual(status["frame_rate"], FRAME_RATE)
    result = case.success("render.artifacts", {"job_id": job["job_id"]})
    case.assertNotIn("image", result)
    case.assertEqual(len(result["artifacts"]), 1, result)
    return result["artifacts"][0]


def _check_artifact(case: NativeAcceptance, artifact: dict[str, Any]) -> Path:
    expected = {
        "kind": "movie",
        "mime_type": "video/mp4",
        "width": WIDTH,
        "height": HEIGHT,
        "frame_count": len(COLORS),
        "frame_rate": FRAME_RATE,
        "source_frame_rate": FRAME_RATE,
        "pixel_format": "yuv420p",
        "container": "mp4",
        "alpha": "opaque",
        "matte": "black",
        "start_frame": START_FRAME,
        "end_frame": START_FRAME + len(COLORS) - 1,
        "step": 1,
    }
    for key, value in expected.items():
        case.assertEqual(artifact[key], value, key)
    case.assertIs(artifact["audio"], False)
    case.assertNotIn("image", artifact)
    relative = Path(artifact["workspace_relative_path"])
    case.assertFalse(relative.is_absolute())
    path = (case.workspace / relative).resolve()
    case.assertTrue(path.is_relative_to(case.workspace.resolve()), path)
    case.assertEqual(path.suffix.lower(), ".mp4")
    data = path.read_bytes()
    case.assertGreater(len(data), 0)
    case.assertEqual(artifact["size_bytes"], len(data))
    case.assertEqual(artifact["sha256"], hashlib.sha256(data).hexdigest())
    error = case.failure("artifact.read", {"artifact_id": artifact["artifact_id"]})
    case.assertEqual(error["code"], "unsupported_artifact_kind")
    return path


def _run_tool(case: NativeAcceptance, name: str, arguments: list[str]) -> bytes:
    result = subprocess.run(
        [str(_binary(_media_directory(case), name)), *arguments],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=60,
        check=False,
    )
    (case.directory / f"{name}.stderr.log").write_bytes(result.stderr)
    case.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", errors="replace"))
    return result.stdout


def _probe(case: NativeAcceptance, path: Path, artifact: dict[str, Any]) -> None:
    output = _run_tool(
        case,
        "ffprobe",
        ["-v", "error", "-count_frames", "-show_streams", "-show_format", "-of", "json", str(path)],
    )
    (case.directory / "independent-ffprobe.json").write_bytes(output)
    probe = json.loads(output)
    case.assertEqual(len(probe["streams"]), 1, probe)
    stream = probe["streams"][0]
    case.assertEqual(stream["codec_type"], "video")
    case.assertTrue(stream["codec_name"])
    case.assertEqual(artifact["codec"], stream["codec_name"])
    case.assertEqual((stream["width"], stream["height"]), (WIDTH, HEIGHT))
    case.assertEqual(stream["pix_fmt"], "yuv420p")
    case.assertEqual(int(stream["nb_read_frames"]), len(COLORS))
    case.assertEqual(Fraction(stream["avg_frame_rate"]), Fraction(FRAME_RATE))
    case.assertIn("mp4", probe["format"]["format_name"].split(","))
    case.assertEqual(artifact["major_brand"], probe["format"]["tags"]["major_brand"])
    duration = float(probe["format"]["duration"])
    case.assertAlmostEqual(duration, len(COLORS) / FRAME_RATE, delta=0.001)
    case.assertEqual(artifact["duration_seconds"], duration)


def _decoded_colors(case: NativeAcceptance, path: Path) -> None:
    decoded = _run_tool(
        case,
        "ffmpeg",
        [
            "-v",
            "error",
            "-i",
            str(path),
            "-map",
            "0:v:0",
            "-fps_mode",
            "passthrough",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "rgb24",
            "pipe:1",
        ],
    )
    frame_bytes = WIDTH * HEIGHT * 3
    case.assertEqual(len(decoded), len(COLORS) * frame_bytes)
    measurements = []
    for index, color in enumerate(COLORS):
        frame = decoded[index * frame_bytes : (index + 1) * frame_bytes]
        expected = tuple(round(channel * color[3] / 255) for channel in color[:3])
        maximum_error = max(abs(value - expected[offset % 3]) for offset, value in enumerate(frame))
        # Match the native MP4 regression tolerance for this writer's fixed bitrate profile.
        case.assertLessEqual(maximum_error, 12, f"Frame {START_FRAME + index}: {expected}")
        measurements.append({"frame": START_FRAME + index, "maximum_rgb_error": maximum_error})
    (case.directory / "decoded-frame-proof.json").write_text(
        json.dumps(measurements, indent=2) + "\n", encoding="utf-8"
    )


def movie_snapshot_metadata_and_pixels(case: NativeAcceptance) -> None:
    """Decode distinct translucent frames after source edits and discard.

    Args:
        case: A fresh acceptance fixture with explicit media-tool configuration.
    """
    _start_configured_worker(case, _media_directory(case))
    document, effect = _fixture(case)
    revision = case.revisions[document]
    job = case.mutate("render.submit", document, **_options())
    case.mutate(
        "fx.parameter.set_key",
        document,
        fx_id=effect,
        parameter="color",
        frame=START_FRAME,
        value=[255, 255, 255, 255],
    )
    case.mutate("scene.settings", document, frame_rate=24)
    case.mutate("scene.discard", document)
    artifact = _movie_artifact(case, job)
    (case.directory / "movie-artifact.json").write_text(
        json.dumps(artifact, indent=2) + "\n", encoding="utf-8"
    )
    case.assertEqual(artifact["document_id"], document)
    case.assertEqual(artifact["source_revision"], revision)
    case.assertEqual(artifact["job_id"], job["job_id"])
    path = _check_artifact(case, artifact)
    _probe(case, path, artifact)
    _decoded_colors(case, path)


def _reject_submit(
    case: NativeAcceptance, document: str, options: dict[str, Any], code: str
) -> None:
    before = case.success("scene.summary", {"document_id": document})
    paths = set(case.workspace.rglob("*"))
    error = case.failure("render.submit", case.write_params(document, options))
    case.assertEqual(error["code"], code)
    case.assertEqual(case.success("scene.summary", {"document_id": document}), before)
    case.assertEqual(set(case.workspace.rglob("*")), paths)


def movie_without_media_tools_is_rejected(case: NativeAcceptance) -> None:
    """Keep MP4 disabled without explicit configuration while PNG remains usable.

    Args:
        case: A fresh worker launched without media tools.
    """
    document = case.create("No media tools")
    _reject_submit(case, document, _options(), "render_unavailable")
    case.assertTrue(case.preview(document))


def movie_options_reject_unsupported_values(case: NativeAcceptance) -> None:
    """Reject unsupported movie dimensions and sampling before touching native state.

    Args:
        case: A fresh acceptance fixture with explicit media-tool configuration.
    """
    _start_configured_worker(case, _media_directory(case))
    document = case.create("Unsupported movie settings")
    for override in ({"width": 319}, {"height": 239}, {"step": 2}):
        _reject_submit(case, document, {**_options(), **override}, "unsupported_render_settings")
    for override in ({"format": "webm"}, {"quality": 50}, {"scale": 50}, {"audio": True}):
        _reject_submit(case, document, {**_options(), **override}, "invalid_params")
    before = case.success("scene.summary", {"document_id": document})
    params = case.write_params(document, {"frame": 0, "format": "mp4"})
    case.assertEqual(case.failure("render.preview", params)["code"], "invalid_params")
    case.assertEqual(case.success("scene.summary", {"document_id": document}), before)


def _unpublished(case: NativeAcceptance, job: dict[str, Any], expected: str) -> None:
    status = case.wait_job(job["job_id"])
    case.assertEqual(status["status"], expected, status)
    case.assertIs(status["writers_closed"], True)
    error = case.failure("render.artifacts", {"job_id": job["job_id"]})
    case.assertEqual(error["code"], "artifacts_unavailable")


def movie_cancellation_publishes_no_artifact(case: NativeAcceptance) -> None:
    """Cancel a real movie job and require closed writers before terminal status.

    Args:
        case: A fresh acceptance fixture with explicit media-tool configuration.
    """
    _start_configured_worker(case, _media_directory(case))
    document, _effect = _fixture(case)
    before = case.success("scene.summary", {"document_id": document})
    job = case.mutate("render.submit", document, **{**_options(), "end_frame": 129})
    case.mutate("render.cancel", None, job_id=job["job_id"])
    _unpublished(case, job, "canceled")
    case.assertEqual(case.success("scene.summary", {"document_id": document}), before)
    case.assertTrue(case.preview(document))


def _private_media(case: NativeAcceptance) -> Path:
    source = _media_directory(case)
    directory = case.directory / "private-media-tools"
    directory.mkdir()
    for name in ("ffmpeg", "ffprobe"):
        shutil.copy2(_binary(source, name), _binary(directory, name))
    for library in source.glob("*.dll"):
        shutil.copy2(library, directory / library.name)
    return directory.resolve()


def movie_writer_failure_publishes_no_artifact(case: NativeAcceptance) -> None:
    """Fail a real writer after successful startup without publishing partial output.

    Args:
        case: A fresh acceptance fixture with explicit media-tool configuration.
    """
    directory = _private_media(case)
    _start_configured_worker(case, directory)
    document, _effect = _fixture(case)
    before = case.success("scene.summary", {"document_id": document})
    # The real probe rejects encoder-only arguments; startup already admitted both real tools.
    shutil.copy2(_binary(directory, "ffprobe"), _binary(directory, "ffmpeg"))
    job = case.mutate("render.submit", document, **_options())
    _unpublished(case, job, "failed")
    case.assertEqual(case.success("scene.summary", {"document_id": document}), before)
    case.assertTrue(case.preview(document))
