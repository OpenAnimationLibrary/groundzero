"""Inject failures into previews, render jobs and file writes, and check what is left behind."""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_audio_cases import Clip, _column, _import, _place
from mcp_audio_cases import _scene as _audio_scene
from mcp_audio_model import tone, wav_file
from mcp_scene_access_cases import _op, corpus, host_processes, open_scene, project_id, wait
from mcp_studio_palette_cases import call, refuse
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

TERMINAL = {"completed", "failed", "canceled", "deadline_exceeded"}
SEQUENCE = {"start_frame": 0, "end_frame": 3, "width": 32, "height": 32}


def _status(case: NativeAcceptance, job: str) -> dict[str, Any]:
    return case.success("render.status", {"job_id": job})


def _cancel(case: NativeAcceptance, job: str) -> dict[str, Any]:
    return case.success("render.cancel", case.write_params(None, {"job_id": job}))


def _result(case: NativeAcceptance, job: str) -> dict[str, Any]:
    return case.worker.call("render.artifacts", {"job_id": job})


def _descendants(pid: int) -> set[int]:
    """Every process below pid, so a leftover encoder or helper is visible."""
    if os.name == "nt":
        return set()
    table = subprocess.run(
        ["ps", "-A", "-o", "pid=,ppid="], capture_output=True, text=True, check=True
    ).stdout
    children: dict[int, list[int]] = {}
    for line in table.splitlines():
        child, parent = (int(field) for field in line.split())
        children.setdefault(parent, []).append(child)
    found: set[int] = set()
    pending = [pid]
    while pending:
        for child in children.get(pending.pop(), []):
            found.add(child)
            pending.append(child)
    return found


def _fault_worker(case: NativeAcceptance, name: str, *arguments: str, media: bool = False) -> Path:
    """Replace the case's worker with one started with test flags on a fresh workspace."""
    case.worker.close()
    case.workspace = case.directory / f"{name}-workspace"
    case.workspace.mkdir()
    case.worker = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / f"{name}.stderr.log",
        media_tools=case.media_tools if media else None,
        test_arguments=arguments,
    )
    case.addCleanup(case.worker.close)
    return case.workspace


def _scene(case: NativeAcceptance, frames: int = 8) -> str:
    document = case.create("Failure injection")
    case.mutate("fx.create", document, type="colorCardFx", frame_count=frames)
    return document


def _renders(case: NativeAcceptance) -> list[Path]:
    root = case.workspace / "renders"
    return sorted(root.iterdir()) if root.is_dir() else []


def _settle_processes(case: NativeAcceptance, baseline: set[int]) -> None:
    deadline = time.monotonic() + 10
    while _descendants(case.worker.process.pid) != baseline and time.monotonic() < deadline:
        time.sleep(0.1)
    case.assertEqual(_descendants(case.worker.process.pid), baseline, "leftover process")


def _assert_nothing_published(case: NativeAcceptance, job: str) -> None:
    error = _result(case, job)["error"]
    case.assertEqual(error["code"], "artifacts_unavailable", error)
    case.assertEqual(error["details"]["status"], _status(case, job)["status"])
    case.assertFalse((case.workspace / "renders" / job).exists())
    case.assertEqual([path.name for path in _renders(case)], [])
    case.assertEqual(list(case.workspace.rglob("frame*.png")), [])


def _ends(case: NativeAcceptance, job: str, status: str, text: str = "") -> dict[str, Any]:
    ended = case.wait_job(job)
    case.assertEqual(ended["status"], status, ended)
    case.assertIs(ended["writers_closed"], True)
    case.assertIn(text, ended["error"])
    return ended


def render_cancel_leaves_no_output_and_no_process(case: NativeAcceptance) -> None:
    """Cancel a running sequence: it ends canceled with nothing on disk and no leftover process."""
    document = _scene(case, 120)
    revision = case.revisions[document]
    baseline = _descendants(case.worker.process.pid)
    job = case.mutate(
        "render.submit", document, start_frame=0, end_frame=119, width=256, height=256
    )
    _cancel(case, job["job_id"])
    ended = _ends(case, job["job_id"], "canceled")
    case.assertLess(ended["completed_frames"], ended["frame_count"])
    _assert_nothing_published(case, job["job_id"])
    _settle_processes(case, baseline)
    case.assertEqual(case.revisions[document], revision)
    preview = case.preview(document)
    case.assertTrue(preview.startswith(b"\x89PNG"))
    done = case.wait_job(case.mutate("render.preview", document, frame=0)["job_id"])
    case.assertEqual(done["status"], "completed")
    again = _cancel(case, done["job_id"])
    case.assertEqual(again["status"], "completed")
    case.assertEqual(
        case.success("render.artifacts", {"job_id": done["job_id"]})["status"], "completed"
    )


def _held(case: NativeAcceptance, document: str, status: str, closed: bool = False) -> str:
    """Submit under a hold hook and require the job to stay in one state until canceled."""
    job = case.mutate("render.submit", document, **SEQUENCE)["job_id"]
    time.sleep(0.4)
    held = _status(case, job)
    case.assertEqual(held["status"], status, held)
    case.assertIs(held["writers_closed"], closed)
    return job


def render_cancel_before_launch_leaves_nothing(case: NativeAcceptance) -> None:
    """Cancel a job held before its first frame is built."""
    _fault_worker(case, "launch", "--test-render-fault", "hold-launch")
    document = _scene(case)
    job = _held(case, document, "preparing")
    case.assertEqual(_status(case, job)["completed_frames"], 0)
    _cancel(case, job)
    _ends(case, job, "canceled")
    _assert_nothing_published(case, job)


def render_cancel_racing_dispatch_leaves_nothing(case: NativeAcceptance) -> None:
    """Cancel a job whose frames are built but whose native renderer has not started."""
    _fault_worker(case, "dispatch", "--test-render-fault", "hold-dispatch")
    document = _scene(case)
    job = _held(case, document, "preparing")
    case.assertEqual(list((case.workspace / "renders" / job / "frames").iterdir()), [])
    _cancel(case, job)
    _ends(case, job, "canceled")
    _assert_nothing_published(case, job)


def render_cancel_racing_publication_leaves_nothing(case: NativeAcceptance) -> None:
    """Cancel after every frame is written but before the output is verified and published."""
    _fault_worker(case, "publication", "--test-render-fault", "hold-publication")
    document = _scene(case)
    job = _held(case, document, "finalizing", closed=True)
    frames = sorted((case.workspace / "renders" / job / "frames").glob("*.png"))
    case.assertEqual(len(frames), SEQUENCE["end_frame"] + 1)
    case.assertEqual(
        case.failure("render.artifacts", {"job_id": job})["code"], "artifacts_unavailable"
    )
    _cancel(case, job)
    _ends(case, job, "canceled")
    _assert_nothing_published(case, job)


def render_failure_after_final_frame_publishes_nothing(case: NativeAcceptance) -> None:
    """Fail right after the final-frame callback: failed, explained, and nothing left."""
    _fault_worker(case, "final", "--test-render-fault", "fail-after-final-frame")
    document = _scene(case)
    job = case.mutate("render.submit", document, **SEQUENCE)["job_id"]
    ended = _ends(case, job, "failed", "final frame")
    case.assertEqual(ended["completed_frames"], SEQUENCE["end_frame"] + 1)
    _assert_nothing_published(case, job)
    case.assertEqual(case.success("automation.capabilities", {})["protocol_version"], 3)


def render_without_worker_gl_marks_graphics_failed(case: NativeAcceptance) -> None:
    """The helper's GL probe passes but the worker's render context fails: the job fails,
    publishes nothing, the cached probe is marked failed and later renders are refused."""
    _fault_worker(case, "gl", "--test-render-fault", "gl-context-failure")
    document = _scene(case)
    job = case.mutate("render.submit", document, **SEQUENCE)["job_id"]
    _ends(case, job, "failed", "OpenGL is not available")
    _assert_nothing_published(case, job)
    graphics = case.success("runtime.inspect", {})["graphics"]
    case.assertEqual(graphics["probe"]["result"], "context_ready", graphics)
    case.assertEqual(graphics["state"]["verified"], "failed", graphics)
    case.assertIs(graphics["state"]["usable"], False)
    case.assertIn("render_failed_without_gl", graphics["state"]["reasons"])
    case.assertEqual(case.success("render.capabilities", {})["profiles"], [])
    refused = case.failure("render.preview", case.write_params(document, {"frame": 0}))
    case.assertEqual(refused["code"], "render_unavailable")
    case.assertEqual(refused["details"]["cause"], "gl_unavailable")
    case.assertEqual(_renders(case), [])


def render_encoder_failure_publishes_nothing(case: NativeAcceptance) -> None:
    """Fail the encoder at finalization of an MP4 job: no movie, no partial file, no process."""
    if case.media_tools is None:
        case.skipTest("Needs --media-tools with the real ffmpeg binaries")
    _fault_worker(case, "encoder", "--test-render-fault", "encoder-failure", media=True)
    document = _scene(case)
    baseline = _descendants(case.worker.process.pid)
    movie = {"format": "mp4", **SEQUENCE}
    job = case.mutate("render.submit", document, **movie)["job_id"]
    _ends(case, job, "failed", "encoder")
    _assert_nothing_published(case, job)
    case.assertEqual(list(case.workspace.rglob("*.mp4")), [])
    _settle_processes(case, baseline)


def render_stray_callbacks_do_not_change_the_outcome(case: NativeAcceptance) -> None:
    """Ignore out-of-order, unrequested and post-terminal callbacks."""
    _fault_worker(case, "stray", "--test-render-fault", "stray-callbacks")
    document = _scene(case)
    request = {"start_frame": 0, "end_frame": 6, "step": 2, "width": 32, "height": 32}
    job = case.mutate("render.submit", document, **request)["job_id"]
    ended = _ends(case, job, "completed")
    case.assertEqual((ended["completed_frames"], ended["failed_frames"]), (4, 0))
    result = case.success("render.artifacts", {"job_id": job})
    case.assertEqual([item["frame"] for item in result["artifacts"]], [0, 2, 4, 6])
    time.sleep(0.5)
    case.assertEqual(_status(case, job), ended)
    case.assertEqual(case.success("render.artifacts", {"job_id": job}), result)
    for artifact in result["artifacts"]:
        case.assertEqual(
            case.success("artifact.read", {"artifact_id": artifact["artifact_id"]})["frame"],
            artifact["frame"],
        )


def render_worker_killed_mid_preview_then_new_worker(case: NativeAcceptance) -> None:
    """Kill a worker holding written frames; its successor starts clean and adopts nothing."""
    _fault_worker(case, "killed", "--test-render-fault", "hold-publication")
    document = _scene(case)
    job = _held(case, document, "finalizing", closed=True)
    left = case.workspace / "renders" / job
    case.assertTrue(any((left / "frames").glob("*.png")))
    case.worker.process.kill()
    case.worker.process.wait(timeout=10)
    case.worker = Worker(
        case.executable, case.workspace, case.inputs, case.directory / "successor.stderr.log"
    )
    case.addCleanup(case.worker.close)
    case.assertEqual(case.success("automation.list_documents", {})["documents"], [])
    case.assertFalse(left.exists())
    case.assertEqual(case.failure("render.status", {"job_id": job})["code"], "unknown_job")
    case.assertEqual(case.failure("render.artifacts", {"job_id": job})["code"], "unknown_job")
    case.assertEqual(list(case.workspace.rglob("frame*.png")), [])
    fresh = _scene(case)
    png = case.preview(fresh)
    case.assertTrue(png.startswith(b"\x89PNG"))
    case.assertEqual(len(_renders(case)), 1)


def render_storage_budget_refuses_before_creating_anything(case: NativeAcceptance) -> None:
    """Refuse the job that would exceed the worker's output budget, leaving the others intact."""
    document = _scene(case)
    big = {"start_frame": 0, "end_frame": 0, "width": 2048, "height": 2048}
    kept = []
    for _ in range(7):
        job = case.mutate("render.submit", document, **big)["job_id"]
        case.assertEqual(case.wait_job(job)["status"], "completed")
        kept.append(job)
    before = [path.name for path in _renders(case)]
    error = case.failure("render.submit", case.write_params(document, big))
    case.assertEqual(error["code"], "render_limit", error)
    case.assertIn("storage", error["message"])
    case.assertEqual([path.name for path in _renders(case)], before)
    for job in kept:
        case.assertEqual(case.success("render.artifacts", {"job_id": job})["status"], "completed")


def render_unwritable_output_directory_is_refused_cleanly(case: NativeAcceptance) -> None:
    """Refuse a job whose private output directory cannot be created, leaving nothing behind."""
    if os.name == "nt":
        case.skipTest("POSIX permission bits do not deny writes on Windows")
    document = _scene(case)
    renders = case.workspace / "renders"
    renders.mkdir(exist_ok=True)
    case.addCleanup(renders.chmod, 0o755)
    renders.chmod(0o555)
    if os.access(renders, os.W_OK):
        case.skipTest("The test user can write without permission")
    error = case.failure("render.submit", case.write_params(document, SEQUENCE))
    case.assertEqual(error["code"], "render_output_failed", error)
    case.assertEqual(list(renders.iterdir()), [])
    renders.chmod(0o755)
    job = case.mutate("render.submit", document, **SEQUENCE)["job_id"]
    case.assertEqual(case.wait_job(job)["status"], "completed")


def _free_bytes(path: Path) -> int:
    status = os.statvfs(path)
    return status.f_bavail * status.f_frsize


def _fill_disk(directory: Path, leave: int) -> Path:
    filler = directory / "filler.bin"
    block = bytes(65536)
    with filler.open("wb") as handle:
        try:
            while True:
                handle.write(block)
                handle.flush()
        except OSError:
            pass
    with filler.open("r+b") as handle:
        handle.truncate(max(0, filler.stat().st_size - leave))
    return filler


def _submit_on_full_disk(case: NativeAcceptance, document: str, workspace: Path) -> str:
    """Submit with the disk nearly full and require a typed, fully cleaned-up outcome."""
    before = {path.name for path in _renders(case)}
    frames = set(workspace.rglob("frame*.png"))
    request = case.write_params(
        document, {"start_frame": 0, "end_frame": 23, "width": 384, "height": 384}
    )
    response = case.worker.call("render.submit", request)
    if "error" in response:
        case.assertIn(
            response["error"]["code"],
            {
                "render_output_failed",
                "io_error",
                "native_error",
                "invalid_package",
                "package_write_failed",
            },
        )
        case.assertEqual({path.name for path in _renders(case)}, before)
        case.assertEqual(set(workspace.rglob("frame*.png")), frames)
        return "refused"
    job = response["result"]["job_id"]
    ended = case.wait_job(job)
    if ended["status"] == "completed":
        case.assertEqual(case.success("render.artifacts", {"job_id": job})["status"], "completed")
        return "completed"
    case.assertEqual(ended["status"], "failed", ended)
    case.assertTrue(ended["error"])
    case.assertEqual(
        case.failure("render.artifacts", {"job_id": job})["code"], "artifacts_unavailable"
    )
    case.assertFalse((workspace / "renders" / job).exists())
    case.assertEqual(set(workspace.rglob("frame*.png")), frames)
    return "failed"


def render_disk_full_fails_cleanly_and_recovers(case: NativeAcceptance) -> None:
    """Run out of disk during a render: a typed failure, no partial output, then recovery."""
    if sys.platform != "darwin":
        case.skipTest("Builds its small disk with hdiutil, which only macOS provides")
    image = case.directory / "small.dmg"
    mount = case.directory / "small-volume"
    mount.mkdir()
    subprocess.run(
        ["hdiutil", "create", "-size", "24m", "-fs", "APFS", "-volname", "FAILINJ", str(image)],
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
    workspace = mount / "workspace"
    workspace.mkdir()
    case.worker.close()
    case.workspace = workspace
    case.worker = Worker(
        case.executable, workspace, case.inputs, case.directory / "disk-worker.stderr.log"
    )
    case.addCleanup(case.worker.close)
    document = _scene(case, 24)
    outcomes = []
    for leave in (2048 * 1024, 1024 * 1024, 512 * 1024, 256 * 1024, 128 * 1024, 64 * 1024):
        filler = _fill_disk(mount, leave)
        outcomes.append(_submit_on_full_disk(case, document, workspace))
        filler.unlink()
    case.assertNotEqual(set(outcomes), {"completed"})
    case.assertGreater(_free_bytes(mount), 1024 * 1024)
    again = case.mutate("render.submit", document, **SEQUENCE)["job_id"]
    case.assertEqual(case.wait_job(again)["status"], "completed")


def _staging(case: NativeAcceptance) -> list[Path]:
    return [*case.workspace.rglob(".mcp-package-*"), *case.workspace.rglob(".mcp-audio-*")]


def package_write_refusals_leave_nothing_behind(case: NativeAcceptance) -> None:
    """Refuse an existing or unwritable package destination without touching or leaving files."""
    document = _scene(case)
    revision = case.revisions[document]
    taken = case.workspace / "taken"
    taken.mkdir()
    (taken / "keep.txt").write_text("keep")
    error = case.failure("scene.save_copy", case.write_params(document, {"directory": str(taken)}))
    case.assertEqual(error["code"], "already_exists", error)
    case.assertEqual([path.name for path in taken.iterdir()], ["keep.txt"])
    if os.name != "nt":
        locked = case.workspace / "locked"
        locked.mkdir()
        case.addCleanup(locked.chmod, 0o755)
        locked.chmod(0o555)
        if not os.access(locked, os.W_OK):
            target = locked / "package"
            error = case.failure(
                "scene.save_copy", case.write_params(document, {"directory": str(target)})
            )
            case.assertEqual(error["code"], "package_write_failed", error)
            case.assertEqual(list(locked.iterdir()), [])
    case.assertEqual(_staging(case), [])
    case.assertEqual(case.revisions[document], revision)
    case.assertEqual(case.success("scene.summary", {"document_id": document})["revision"], revision)


def package_publish_failure_reports_what_is_on_disk(case: NativeAcceptance) -> None:
    """A save that fails at its final rename never claims a rollback and leaves no files."""
    _fault_worker(case, "publish", "--test-write-fault", "publish")
    document = _scene(case)
    revision = case.revisions[document]
    target = case.workspace / "package"
    request = case.write_params(document, {"directory": str(target)})
    error = case.failure("scene.save_copy", request)
    case.assertIn(error["code"], {"package_write_failed", "partially_applied"}, error)
    text = (error["message"] + str(error["details"])).lower()
    case.assertNotIn("roll", text)
    case.assertFalse(target.exists())
    case.assertEqual(_staging(case), [])
    case.assertEqual(case.failure("scene.save_copy", request), error)
    case.assertEqual(case.success("scene.summary", {"document_id": document})["revision"], revision)


def mixdown_publish_failure_reports_what_is_on_disk(case: NativeAcceptance) -> None:
    """A mixdown that fails at its final rename never claims a rollback and leaves no file."""
    _fault_worker(case, "mixdown", "--test-write-fault", "publish")
    document = _audio_scene(case, 24)
    source = tone(48000, "pcm_s16", 48000 * 2, channels=2)
    level = _import(case, document, "Voice", wav_file(source))["level_id"]
    _place(case, document, _column(case, document), level, Clip(source, 0, 0, 10))
    output = case.workspace / "mix.wav"
    params = {"start_frame": 0, "end_frame": 5, "output_path": str(output)}
    request = case.write_params(document, {**params, "format_policy": "native_mix"})
    error = case.failure("audio.mixdown", request)
    case.assertIn(error["code"], {"io_error", "partially_applied"}, error)
    case.assertNotIn("roll", (error["message"] + str(error["details"])).lower())
    case.assertFalse(output.exists())
    case.assertEqual(_staging(case), [])
    case.assertEqual(case.failure("audio.mixdown", request), error)


def _read_only_render(case: NativeAcceptance, worker: Worker, scene: Path) -> tuple[str, str]:
    show = project_id(case, worker, "ShowA")
    document = open_scene(case, worker, scene, show)["document_id"]
    params = {"document_id": document, "expected_revision": "0", "start_frame": 0}
    params.update(end_frame=49, width=512, height=512, operation_id=_op(case))
    return document, call(case, worker, "render.submit", params)["job_id"]


def _nothing_on_disk(case: NativeAcceptance) -> None:
    case.assertEqual(list(case.workspace.rglob("renders/job_*")), [])
    case.assertEqual(list(case.workspace.rglob("frame*.png")), [])


def read_only_render_cancel_and_host_loss_leave_nothing(case: NativeAcceptance) -> None:
    """A scene host's canceled or lost render ends terminal, leaves no file or process."""
    scene = corpus(case)["A"]
    worker = case.worker
    document, job = _read_only_render(case, worker, scene)
    call(case, worker, "render.cancel", {"job_id": job, "operation_id": _op(case)})
    case.assertEqual(wait(case, worker, job)["status"], "canceled")
    refuse(case, worker, "render.artifacts", {"job_id": job}, "artifacts_unavailable")
    _nothing_on_disk(case)
    params = {"document_id": document, "expected_revision": "0", "start_frame": 0}
    params.update(end_frame=49, width=512, height=512, operation_id=_op(case))
    lost = call(case, worker, "render.submit", params)["job_id"]
    pid = next(pid for pid, owner in host_processes(worker) if owner == document)
    os.kill(pid, signal.SIGKILL)
    details = refuse(case, worker, "render.status", {"job_id": lost}, "scene_host_failed")
    case.assertEqual(details["status"], "failed")
    refuse(case, worker, "render.artifacts", {"job_id": lost}, "scene_host_failed")
    _nothing_on_disk(case)
    case.assertEqual(list(case.workspace.glob("runtime_*/scene-host-*")), [])
    discard = {"document_id": document, "expected_revision": "0", "operation_id": _op(case)}
    case.assertTrue(call(case, worker, "scene.discard", discard)["discarded"])
    case.assertEqual(host_processes(worker), [])


def _directories(case: NativeAcceptance) -> set[Path]:
    runtimes = set(case.workspace.glob("runtime_*"))
    return runtimes | set(case.workspace.rglob("renders/job_*"))


def _second_worker(case: NativeAcceptance, name: str) -> Worker:
    worker = Worker(
        case.executable, case.workspace, case.inputs, case.directory / f"{name}.stderr.log"
    )
    case.addCleanup(worker.close)
    return worker


def live_worker_and_host_files_survive_another_workers_start(case: NativeAcceptance) -> None:
    """A second worker on the same workspace leaves a live worker's and host's files alone."""
    scene = corpus(case)["A"]
    document = _scene(case, 120)
    editable = case.mutate(
        "render.submit", document, start_frame=0, end_frame=119, width=256, height=256
    )
    before = _directories(case)
    case.assertTrue(any(path.name.startswith("job_") for path in before))
    other = _second_worker(case, "bystander")
    case.assertEqual(other.call("automation.capabilities", {})["result"]["protocol_version"], 3)
    case.assertLessEqual(before, _directories(case))
    ended = case.wait_job(editable["job_id"])
    case.assertEqual(ended["status"], "completed", ended)
    other.close()
    show = project_id(case, case.worker, "ShowA")
    opened = open_scene(case, case.worker, scene, show)["document_id"]
    params = {"document_id": opened, "expected_revision": "0", "start_frame": 0}
    params.update(end_frame=119, width=512, height=512, operation_id=_op(case))
    hosted = call(case, case.worker, "render.submit", params)["job_id"]
    hosts = list(case.workspace.glob("runtime_*/scene-host-*"))
    case.assertEqual(len(hosts), 1)
    again = _second_worker(case, "second-bystander")
    case.assertEqual(again.call("automation.capabilities", {})["result"]["protocol_version"], 3)
    case.assertTrue(hosts[0].is_dir())
    case.assertEqual(wait(case, case.worker, hosted)["status"], "completed")
    artifacts = call(case, case.worker, "render.artifacts", {"job_id": hosted})["artifacts"]
    image = call(case, case.worker, "artifact.read", {"artifact_id": artifacts[0]["artifact_id"]})
    case.assertEqual(image["frame"], 0)


def _native_pids(path: Path) -> set[int]:
    listing = subprocess.run(
        ["ps", "-ax", "-o", "pid=,command="], capture_output=True, text=True, check=True
    ).stdout
    found = set()
    for line in listing.splitlines():
        pid, _, command = line.strip().partition(" ")
        if f"--workspace {path}" in command and int(pid) != os.getpid():
            found.add(int(pid))
    return found


def _wait_dead(case: NativeAcceptance, pids: set[int]) -> None:
    deadline = time.monotonic() + 30
    while pids & _native_pids(case.workspace) and time.monotonic() < deadline:
        time.sleep(0.1)
    case.assertEqual(pids & _native_pids(case.workspace), set())


def dead_workers_directories_are_swept_and_live_ones_are_not(case: NativeAcceptance) -> None:
    """Sweep runtime and job directories whose owner is gone, even if its pid is reused."""
    if os.name == "nt":
        case.skipTest("The fixture holds a POSIX advisory lock")
    import fcntl

    scene = corpus(case)["A"]
    mine = set(case.workspace.glob("runtime_*"))
    alive = _native_pids(case.workspace)
    victim = _second_worker(case, "victim")
    show = project_id(case, victim, "ShowA")
    opened = open_scene(case, victim, scene, show)["document_id"]
    params = {"document_id": opened, "expected_revision": "0", "start_frame": 0}
    params.update(end_frame=119, width=512, height=512, operation_id=_op(case))
    call(case, victim, "render.submit", params)
    victims = set(case.workspace.glob("runtime_*")) - mine
    case.assertEqual(len(victims), 1)
    case.assertTrue(any(case.workspace.glob("runtime_*/scene-host-*")))
    case.assertTrue(any(case.workspace.rglob("renders/job_*")))
    held = case.workspace / "runtime_fixture-held"
    reused = case.workspace / "runtime_fixture-reused"
    plain = case.workspace / "runtime_fixture-unmarked"
    for directory in (held, reused, plain):
        directory.mkdir()
    (held / ".owner.lock").write_text("held")
    (reused / ".owner.lock").write_text(str(os.getpid()))
    holder = (held / ".owner.lock").open("r+")
    case.addCleanup(holder.close)
    fcntl.flock(holder, fcntl.LOCK_EX | fcntl.LOCK_NB)
    bystander = _second_worker(case, "sweeper")
    case.assertEqual(bystander.call("automation.list_documents", {})["result"]["documents"], [])
    case.assertTrue(victims <= set(case.workspace.glob("runtime_*")))
    case.assertTrue(mine <= set(case.workspace.glob("runtime_*")))
    case.assertTrue(held.is_dir() and plain.is_dir())
    case.assertFalse(reused.exists())
    bystander.close()
    victim.process.kill()
    victim.process.wait(timeout=10)
    _wait_dead(case, _native_pids(case.workspace) - alive)
    successor = _second_worker(case, "successor")
    case.assertEqual(successor.call("automation.list_documents", {})["result"]["documents"], [])
    case.assertEqual(victims & set(case.workspace.glob("runtime_*")), set())
    case.assertEqual(list(case.workspace.glob("runtime_*/scene-host-*")), [])
    case.assertEqual(list(case.workspace.rglob("renders/job_*")), [])
    case.assertTrue(mine <= set(case.workspace.glob("runtime_*")))
    case.assertTrue(held.is_dir() and plain.is_dir())


def private_files_end_with_their_document_and_worker(case: NativeAcceptance) -> None:
    """Worker-private level, sound and project files are removed with their document or worker."""
    document = _scene(case)
    source = tone(24000, "pcm_s16", 24000)
    level = _import(case, document, "Voice", wav_file(source))["level_id"]
    _place(case, document, _column(case, document), level, Clip(source, 0, 0, 10))
    case.assertTrue(list(case.workspace.rglob("sounds/*")))
    case.mutate("scene.discard", document)
    case.assertEqual(list(case.workspace.rglob("sounds/*")), [])
    packaged = _scene(case)
    voice = _import(case, packaged, "Voice", wav_file(source))["level_id"]
    _place(case, packaged, _column(case, packaged), voice, Clip(source, 0, 0, 10))
    saved = case.mutate("scene.save_copy", packaged, directory=str(case.workspace / "residue"))
    case.mutate("scene.discard", packaged)
    reopened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    case.assertTrue(list(case.workspace.rglob("sounds/*")))
    case.mutate("scene.discard", reopened)
    case.assertEqual(list(case.workspace.rglob("sounds/*")), [])
    case.assertEqual(list(case.workspace.glob("project_*")), [])
    case.assertEqual(list(case.workspace.glob("sounds")) + list(case.workspace.glob("levels")), [])
