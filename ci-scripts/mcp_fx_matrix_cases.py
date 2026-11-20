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
import math
import operator
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
SIZE = 256
BLOCK = 32
MARGIN = 1.25
TYPES_PER_WORKER = 6
HOST = {"darwin": "macOS", "linux": "Linux", "win32": "Windows"}
PNG_TOLERANCE = {"max_color": 0, "max_alpha": 0, "differing_pixels": 0, "why": ""}
MP4_WHY = (
    "set per cell from the recorded run: the observed error times 1.25, rounded up, plus one code "
    "value; the largest of 16 pixel blocks bounds a misplaced or missing effect, the mean error "
    "the whole frame; the decoded bytes must equal the stored decode (determinism only)"
)


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


def block_errors(
    expected: bytes, decoded: bytes, size: int = SIZE, block: int = BLOCK
) -> list[float]:
    """Mean absolute error of each block of two RGB frames, in reading order."""
    stride = size * 3
    block = min(block, size)
    found = []
    for top in range(0, size, block):
        for left in range(0, size, block):
            total = 0
            for row in range(top, top + block):
                start = row * stride + left * 3
                span = slice(start, start + block * 3)
                total += sum(map(abs, map(operator.sub, expected[span], decoded[span])))
            found.append(total / (block * block * 3))
    return found


def movie_difference(expected: bytes, decoded: bytes, size: int = SIZE) -> dict[str, float]:
    """How far a decoded frame is from the expected one: whole frame, worst block, worst value."""
    errors = list(map(abs, map(operator.sub, expected, decoded)))
    return {
        "mean_error": round(sum(errors) / len(errors), 3),
        "block_error": round(max(block_errors(expected, decoded, size)), 3),
        "max_error": max(errors),
    }


def preview_pixels(case: NativeAcceptance, document: str) -> list[Pixel]:
    """The preview of the frame at the matrix size, decoded."""
    job = case.mutate("render.preview", document, frame=FRAME, width=SIZE, height=SIZE)
    case.assertEqual(case.wait_job(job["job_id"])["status"], "completed")
    pixels, _artifact = _read_png(case, job["job_id"])
    return pixels


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
    profile = {"png": "rgba8-png", "mp4": "opaque8-mp4"}[form]
    job = case.mutate("render.submit", document, profile=profile, **options)
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


def measure_type(
    case: NativeAcceptance,
    kind: str,
    media: Path | None,
    pictures: dict[str, tuple[list[Pixel], bytes]] | None = None,
) -> dict[str, Any]:
    """Render one type through the worker and return what each format measured.

    Args:
        case: The acceptance fixture whose worker renders.
        kind: The admitted native FX type.
        media: The media tools, or None when a movie cannot be made.
        pictures: When given, receives the type's decoded PNG pixels and decoded movie bytes.
    Returns:
        `{"png_sequence": {...}, "mp4": {...}}`: for a format, either the measurements or a
        `skipped` reason; the PNG entry always carries the decoded-pixel digest.
    """
    document, artwork = _artwork_fixture(case)
    effect = _wired_effect(case, document, kind, artwork)
    _output(case, document, effect)
    for name, value in NATIVE_PRESETS[kind].items():
        case.mutate("fx.parameter.set", document, fx_id=effect, parameter=name, value=value)
    preview = preview_pixels(case, document)
    png, _artifact = _read_png(case, _submit(case, document, "png"))
    result: dict[str, Any] = {
        "png_sequence": {
            "digest": pixel_digest(png),
            "preview_difference": png_difference(preview, png),
        }
    }
    movie, decoded = _measure_movie(case, document, png, media)
    result["mp4"] = movie
    if pictures is not None and decoded:
        pictures[kind] = (png, decoded)
    return result


def _measure_movie(
    case: NativeAcceptance, document: str, png: list[Pixel], media: Path | None
) -> tuple[dict[str, Any], bytes]:
    if media is None:
        return {"skipped": "no media tools were configured for the run (--media-tools)"}, b""
    job = _submit(case, document, "mp4")
    artifact = case.success("job.result", {"job_id": job})["artifacts"][0]
    case.assertEqual((artifact["width"], artifact["height"]), (SIZE, SIZE))
    case.assertEqual(artifact["frame_count"], 1)
    decoded = _decode_movie(case, media, artifact)
    case.assertEqual(len(decoded), SIZE * SIZE * 3)
    measured = {
        "codec": artifact["codec"],
        "decoded_digest": hashlib.sha256(decoded).hexdigest(),
        **movie_difference(black_matte(png), decoded),
    }
    return measured, decoded


def separation(kind: str, pictures: dict[str, tuple[list[Pixel], bytes]]) -> dict[str, float | str]:
    """How far a type's decoded movie is from the nearest picture of another type.

    Types whose PNG pixels are identical are the same picture and are not counted. The movie
    oracle can only tell a wrong effect from the right one if its tolerance is smaller than this.
    """
    own_png, decoded = pictures[kind]
    own = pixel_digest(own_png)
    nearest: tuple[float, str] = (math.inf, "")
    for other, (other_png, _bytes) in pictures.items():
        if other == kind or pixel_digest(other_png) == own:
            continue
        distance = max(block_errors(black_matte(other_png), decoded))
        nearest = min(nearest, (distance, other))
    return {"nearest_other_block_error": round(nearest[0], 3), "nearest_other_type": nearest[1]}


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
) -> tuple[dict[str, dict[str, Any]], dict[str, str], dict[str, tuple[list[Pixel], bytes]]]:
    """Measure every listed type, a fresh worker for each few so the job limit is never met.

    Returns:
        The measurements by type, the run's identity, and the decoded pictures by type (for
        the checks that compare one type's movie with another type's picture).
    """
    results: dict[str, dict[str, Any]] = {}
    pictures: dict[str, tuple[list[Pixel], bytes]] = {}
    identity: dict[str, str] = {}
    for first in range(0, len(types), TYPES_PER_WORKER):
        worker = _worker(case, media, f"matrix-{first}")
        identity = identity or _identity(case, worker)
        for kind in types[first : first + TYPES_PER_WORKER]:
            results[kind] = measure_type(case, kind, media, pictures)
        worker.close()
    for kind in pictures:
        results[kind]["mp4"].update(separation(kind, pictures))
    return results, identity, pictures


def movie_tolerance(measured: dict[str, Any]) -> dict[str, Any]:
    """The tolerance a recorded movie cell states: the observed error with its margin."""

    def bound(value: float) -> float:
        return float(math.ceil(value * MARGIN) + 1)

    return {
        "block_error": bound(measured["block_error"]),
        "mean_error": bound(measured["mean_error"]),
        "decoded_differing_bytes": 0,
        "why": MP4_WHY,
    }


def cell_from(
    measured: dict[str, Any],
    form: str,
    references: dict[str, str],
    tolerance: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """The cell a measurement makes: passed when inside its tolerance, failed otherwise.

    Args:
        measured: What `measure_type` returned for the format.
        form: `png_sequence` or `mp4`.
        references: The digests to compare with: `png` always, `decoded` for a movie.
        tolerance: The stored tolerance of a movie cell; recording derives it from the
            measurement.
    """
    if "skipped" in measured:
        return {"status": "unavailable", "reason": measured["skipped"]}
    cell: dict[str, Any] = {"reference": references["png"]}
    if form == "png_sequence":
        observed: dict[str, float] = dict(png_difference_to(measured, references["png"]))
        tolerance = PNG_TOLERANCE
    else:
        tolerance = tolerance or movie_tolerance(measured)
        keys = ("mean_error", "block_error", "max_error")
        observed = {key: measured[key] for key in keys}
        observed["decoded_differing_bytes"] = int(
            measured["decoded_digest"] != references["decoded"]
        )
        cell["decoded_reference"] = references["decoded"]
        if "nearest_other_block_error" in measured:
            cell["separation"] = {
                key: measured[key] for key in ("nearest_other_block_error", "nearest_other_type")
            }
    limits = {n: float(v) for n, v in tolerance.items() if not isinstance(v, str)}
    over = [name for name in limits if observed[name] > limits[name]]
    blind = _blind(cell, tolerance)
    status = "failed" if over else "unverified" if blind else "passed"
    cell.update(status=status, tolerance=dict(tolerance), observed=observed)
    if over or blind:
        cell["reason"] = "outside the tolerance on " + ", ".join(over) if over else blind
    return cell


def _blind(cell: dict[str, Any], tolerance: dict[str, Any]) -> str:
    """Why a movie tolerance cannot tell this picture from another type's, or empty."""
    near = cell.get("separation")
    if near and tolerance["block_error"] >= near["nearest_other_block_error"]:
        return (
            f"the block tolerance {tolerance['block_error']} does not separate this picture from "
            f"{near['nearest_other_type']}'s (block error {near['nearest_other_block_error']}); "
            "the movie cannot confirm it, the PNG cell of the type is the check"
        )
    return ""


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
            if "skipped" in entry or cell["status"] not in {"passed", "unverified"}:
                continue
            references = {"png": cell["reference"], "decoded": cell.get("decoded_reference", "")}
            now = cell_from(entry, form, references, cell.get("tolerance"))
            if now["status"] != cell["status"]:
                found.append(
                    f"{kind} / {form}: recorded {cell['status']}, now {now['status']}: "
                    f"{now.get('reason', '')}; observed {now['observed']}"
                )
    return found


def wrong_pictures_fail(
    case: NativeAcceptance,
    types: Sequence[str],
    pictures: dict[str, tuple[list[Pixel], bytes]],
) -> None:
    """Score each passed movie against the hardest other type's picture: it must fail.

    This is the swap of two types' renders: a movie of one effect offered as another's picture.
    """
    cells = load_data()["cells"]
    platform = host_platform()
    for kind in types:
        cell = cells[kind]["mp4"][platform]
        if cell["status"] != "passed" or kind not in pictures:
            continue
        other = cell["separation"]["nearest_other_type"]
        swapped = movie_difference(black_matte(pictures[other][0]), pictures[kind][1])
        limit = cell["tolerance"]["block_error"]
        case.assertGreater(swapped["block_error"], limit, f"{kind} scored against {other}")


def dropped_effect_fails(
    case: NativeAcceptance,
    kind: str,
    media: Path,
    pictures: dict[str, tuple[list[Pixel], bytes]],
) -> float:
    """Render the type's scene with the effect left out of the graph; its movie cell must fail.

    The output is the artwork itself, the way a graph looks when the effect is removed or left
    unconnected; the movie is scored against the picture of the scene with the effect.
    """
    cell = load_data()["cells"][kind]["mp4"][host_platform()]
    document, artwork = _artwork_fixture(case)
    _output(case, document, artwork)
    job = _submit(case, document, "mp4")
    artifact = case.success("job.result", {"job_id": job})["artifacts"][0]
    decoded = _decode_movie(case, media, artifact)
    scored = movie_difference(black_matte(pictures[kind][0]), decoded)
    case.assertGreater(scored["block_error"], cell["tolerance"]["block_error"], kind)
    return scored["block_error"]


DROPPED = ("STD_blurFx", "invertFx", "STD_embossFx", "STD_hsvScaleFx")


@acceptance_case
def fx_matrix_cells_match_their_record(case: NativeAcceptance) -> None:
    """Render every admitted type as PNG and MP4, compare each with its stored matrix cell, and
    show the movie check fails for another type's picture and for a dropped effect."""
    catalog = case.success("fx.catalog", {})
    types = [effect["type"] for effect in catalog["effects"]]
    data = load_data()
    case.assertEqual(
        sorted(data["cells"]), sorted(types), "the matrix grid is not the live catalog"
    )
    case.assertEqual(sorted(NATIVE_PRESETS), sorted(types))
    platform = host_platform()
    if data["platforms"][platform].get("latest_run") is None:
        case.skipTest(f"no run is recorded for {platform}: its cells stay not tested")
    results, _identity_text, pictures = measure(case, types, case.media_tools)
    for kind, measured in results.items():
        case.assertLessEqual(
            measured["png_sequence"]["preview_difference"]["max_color"], 0, f"{kind}: preview"
        )
    case.assertEqual(compare(case, results), [], "cells differ from the matrix record")
    if case.media_tools is None:
        return
    wrong_pictures_fail(case, types, pictures)
    _worker(case, case.media_tools, "matrix-dropped")
    for kind in DROPPED:
        dropped_effect_fails(case, kind, case.media_tools, pictures)
