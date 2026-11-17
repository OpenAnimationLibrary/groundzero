"""Exercise protocol EOF while the native MP4 writer awaits encoder finalization."""

from __future__ import annotations

import ctypes
import json
import os
import shutil
import signal
import stat
import sys
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


def _binary(directory: Path, name: str) -> Path:
    return directory / (f"{name}.exe" if os.name == "nt" else name)


def _private_media(case: NativeAcceptance) -> Path:
    source = case.media_tools
    if source is None:
        case.fail("Movie EOF qualification requires configured real media helpers")
    directory = case.directory / "eof-media-tools"
    directory.mkdir()
    for name in ("ffmpeg", "ffprobe"):
        shutil.copy2(_binary(source, name), _binary(directory, name))
    for library in source.glob("*.dll"):
        shutil.copy2(library, directory / library.name)
    return directory.resolve()


def _start_worker(case: NativeAcceptance, media: Path) -> None:
    case.success("automation.capabilities", {})
    case.worker.close()
    case.assertEqual(case.worker.process.returncode, 0)
    case.workspace = case.directory / "eof-workspace"
    case.workspace.mkdir()
    case.worker = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / "movie-eof.stderr.log",
        media_tools=media,
    )
    case.addCleanup(case.worker.close)


def _replace_encoder(case: NativeAcceptance, media: Path) -> None:
    fixture = _binary(case.executable.parent, "mcp_ffprobe_fixture")
    case.assertTrue(fixture.is_file(), f"Missing compiled encoder fixture: {fixture}")
    encoder = _binary(media, "ffmpeg")
    encoder.chmod(encoder.stat().st_mode | stat.S_IWUSR)
    encoder.unlink()
    shutil.copy2(fixture, encoder)


def _windows_alive(pid: int) -> bool:
    if sys.platform != "win32":
        raise OSError("Windows process observation requires Windows")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int32, ctypes.c_uint32]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel.WaitForSingleObject.restype = ctypes.c_uint32
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.restype = ctypes.c_int32
    handle = kernel.OpenProcess(0x100000, False, pid)
    if not handle:
        error = ctypes.get_last_error()
        if error == 87:
            return False
        raise OSError(error, f"Cannot inspect the owned encoder process {pid}")
    try:
        status = kernel.WaitForSingleObject(handle, 0)
        if status not in (0, 258):
            raise OSError(f"Cannot inspect encoder process {pid}: wait result {status}")
        return status == 258
    finally:
        kernel.CloseHandle(handle)


def _alive(pid: int) -> bool:
    if os.name == "nt":
        return _windows_alive(pid)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def _cleanup_fixture(pid: int) -> None:
    if _alive(pid):
        try:
            os.kill(pid, signal.SIGTERM if os.name == "nt" else signal.SIGKILL)
        except ProcessLookupError:
            return


def _await_encoder(case: NativeAcceptance, job_id: str) -> tuple[Path, dict[str, Any]]:
    directory = case.workspace / "renders" / job_id
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        status = case.success("render.status", {"job_id": job_id})
        case.assertNotIn(status["status"], {"completed", "canceled", "failed"}, status)
        markers = list(directory.glob("*.encoder-eof.json"))
        if markers:
            case.assertEqual(len(markers), 1)
            return markers[0], case.success("render.status", {"job_id": job_id})
        time.sleep(0.05)
    case.fail("The native encoder did not consume all frames and reach its EOF stall")


def _close_input(case: NativeAcceptance) -> float:
    stream = case.worker.process.stdin
    if stream is None:
        case.fail("The native worker did not retain its protocol input pipe")
    started = time.monotonic()
    stream.close()
    case.worker.process.wait(timeout=15)
    elapsed = time.monotonic() - started
    diagnostics = case.worker.log_path.read_text(encoding="utf-8", errors="replace")
    case.assertEqual(case.worker.process.returncode, 0, diagnostics)
    return elapsed


def movie_eof_interrupts_encoder_finalization(case: NativeAcceptance) -> None:
    """Cancel a stalled native encoder before quitting the application's event loop.

    Args:
        case: A fresh native-worker fixture with real media tools configured.
    """
    case.assertEqual(os.environ.get("OTDEV_FFMPEG_NO_PIPE", "0"), "0")
    media = _private_media(case)
    _start_worker(case, media)
    document, _level, _column = case.populated_scene()
    _replace_encoder(case, media)
    job = case.mutate(
        "render.submit", document, format="mp4", start_frame=0, end_frame=2, width=32, height=32
    )
    marker, status = _await_encoder(case, job["job_id"])
    observed = json.loads(marker.read_text(encoding="utf-8"))
    pid = observed["pid"]
    case.assertIsInstance(pid, int)
    case.assertGreater(pid, 0)
    case.assertNotEqual(pid, case.worker.process.pid)
    case.addCleanup(_cleanup_fixture, pid)
    case.assertEqual(observed["input_bytes"], 3 * 32 * 32 * 4)
    case.assertTrue(_alive(pid), observed)
    case.assertEqual(status["status"], "finalizing", status)
    case.assertEqual(status["completed_frames"], 3, status)
    case.assertIs(status["writers_closed"], False)
    denied = case.failure("render.artifacts", {"job_id": job["job_id"]})
    case.assertEqual(denied["code"], "artifacts_unavailable")
    temporary = Path(str(marker).removesuffix(".encoder-eof.json"))
    case.assertGreater(temporary.stat().st_size, 0)
    report: dict[str, Any] = {"job": job, "before_eof": status, "encoder": observed}
    evidence = case.directory / "movie-eof.json"
    evidence.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    elapsed = _close_input(case)
    case.assertFalse(_alive(pid), "The exited worker left its encoder running")
    case.assertFalse(temporary.exists(), "Aborted encoder output was not removed")
    case.assertFalse((marker.parent / "movie.mp4").exists(), "Canceled movie was published")
    report["exit_seconds"] = elapsed
    evidence.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
