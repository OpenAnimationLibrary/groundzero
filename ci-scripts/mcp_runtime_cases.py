"""Qualify runtime capability reporting: probes, media identity, formats and policy."""

from __future__ import annotations

import contextlib
import hashlib
import os
import shutil
import sys
from collections.abc import Generator
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_live import verified_build_digest
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

LAYERS = ("compiled", "installed", "configured", "loaded", "verified", "usable")
STUB_VERSION = b"echo 'ffmpeg version stub-1'; echo 'libavcodec 62. 28.102 / 62. 28.102'"


def _binary(directory: Path, name: str) -> Path:
    return directory / (f"{name}.exe" if os.name == "nt" else name)


@contextlib.contextmanager
def _environment(**values: str) -> Generator[None]:
    """Set environment variables for the processes started inside the block."""
    previous = {name: os.environ.get(name) for name in values}
    os.environ.update(values)
    try:
        yield
    finally:
        for name, value in previous.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value


def _worker(case: NativeAcceptance, name: str, media_tools: Path | None = None) -> Worker:
    workspace = case.directory / f"{name}-workspace"
    workspace.mkdir()
    worker = Worker(
        case.executable,
        workspace,
        case.inputs,
        case.directory / f"{name}.stderr.log",
        media_tools=media_tools,
    )
    case.addCleanup(worker.close)
    return worker


def _success(case: NativeAcceptance, worker: Worker, method: str, params: dict[str, Any]) -> Any:
    response = worker.call(method, params)
    case.assertNotIn("error", response, f"{method}: {response}")
    return response["result"]


def _inspect(case: NativeAcceptance, worker: Worker, *probes: str) -> dict[str, Any]:
    params = {"probes": list(probes)} if probes else {}
    return _success(case, worker, "runtime.inspect", params)


def stub_media_tools(case: NativeAcceptance, name: str, encoders: str) -> Path:
    """Write shell-script ffmpeg and ffprobe whose encoder list the case chooses.

    Args:
        case: The running acceptance case.
        name: Directory name under the case directory.
        encoders: The `-encoders` rows after the header.
    Returns:
        The canonical directory holding the two stubs.
    """
    if os.name == "nt":
        case.skipTest("shell-script media stubs are POSIX executables")
    directory = case.directory / name
    directory.mkdir()
    ffmpeg = (
        b"#!/bin/sh\n[ \"$1\" = -formats ] && echo ' E mp4  MP4 (MPEG-4 Part 14)' && exit 0\n"
        b'case "$2" in\n-version) '
        + STUB_VERSION
        + b";;\n-h) echo 'Muxer mp4:'; echo '    Default video codec: h264.';;\n"
        + b"-encoders) echo 'Encoders:'; echo ' ------'; echo '"
        + encoders.encode()
        + b"';;\nesac\n"
    )
    ffprobe = b"#!/bin/sh\necho 'ffprobe version stub-1'\n"
    for tool, body in (("ffmpeg", ffmpeg), ("ffprobe", ffprobe)):
        path = _binary(directory, tool)
        path.write_bytes(body)
        path.chmod(0o700)
    return directory.resolve()


def _sentinel_tools(case: NativeAcceptance) -> tuple[Path, Path]:
    """Put an ffmpeg and ffprobe on a directory that record any run in a sentinel file."""
    directory = case.directory / "path-tools"
    directory.mkdir()
    sentinel = case.directory / "path-tools-ran"
    for tool in ("ffmpeg", "ffprobe"):
        path = _binary(directory, tool)
        path.write_bytes(f"#!/bin/sh\necho {tool} >> '{sentinel}'\n".encode())
        path.chmod(0o700)
    return directory, sentinel


def _require_state(case: NativeAcceptance, state: dict[str, Any], **expected: Any) -> None:
    case.assertEqual({key: state[key] for key in expected}, expected, state)
    usable = (
        state["compiled"]
        and state["installed"] is not False
        and state["configured"] is not False
        and state["loaded"] is not False
        and state["verified"] in {"passed", "not_applicable"}
    )
    case.assertIs(state["usable"], usable, state)


def _require_worker_identity(case: NativeAcceptance, report: dict[str, Any]) -> None:
    worker = report["worker"]
    case.assertEqual(len(worker["instance_id"]), 36)
    build = worker["build"]
    case.assertEqual(set(build), {"application", "version", "build_digest"})
    case.assertEqual(build["build_digest"], verified_build_digest(case.native_executable))
    capabilities = case.success("automation.capabilities", {})
    case.assertEqual(worker, capabilities["worker"])
    case.assertEqual(report["platform"], capabilities["platform"])


def _require_toolsets(case: NativeAcceptance, report: dict[str, Any]) -> None:
    toolsets = {entry["name"]: entry["state"] for entry in report["toolsets"]}
    case.assertIn("core", toolsets)
    case.assertEqual(toolsets["core"]["activation"], {"kind": "always", "name": None})
    for name, state in toolsets.items():
        _require_state(case, state, configured=True, usable=True)
        if name != "core":
            case.assertEqual(state["activation"], {"kind": "toolset", "name": name})
    if not case.through_sdk:
        return
    case.success("toolsets.disable", {"names": ["audio"]})
    disabled = {entry["name"]: entry["state"] for entry in _inspect(case, case.worker)["toolsets"]}
    _require_state(case, disabled["audio"], configured=False, usable=False)
    case.assertIn("toolset_disabled", disabled["audio"]["reasons"])
    case.assertEqual(disabled["audio"]["activation"], {"kind": "toolset", "name": "audio"})
    case.success("toolsets.enable", {"names": ["audio"]})


@acceptance_case
def runtime_states_distinguish_layers(case: NativeAcceptance) -> None:
    """Report compiled, configured, loaded, verified and usable separately per capability."""
    report = _inspect(case, case.worker)
    _require_worker_identity(case, report)
    media = report["media"]["state"]
    _require_state(case, media, configured=False, verified="not_run", usable=False)
    case.assertEqual(media["activation"], {"kind": "launch_option", "name": "--media-tools"})
    case.assertIn("not_configured", media["reasons"])
    graphics = report["graphics"]
    _require_state(case, graphics["state"], verified="not_run", usable=False)
    case.assertIsNone(graphics["probe"])
    case.assertEqual(graphics["required_by"], "every_render")
    unconfigured = _inspect(case, case.worker, "media")["media"]
    case.assertIsNone(unconfigured["ffmpeg"])
    case.assertEqual(unconfigured["state"]["verified"], "not_run")
    probed = _inspect(case, case.worker, "graphics")["graphics"]
    case.assertEqual(probed["probe"]["result"], "context_ready", probed)
    _require_state(case, probed["state"], verified="passed", usable=True)
    case.assertEqual(_inspect(case, case.worker)["graphics"]["probe"]["result"], "context_ready")
    _require_toolsets(case, report)
    configured = _worker(case, "media", media_tools=case.media_tools)
    state = _inspect(case, configured, "media")["media"]["state"]
    _require_state(case, state, configured=True, loaded=True, verified="passed", usable=True)


@acceptance_case
def runtime_media_identity(case: NativeAcceptance) -> None:
    """Report the granted media tools' hashes and versions and never run a PATH ffmpeg."""
    directory = case.media_tools
    if directory is None:
        case.fail("Media identity qualification requires --media-tools")
    path_tools, sentinel = _sentinel_tools(case)
    search = str(path_tools) + os.pathsep + os.environ.get("PATH", "")
    with _environment(PATH=search):
        unconfigured = _worker(case, "path-only")
        _inspect(case, unconfigured, "media", "graphics")
        _success(case, unconfigured, "render.capabilities", {})
        configured = _worker(case, "granted", media_tools=directory)
        before = _inspect(case, configured)
        case.assertIsNone(before["media"]["ffmpeg"])
        report = _inspect(case, configured, "media")["media"]
    case.assertFalse(sentinel.exists(), "A media tool on PATH was executed")
    for tool in ("ffmpeg", "ffprobe"):
        identity = report[tool]
        binary = _binary(directory, tool).read_bytes()
        case.assertEqual(identity["file"], _binary(directory, tool).name)
        case.assertEqual(identity["sha256"], hashlib.sha256(binary).hexdigest())
        case.assertEqual(identity["size_bytes"], len(binary))
        case.assertTrue(identity["version_line"].startswith(f"{tool} version"), identity)
        case.assertTrue(identity["library_versions"], identity)
    mp4 = report["mp4"]
    case.assertTrue(mp4["muxer_default_video_codec"], mp4)
    case.assertIs(mp4["encoder_available"], True, mp4)
    case.assertIsInstance(report["probed_ms"], int)
    restarted = _worker(case, "restarted", media_tools=directory)
    case.assertEqual(_inspect(case, restarted)["media"]["state"]["verified"], "not_run")


@acceptance_case
def runtime_formats_native_vs_admitted(case: NativeAcceptance) -> None:
    """Separate what the native registries list from what the tools admit."""
    formats = _inspect(case, case.worker)["formats"]
    native = formats["native"]
    case.assertIn("png", native["image_read"])
    case.assertIn("png", native["image_write"])
    case.assertIn("wav", native["sound_read"])
    admitted = {(row["tool"], row["direction"]): row["formats"] for row in formats["admitted"]}
    case.assertEqual(admitted[("level.import", "read")], ["png"])
    case.assertEqual(admitted[("audio.import", "read")], ["wav", "aiff"])
    case.assertEqual(admitted[("render.submit", "render")], ["png"])
    unadmitted = [name for name in native["image_read"] if name not in {"png"}]
    case.assertTrue(unadmitted, "The native image registry lists only PNG")
    configured = _worker(case, "formats-media", media_tools=case.media_tools)
    rows = _inspect(case, configured, "media")["formats"]["admitted"]
    rendered = {row["tool"]: row["formats"] for row in rows if row["direction"] == "render"}
    case.assertEqual(rendered["render.submit"], ["png", "mp4"])


@acceptance_case
def runtime_reports_private_stuff_and_plugin_policy(case: NativeAcceptance) -> None:
    """Report built-in-only plugins, the empty private stuff folder and fonts."""
    report = _inspect(case, case.worker)
    case.assertEqual(report["plugins"], {"policy": "builtin_only", "plugin_fx": 0, "shader_fx": 0})
    case.assertEqual(report["stuff"], {"kind": "private_runtime", "library": "empty"})
    case.assertEqual(report["components"], {"see": "assistance.inspect"})
    fonts = report["fonts"]
    case.assertGreater(fonts["families"], 0)
    _require_state(case, fonts["state"], installed=True, verified="not_applicable", usable=True)


@acceptance_case
def runtime_media_without_encoder_is_not_usable(case: NativeAcceptance) -> None:
    """Refuse to call media usable when ffmpeg lists no encoder for the muxer's codec."""
    directory = stub_media_tools(case, "no-encoder-tools", " V....D mpeg4  MPEG-4 part 2")
    worker = _worker(case, "no-encoder", media_tools=directory)
    media = _inspect(case, worker, "media")["media"]
    _require_state(case, media["state"], configured=True, loaded=True, verified="failed")
    case.assertIn("codec_unavailable", media["state"]["reasons"])
    case.assertEqual(
        media["mp4"],
        {
            "writer_registered": True,
            "muxer_default_video_codec": "h264",
            "encoder": None,
            "encoder_available": False,
        },
    )
    case.assertEqual(media["ffmpeg"]["version_line"], "ffmpeg version stub-1")


CHECK_ORDER = [
    "frame_range",
    "dimensions",
    "scene_numbering",
    "fx_graph",
    "fx_extent",
    "graphics",
    "writer",
    "media_tools",
    "encoder",
    "dependencies",
    "storage",
    "lane",
    "retention",
    "render_admission",
    "source_pins",
]


def _use_worker(case: NativeAcceptance, name: str, media_tools: Path | None = None) -> None:
    """Make a fresh worker the case's own, so its scene helpers address it."""
    case.workspace = case.directory / f"{name}-workspace"
    case.workspace.mkdir()
    case.worker = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / f"{name}.stderr.log",
        media_tools=media_tools,
    )
    case.addCleanup(case.worker.close)
    case.revisions.clear()


def _preflight(case: NativeAcceptance, document: str, **params: Any) -> dict[str, Any]:
    request = {"document_id": document, "expected_revision": case.revisions[document], **params}
    return case.success("render.preflight", request)


def _refused(case: NativeAcceptance, method: str, document: str, **params: Any) -> dict[str, Any]:
    """Require a render refusal that leaves no job output and an unchanged revision."""
    before = case.success("scene.summary", {"document_id": document})
    error = case.failure(method, case.write_params(document, params))
    case.assertFalse((case.workspace / "renders").exists(), error)
    case.assertEqual(case.success("scene.summary", {"document_id": document}), before)
    return error


@acceptance_case
def render_preflight_reports_every_failure(case: NativeAcceptance) -> None:
    """Report every failing check at once; the job tools refuse with the first one."""
    tools = stub_media_tools(case, "preflight-tools", " V....D mpeg4  MPEG-4 part 2")
    _use_worker(case, "preflight", media_tools=tools)
    document = case.create("Preflight")
    broken = {"start_frame": 10, "end_frame": 5, "format": "mp4", "width": 33, "height": 32}
    report = _preflight(case, document, kind="sequence", **broken)
    case.assertEqual([check["id"] for check in report["checks"]], CHECK_ORDER)
    failed = {check["id"]: check for check in report["checks"] if check["status"] == "fail"}
    case.assertEqual(set(failed), {"frame_range", "dimensions", "encoder"}, report)
    case.assertEqual(failed["frame_range"]["code"], "invalid_frame_range")
    case.assertEqual(failed["dimensions"]["code"], "unsupported_render_settings")
    case.assertEqual(failed["encoder"]["code"], "render_unavailable")
    case.assertEqual(failed["encoder"]["details"]["cause"], "codec_unavailable")
    case.assertEqual((report["ready"], report["first_failure"]), (False, "frame_range"))
    case.assertIsNone(report["effective"])
    case.assertEqual(sorted(report["probes_run"]), ["graphics", "media"])
    case.assertEqual(report["revision"], case.revisions[document])
    error = _refused(case, "render.submit", document, **broken)
    case.assertEqual(error["code"], "invalid_params")
    case.assertEqual(error["details"]["reason"], "end_before_start")
    case.assertEqual(error["details"]["check"], "frame_range")
    case.assertFalse((case.workspace / "renders").exists())
    paths = set(case.workspace.rglob("*"))
    ready = _preflight(case, document, kind="sequence", start_frame=0, end_frame=2)
    case.assertEqual(set(case.workspace.rglob("*")), paths, "A preflight wrote files")
    case.assertEqual(
        (ready["ready"], ready["first_failure"], ready["probes_run"]), (True, None, [])
    )
    case.assertEqual(ready["effective"]["frames"], {"start": 0, "end": 2, "step": 1, "count": 3})
    stale = {"document_id": document, "expected_revision": "999", "kind": "preview", "frame": 0}
    case.assertEqual(case.failure("render.preflight", stale)["code"], "stale_revision")
    for shape in ({"kind": "preview", "frame": 0, "start_frame": 0}, {"kind": "sequence"}):
        request = {"document_id": document, "expected_revision": case.revisions[document], **shape}
        case.assertEqual(case.failure("render.preflight", request)["code"], "invalid_params")


@acceptance_case
def mp4_refused_without_encoder(case: NativeAcceptance) -> None:
    """Refuse MP4 before any job when ffmpeg has no encoder for the muxer's codec."""
    tools = stub_media_tools(case, "no-encoder-tools", " V....D mpeg4  MPEG-4 part 2")
    _use_worker(case, "no-encoder", media_tools=tools)
    capabilities = case.success("render.capabilities", {})
    case.assertEqual(capabilities["profiles"], ["rgba8-png"])
    case.assertIs(capabilities["mp4"]["encoder_available"], False)
    case.assertNotIn("graphics_backend", capabilities)
    document = case.create("No encoder")
    movie = {"start_frame": 0, "end_frame": 1, "format": "mp4", "width": 32, "height": 32}
    error = _refused(case, "render.submit", document, **movie)
    case.assertEqual(error["code"], "render_unavailable")
    case.assertEqual(
        (error["details"]["cause"], error["details"]["check"]), ("codec_unavailable", "encoder")
    )


@acceptance_case
def runtime_unavailable_backends_are_not_advertised(case: NativeAcceptance) -> None:
    """Under Qt's offscreen platform on macOS, report no GL and refuse renders before a job."""
    if sys.platform != "darwin":
        case.skipTest("the offscreen platform has no OpenGL integration only on macOS")
    with _environment(QT_QPA_PLATFORM="offscreen"):
        _use_worker(case, "offscreen")
        graphics = _inspect(case, case.worker, "graphics")["graphics"]
        capabilities = case.success("render.capabilities", {})
        document = case.create("No GL")
        error = _refused(case, "render.preview", document, frame=0)
    case.assertEqual(graphics["qpa_platform"], "offscreen")
    case.assertNotEqual(graphics["probe"]["result"], "context_ready", graphics)
    _require_state(case, graphics["state"], verified="failed", usable=False)
    case.assertEqual(capabilities["profiles"], [])
    case.assertIs(capabilities["graphics"]["state"]["usable"], False)
    case.assertEqual(error["code"], "render_unavailable")
    case.assertEqual(
        (error["details"]["cause"], error["details"]["check"]), ("gl_unavailable", "graphics")
    )


@acceptance_case
def preview_reports_effective_output(case: NativeAcceptance) -> None:
    """Report the preview fit, crop, color policy and camera DPI before and with the job."""
    document = case.mutate(
        "scene.create",
        None,
        name="Effective",
        width_px=1920,
        height_px=1080,
        width_in=16,
        height_in=9,
        frame_rate=24,
    )["document_id"]
    camera = case.success("camera.inspect", {"document_id": document})
    report = _preflight(case, document, kind="preview", frame=0)
    effective = report["effective"]
    case.assertEqual((effective["width"], effective["height"]), (1024, 576))
    case.assertAlmostEqual(effective["fit_scale"], 1024 / 1920)
    case.assertEqual(
        effective["crop"],
        {"kind": "full_camera", "camera_pixels": {"x0": 0, "y0": 0, "x1": 1920, "y1": 1080}},
    )
    dpi = effective["camera"]["dpi"]
    case.assertAlmostEqual(dpi["x"], camera["width_px"] / camera["width_in"])
    case.assertAlmostEqual(dpi["y"], camera["height_px"] / camera["height_in"])
    case.assertEqual(effective["camera"]["resolution"], {"width": 1920, "height": 1080})
    expected = {"profile": "rgba8-png", "alpha": "straight", "color_depth_bits": 8}
    case.assertEqual({key: effective[key] for key in expected}, expected)
    case.assertEqual((effective["frame_rate"], effective["audio"]), (24, False))
    job = case.mutate("render.preview", document, frame=0)
    case.assertEqual(job["effective"], effective)
    case.wait_job(job["job_id"])
    sized = _preflight(
        case, document, kind="sequence", start_frame=0, end_frame=0, width=64, height=36
    )
    case.assertEqual(sized["effective"]["fit_scale"], 1)


@acceptance_case
def media_tools_replaced_after_launch_are_refused(case: NativeAcceptance) -> None:
    """Refuse MP4 once a granted media tool changes, and accept an unchanged re-stat."""
    source = case.media_tools
    if source is None:
        case.fail("Media replacement qualification requires --media-tools")
    tools = case.directory / "replaced-tools"
    tools.mkdir()
    for name in ("ffmpeg", "ffprobe"):
        shutil.copy2(_binary(source, name), _binary(tools, name))
    _use_worker(case, "replaced", media_tools=tools.resolve())
    document = case.create("Replaced tools")
    movie = {"start_frame": 0, "end_frame": 1, "format": "mp4", "width": 32, "height": 32}
    case.assertTrue(_preflight(case, document, kind="sequence", **movie)["ready"])
    probe = _binary(tools, "ffprobe")
    os.utime(probe)
    case.assertTrue(_preflight(case, document, kind="sequence", **movie)["ready"])
    probe.chmod(0o700)
    with probe.open("ab") as handle:
        handle.write(b"\0")
    error = _refused(case, "render.submit", document, **movie)
    case.assertEqual(error["code"], "render_unavailable")
    case.assertEqual(error["details"]["cause"], "media_tools_changed")
    state = _inspect(case, case.worker)["media"]["state"]
    _require_state(case, state, verified="failed", usable=False)
    case.assertIn("media_tools_changed", state["reasons"])
