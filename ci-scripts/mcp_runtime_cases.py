"""Qualify runtime capability reporting: probes, media identity, formats and policy."""

from __future__ import annotations

import contextlib
import hashlib
import os
from collections.abc import Generator
from pathlib import Path
from typing import TYPE_CHECKING, Any

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
    case.assertEqual(build["application"], "groundzero")
    digest = hashlib.sha256(case.native_executable.read_bytes()).hexdigest()
    case.assertEqual(build["executable_sha256"], digest)
    platform = case.success("automation.capabilities", {})["platform"]
    case.assertEqual(report["platform"], platform)


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


def runtime_reports_private_stuff_and_plugin_policy(case: NativeAcceptance) -> None:
    """Report built-in-only plugins, the empty private stuff folder and fonts."""
    report = _inspect(case, case.worker)
    case.assertEqual(report["plugins"], {"policy": "builtin_only", "plugin_fx": 0, "shader_fx": 0})
    case.assertEqual(report["stuff"], {"kind": "private_runtime", "library": "empty"})
    case.assertEqual(report["components"], {"see": "assistance.inspect"})
    fonts = report["fonts"]
    case.assertGreater(fonts["families"], 0)
    _require_state(case, fonts["state"], installed=True, verified="not_applicable", usable=True)


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
