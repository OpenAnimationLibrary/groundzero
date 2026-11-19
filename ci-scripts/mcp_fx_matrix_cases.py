"""Render every admitted FX type to every output format and compare with the matrix record.

    mcp_fx_matrix_record.py record --executable APP --media-tools DIR --reason "why"
                                   [--only TYPE ...]

The acceptance case below renders each admitted type through the worker as a PNG frame and as a
one-frame MP4, decodes both independently (the PNG with the suite's decoder, the movie with the
configured ffmpeg), and compares each with what `mcp_fx_matrix.json` stores for this platform. It
never writes the record. `record` is the one command that does, with a reason, and only for the
platform it runs on; a run of some types is stored as a subset and leaves the platform incomplete.

A cell passes when the job completes with closed writers, the output decodes at the requested
size, the PNG equals the preview of the same frame (a second path through the renderer), and the
SHA-256 of the decoded pixels equals the stored reference (PNG) or the movie frame stays within
the stated error of the PNG render (MP4).
"""

from __future__ import annotations

import base64
import hashlib
import subprocess
import sys
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_fx_authoring_cases import (
    NATIVE_PRESETS,
    _artwork_fixture,
    _output,
    _pixels,
    _wired_effect,
)
from mcp_fx_matrix import admitted_types, load_data
from mcp_movie_cases import _binary, _frame_sync_args
from mcp_png import Pixel, decode_png
from mcp_worker import Worker

if TYPE_CHECKING:
    from collections.abc import Sequence

    from mcp_acceptance import NativeAcceptance

FRAME = 6
SIZE = 32
TYPES_PER_WORKER = 6
HOST = {"darwin": "macOS", "linux": "Linux", "win32": "Windows"}
PNG_TOLERANCE = {"max_color": 0, "max_alpha": 0, "differing_pixels": 0, "why": ""}
MP4_TOLERANCE = {
    "mean_error": 40.0,
    "max_error": 220,
    "decoded_differing_bytes": 0,
    "why": (
        "against the PNG only a gross error (a wrong or flipped frame) is caught: the writer's "
        "fixed low bitrate decodes a 32 pixel frame with a mean error up to about 26 (black "
        "decodes as 20); against the stored decode there is no tolerance, the same ffmpeg and "
        "encoder must give the same bytes"
    ),
}


def host_platform() -> str:
    """The census name of the platform this process runs on."""
    return HOST[sys.platform]


def pixel_digest(pixels: Sequence[Pixel]) -> str:
    """The SHA-256 of decoded RGBA values, independent of how a PNG file was compressed."""
    return hashlib.sha256(bytes(channel for pixel in pixels for channel in pixel)).hexdigest()


def png_difference(left: Sequence[Pixel], right: Sequence[Pixel]) -> dict[str, int]:
    """The difference between two decoded images in the units the PNG tolerance states."""
    colors = alphas = pixels = 0
    for one, other in zip(left, right, strict=True):
        color = max(abs(one[index] - other[index]) for index in range(3))
        alpha = abs(one[3] - other[3])
        colors, alphas = max(colors, color), max(alphas, alpha)
        pixels += bool(color or alpha)
    return {"max_color": colors, "max_alpha": alphas, "differing_pixels": pixels}


def black_matte(pixels: Sequence[Pixel]) -> bytes:
    """What an opaque movie holds for straight-alpha pixels: the colour over black as RGB."""
    return bytes(round(value * pixel[3] / 255) for pixel in pixels for value in pixel[:3])


def movie_difference(expected: bytes, decoded: bytes) -> dict[str, float]:
    """Mean and largest absolute error between two RGB byte strings of one size."""
    errors = [abs(one - other) for one, other in zip(expected, decoded, strict=True)]
    return {"mean_error": round(sum(errors) / len(errors), 3), "max_error": max(errors)}


def _read_png(case: NativeAcceptance, job: str) -> tuple[list[Pixel], dict[str, Any]]:
    artifacts = case.success("job.result", {"job_id": job})["artifacts"]
    case.assertEqual(len(artifacts), 1, artifacts)
    image = case.success("artifact.read", {"artifact_id": artifacts[0]["artifact_id"]})
    data = base64.b64decode(image["image"]["data"], validate=True)
    case.assertEqual(hashlib.sha256(data).hexdigest(), artifacts[0]["sha256"])
    width, height, pixels = decode_png(data)
    case.assertEqual((width, height), (SIZE, SIZE))
    return pixels, artifacts[0]


def _submit(case: NativeAcceptance, document: str, form: str) -> str:
    options = {"start_frame": FRAME, "end_frame": FRAME, "step": 1, "width": SIZE, "height": SIZE}
    job = case.mutate("render.submit", document, format=form, **options)
    status = case.wait_job(job["job_id"])
    case.assertEqual(status["status"], "completed", status)
    case.assertIs(status["writers_closed"], True)
    case.assertEqual((status["completed_frames"], status["failed_frames"]), (1, 0))
    return job["job_id"]


def _decode_movie(case: NativeAcceptance, media: Path, artifact: dict[str, Any]) -> bytes:
    path = (case.workspace / artifact["workspace_relative_path"]).resolve()
    command = [
        str(_binary(media, "ffmpeg")),
        "-v",
        "error",
        "-i",
        str(path),
        "-map",
        "0:v:0",
        *_frame_sync_args(case),
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "pipe:1",
    ]
    done = subprocess.run(
        command, stdin=subprocess.DEVNULL, capture_output=True, timeout=60, check=False
    )
    case.assertEqual(done.returncode, 0, done.stderr.decode("utf-8", errors="replace"))
    return done.stdout


def measure_type(case: NativeAcceptance, kind: str, media: Path | None) -> dict[str, Any]:
    """Render one type through the worker and return what each format measured.

    Returns:
        `{"png_sequence": {...}, "mp4": {...}}`: for a format, either the measurements or a
        `skipped` reason; the PNG entry always carries the decoded-pixel digest.
    """
    document, artwork = _artwork_fixture(case)
    effect = _wired_effect(case, document, kind, artwork)
    _output(case, document, effect)
    for name, value in NATIVE_PRESETS[kind].items():
        case.mutate("fx.parameter.set", document, fx_id=effect, parameter=name, value=value)
    preview = _pixels(case, document, FRAME)
    png, _artifact = _read_png(case, _submit(case, document, "png"))
    result: dict[str, Any] = {
        "png_sequence": {
            "digest": pixel_digest(png),
            "preview_difference": png_difference(preview, png),
        }
    }
    result["mp4"] = _measure_movie(case, document, png, media)
    return result


def _measure_movie(
    case: NativeAcceptance, document: str, png: list[Pixel], media: Path | None
) -> dict[str, Any]:
    if media is None:
        return {"skipped": "no media tools were configured for the run (--media-tools)"}
    job = _submit(case, document, "mp4")
    artifact = case.success("job.result", {"job_id": job})["artifacts"][0]
    case.assertEqual((artifact["width"], artifact["height"], artifact["frame_count"]), (32, 32, 1))
    decoded = _decode_movie(case, media, artifact)
    case.assertEqual(len(decoded), SIZE * SIZE * 3)
    return {
        "codec": artifact["codec"],
        "decoded_digest": hashlib.sha256(decoded).hexdigest(),
        **movie_difference(black_matte(png), decoded),
    }


def _identity(case: NativeAcceptance, worker: Worker) -> dict[str, str]:
    """The identity fields the worker reports for the run, as flat text."""
    report = worker.call("runtime.inspect", {"probes": ["media", "graphics"]})["result"]
    platform, build = report["platform"], report["worker"]["build"]
    media, graphics = report["media"], report["graphics"]
    capabilities = worker.call("render.capabilities", {})["result"]
    ffmpeg = media.get("ffmpeg") or {}
    return {
        "build": f"{build['application']} {build['version']} digest {build['build_digest']}",
        "operating_system": f"{platform['os']} {platform['os_version']}",
        "cpu": f"{platform['cpu_architecture']} (built for {platform['build_cpu_architecture']})",
        "qt": f"built {platform['qt_build_version']}, running {platform['qt_runtime_version']}",
        "graphics_backend": f"{graphics['qpa_platform']}, probe {graphics['probe'] or 'not run'}",
        "plugins": f"policy {report['plugins']['policy']}",
        "fonts": f"usable {report['fonts']['state']['usable']}",
        "media_tools": f"ffmpeg {ffmpeg.get('version', 'not configured')}, "
        f"sha256 {ffmpeg.get('sha256', 'none')}",
        "codec": f"mp4 encoder {capabilities['mp4']['encoder']}, "
        f"available {capabilities['mp4']['encoder_available']}",
        "render_profiles": ", ".join(capabilities["profiles"]),
    }


def _worker(case: NativeAcceptance, media: Path | None, name: str) -> Worker:
    workspace = case.directory / name
    workspace.mkdir()
    worker = Worker(
        case.executable,
        workspace,
        case.inputs,
        case.directory / f"{name}.stderr.log",
        media_tools=media,
    )
    case.addCleanup(worker.close)
    case.workspace, case.worker = workspace, worker
    return worker


def measure(
    case: NativeAcceptance, types: Sequence[str], media: Path | None
) -> tuple[dict[str, dict[str, Any]], dict[str, str]]:
    """Measure every listed type, a fresh worker for each few so the job limit is never met."""
    results: dict[str, dict[str, Any]] = {}
    identity: dict[str, str] = {}
    for first in range(0, len(types), TYPES_PER_WORKER):
        worker = _worker(case, media, f"matrix-{first}")
        identity = identity or _identity(case, worker)
        for kind in types[first : first + TYPES_PER_WORKER]:
            results[kind] = measure_type(case, kind, media)
        worker.close()
    return results, identity


def cell_from(measured: dict[str, Any], form: str, references: dict[str, str]) -> dict[str, Any]:
    """The cell a measurement makes: passed when inside its tolerance, failed otherwise.

    Args:
        measured: What `measure_type` returned for the format.
        form: `png_sequence` or `mp4`.
        references: The digests to compare with: `png` always, `decoded` for a movie.
    """
    if "skipped" in measured:
        return {"status": "unavailable", "reason": measured["skipped"]}
    cell: dict[str, Any] = {"reference": references["png"]}
    if form == "png_sequence":
        observed = png_difference_to(measured, references["png"])
        tolerance = PNG_TOLERANCE
    else:
        observed = {key: measured[key] for key in ("mean_error", "max_error")}
        observed["decoded_differing_bytes"] = int(
            measured["decoded_digest"] != references["decoded"]
        )
        tolerance = MP4_TOLERANCE
        cell["decoded_reference"] = references["decoded"]
    limits = {n: float(v) for n, v in tolerance.items() if not isinstance(v, str)}
    over = [name for name in observed if observed[name] > limits[name]]
    cell.update(status="failed" if over else "passed", tolerance=dict(tolerance), observed=observed)
    if over:
        cell["reason"] = "outside the tolerance on " + ", ".join(over)
    return cell


def png_difference_to(measured: dict[str, Any], reference: str) -> dict[str, int]:
    """For a PNG cell: how far the decoded frame is from the preview and the stored reference."""
    observed = dict(measured["preview_difference"])
    if measured["digest"] != reference:
        observed["differing_pixels"] = max(observed["differing_pixels"], 1)
    return observed


def apply_run(
    data: dict[str, Any], results: dict[str, dict[str, Any]], identity: dict[str, str], reason: str
) -> str:
    """Store a run: new cells for the types it measured, a new latest run for the platform."""
    platform = host_platform()
    name = f"{platform}-{time.strftime('%Y%m%d-%H%M%S')}"
    data["runs"][name] = {
        "platform": platform,
        "recorded": time.strftime("%Y-%m-%d %H:%M:%S"),
        "reason": reason.strip(),
        "covered": f"{len(results)} of {len(admitted_types())} types",
        "identity": identity,
    }
    data["platforms"][platform]["latest_run"] = name
    for kind, measured in results.items():
        references = {
            "png": measured["png_sequence"]["digest"],
            "decoded": measured["mp4"].get("decoded_digest", ""),
        }
        for form, entry in measured.items():
            cell = cell_from(entry, form, references)
            if cell["status"] != "unavailable":
                cell["run"] = name
            data["cells"][kind][form][platform] = cell
    return name


def compare(case: NativeAcceptance, results: dict[str, dict[str, Any]]) -> list[str]:
    """Differences between a measurement and the record of this platform, as sentences."""
    data = load_data()
    platform = host_platform()
    found = []
    for kind, measured in results.items():
        for form, entry in measured.items():
            cell = data["cells"][kind][form][platform]
            if "skipped" in entry or cell["status"] != "passed":
                continue
            references = {"png": cell["reference"], "decoded": cell.get("decoded_reference", "")}
            now = cell_from(entry, form, references)
            if now["status"] != "passed":
                found.append(f"{kind} / {form}: {now['reason']}; observed {now['observed']}")
    return found


@acceptance_case
def fx_matrix_cells_match_their_record(case: NativeAcceptance) -> None:
    """Render every admitted type as PNG and MP4 and compare each with the stored matrix cell."""
    catalog = case.success("fx.catalog", {})
    types = admitted_types()
    case.assertEqual([effect["type"] for effect in catalog["effects"]], types)
    case.assertEqual(sorted(NATIVE_PRESETS), sorted(types))
    platform = host_platform()
    data = load_data()
    if data["platforms"][platform].get("latest_run") is None:
        case.skipTest(f"no run is recorded for {platform}: its cells stay not tested")
    results, _identity_text = measure(case, types, case.media_tools)
    for kind, measured in results.items():
        case.assertLessEqual(
            measured["png_sequence"]["preview_difference"]["max_color"], 0, f"{kind}: preview"
        )
    case.assertEqual(compare(case, results), [], "cells differ from the matrix record")
