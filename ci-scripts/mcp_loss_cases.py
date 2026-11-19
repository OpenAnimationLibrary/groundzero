"""Lose the encoder, the destination's permissions or the whole volume during a render job."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_failure_cases import (
    SEQUENCE,
    _assert_nothing_published,
    _cancel,
    _descendants,
    _ends,
    _scene,
    _settle_processes,
    _status,
)
from mcp_movie_eof import _alive, _await_encoder, _close_input, _private_media, _replace_encoder
from mcp_movie_eof import _start_worker as _start_movie_worker
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

ENCODER_MODE = "GROUNDZERO_TEST_ENCODER_MODE"
LONG_RUN = {"start_frame": 0, "end_frame": 119, "width": 384, "height": 384}
CANCEL_SECONDS = 20
LOSS_SECONDS = 60


def _wait_frames(case: NativeAcceptance, job: str, at_least: int) -> dict[str, Any]:
    """Wait until a running job has rendered some frames and is still far from done."""
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        status = _status(case, job)
        case.assertNotIn(status["status"], {"completed", "failed", "canceled"}, status)
        if status["completed_frames"] >= at_least:
            case.assertLess(status["completed_frames"], status["frame_count"], status)
            return status
        time.sleep(0.02)
    case.fail(f"Job {job} rendered fewer than {at_least} frames in 60 seconds")


def _blocked_at(case: NativeAcceptance, job: str) -> dict[str, Any]:
    """Wait until a job's completed frames have not moved for 1.5 seconds."""
    deadline = time.monotonic() + 40
    last, since = -1, time.monotonic()
    while time.monotonic() < deadline:
        status = _status(case, job)
        case.assertNotIn(status["status"], {"completed", "failed", "canceled"}, status)
        if status["completed_frames"] != last:
            last, since = status["completed_frames"], time.monotonic()
        elif time.monotonic() - since > 1.5:
            return status
        time.sleep(0.05)
    case.fail(f"Job {job} never stopped advancing behind a full encoder pipe")


def _without_encoder_input(case: NativeAcceptance) -> None:
    """Start a worker whose encoder never reads its input."""
    previous = os.environ.get(ENCODER_MODE)
    os.environ[ENCODER_MODE] = "no-read"

    def restore() -> None:
        if previous is None:
            os.environ.pop(ENCODER_MODE, None)
        else:
            os.environ[ENCODER_MODE] = previous

    case.addCleanup(restore)
    media = _private_media(case)
    _replace_encoder(case, media)
    _start_movie_worker(case, media)


@acceptance_case
def render_cancel_with_a_full_encoder_pipe_leaves_nothing(case: NativeAcceptance) -> None:
    """Cancel a movie job whose encoder stopped reading: it ends canceled, nothing is left."""
    if os.name == "nt":
        case.skipTest("The fixture reads POSIX process tables")
    if case.media_tools is None:
        case.skipTest("Needs --media-tools with the real ffmpeg binaries")
    _without_encoder_input(case)
    document = _scene(case, 40)
    baseline = _descendants(case.worker.process.pid)
    movie = {
        "profile": "opaque8-mp4",
        "start_frame": 0,
        "end_frame": 39,
        "width": 512,
        "height": 512,
    }
    job = case.mutate("render.submit", document, **movie)["job_id"]
    marker, _ = _await_encoder(case, job)
    pid = json.loads(marker.read_text(encoding="utf-8"))["pid"]
    case.addCleanup(lambda: os.kill(pid, 9) if _alive(pid) else None)
    blocked = _blocked_at(case, job)
    case.assertLess(blocked["completed_frames"], 40, blocked)
    case.assertIs(blocked["writers_closed"], False)
    case.assertEqual(case.failure("job.result", {"job_id": job})["code"], "artifacts_unavailable")
    started = time.monotonic()
    _cancel(case, job)
    _ends(case, job, "canceled")
    case.assertLess(time.monotonic() - started, CANCEL_SECONDS)
    deadline = time.monotonic() + 10
    while _alive(pid) and time.monotonic() < deadline:
        time.sleep(0.1)
    case.assertFalse(_alive(pid), "The canceled job left its encoder running")
    _assert_nothing_published(case, job)
    case.assertEqual(list(case.workspace.rglob("*.mp4")), [])
    _settle_processes(case, baseline)
    case.assertLess(_close_input(case), 5)


@acceptance_case
def render_losing_write_permission_ends_failed_and_recovers(case: NativeAcceptance) -> None:
    """Revoke write permission on a running job's frame folder: it fails cleanly, then recovers."""
    if os.name == "nt":
        case.skipTest("POSIX permission bits do not deny writes on Windows")
    document = _scene(case, 120)
    job = case.mutate("render.submit", document, **LONG_RUN)["job_id"]
    _wait_frames(case, job, 2)
    frames = case.workspace / "renders" / job / "frames"
    case.addCleanup(frames.chmod, 0o755)
    frames.chmod(0o555)
    if os.access(frames, os.W_OK):
        case.skipTest("The test user can write without permission")
    started = time.monotonic()
    ended = case.wait_job(job)
    case.assertLess(time.monotonic() - started, LOSS_SECONDS)
    case.assertEqual(ended["status"], "failed", ended)
    case.assertIs(ended["writers_closed"], True)
    case.assertTrue(ended["failure"]["message"], ended)
    case.assertLess(ended["completed_frames"], ended["frame_count"], ended)
    error = case.failure("job.result", {"job_id": job})
    case.assertEqual(error["code"], "artifacts_unavailable", error)
    frames.chmod(0o755)
    fresh = case.mutate("render.submit", document, **SEQUENCE)["job_id"]
    case.assertEqual(case.wait_job(fresh)["status"], "completed")
    case.assertEqual(
        case.success("job.result", {"job_id": fresh})["job"]["status"], "completed", fresh
    )


def _volume(case: NativeAcceptance) -> Path:
    """Attach a small APFS disk image and make sure it is force-detached when the case ends."""
    image = case.directory / "removable.dmg"
    mount = case.directory / "removable-volume"
    mount.mkdir()
    subprocess.run(
        ["hdiutil", "create", "-size", "32m", "-fs", "APFS", "-volname", "GONE", str(image)],
        check=True,
        capture_output=True,
    )
    subprocess.run(
        ["hdiutil", "attach", str(image), "-mountpoint", str(mount), "-nobrowse"],
        check=True,
        capture_output=True,
    )
    case.addCleanup(
        subprocess.run, ["hdiutil", "detach", "-force", str(mount)], capture_output=True
    )
    return mount


@acceptance_case
def render_survives_its_volume_being_removed(case: NativeAcceptance) -> None:
    """Detach the volume under a running render: the job ends, the worker answers and exits."""
    if sys.platform != "darwin":
        case.skipTest("Builds its removable volume with hdiutil, which only macOS provides")
    mount = _volume(case)
    workspace = mount / "workspace"
    workspace.mkdir()
    case.worker.close()
    case.workspace = workspace
    case.worker = Worker(
        case.executable, workspace, case.inputs, case.directory / "volume-worker.stderr.log"
    )
    case.addCleanup(case.worker.close)
    document = _scene(case, 120)
    job = case.mutate("render.submit", document, **LONG_RUN)["job_id"]
    _wait_frames(case, job, 2)
    removed = subprocess.run(
        ["hdiutil", "detach", "-force", str(mount)], capture_output=True, check=False
    )
    case.assertEqual(removed.returncode, 0, removed.stderr)
    started = time.monotonic()
    ended = case.wait_job(job)
    case.assertLess(time.monotonic() - started, LOSS_SECONDS)
    case.assertIn(ended["status"], {"failed", "canceled"}, ended)
    case.assertIs(ended["writers_closed"], True)
    case.assertEqual(case.success("automation.capabilities", {})["protocol_version"], 5)
    case.assertEqual(case.failure("job.result", {"job_id": job})["code"], "artifacts_unavailable")
    # The workspace journal went with the volume, so no write is admitted any more.
    refused = case.failure("render.submit", case.write_params(document, SEQUENCE))
    case.assertEqual(refused["code"], "journal_unavailable", refused)
    case.assertTrue(refused["message"], refused)
    case.assertEqual(list(mount.iterdir()), [], "output was written to the host disk")
    case.assertLess(_close_input(case), 8)
