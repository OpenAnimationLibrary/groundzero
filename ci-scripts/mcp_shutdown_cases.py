"""Qualify the worker's exit once its input ends: bounded, in every state, with cleanup done."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

import mcp_analysis_cases
import mcp_failure_cases
from mcp_scene_access_cases import (
    DEADLINE,
    DEADLINE_EXIT,
    SLACK,
    _alive,
    close_and_time,
    corpus,
    fault_worker,
    host_processes,
)
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


def _openmp_registration(pid: int) -> Path:
    """The file the OpenMP runtime registers each process in, outside the workspace."""
    return Path(f"/tmp/__KMP_REGISTERED_LIB_{pid}")


def _held(marker: Path) -> bool:
    """Whether a process holds the claim marker's POSIX advisory lock."""
    import fcntl

    with marker.open("rb") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return True
        fcntl.flock(handle, fcntl.LOCK_UN)
        return False


def _claims(workspace: Path) -> list[Path]:
    """Claim markers outside the private runtime directories, which keep theirs by design."""
    return [
        marker
        for marker in workspace.rglob(".owner.lock")
        if not any(part.startswith("runtime_") for part in marker.relative_to(workspace).parts)
    ]


def end_of_input_during_a_render_cleans_up(case: NativeAcceptance) -> None:
    """Input ending mid-render cancels it, removes its output and exits in time; a finished
    job keeps its output and a claim marker no process holds, for a later worker to sweep."""
    document = mcp_failure_cases._scene(case, 120)
    done = case.wait_job(case.mutate("render.preview", document, frame=0)["job_id"])
    case.assertEqual(done["status"], "completed")
    running = case.mutate(
        "render.submit", document, start_frame=0, end_frame=119, width=256, height=256
    )["job_id"]
    elapsed, code = close_and_time(case, case.worker)
    case.assertEqual(code, 0)
    case.assertLess(elapsed, DEADLINE + SLACK)
    case.assertFalse(_openmp_registration(case.worker.process.pid).exists())
    renders = case.workspace / "renders"
    case.assertEqual(sorted(path.name for path in renders.iterdir()), [done["job_id"]])
    case.assertFalse((renders / running).exists())
    marker = renders / done["job_id"] / ".owner.lock"
    case.assertEqual(_claims(case.workspace), [marker])
    if os.name != "nt":
        case.assertFalse(_held(marker), "the exited worker's claim is still held")


def end_of_input_during_a_request_is_bounded(case: NativeAcceptance) -> None:
    """A request still running when input ends gets what remains of the deadline; the worker
    then ends itself and its scene host ends with it."""
    scenes = corpus(case)
    worker = fault_worker(case, "hang", "request")
    assert worker.process.stdin is not None
    params = {"path": str(scenes["B"]), "project_id": "standalone", "operation_id": "hang"}
    request = {"id": "hang", "method": "scene.open_read_only", "params": params}
    worker.process.stdin.write(json.dumps(request).encode() + b"\n")
    worker.process.stdin.flush()
    deadline = time.monotonic() + 20
    while not host_processes(worker) and time.monotonic() < deadline:
        time.sleep(0.05)
    hosts = host_processes(worker)
    case.assertEqual(len(hosts), 1)
    elapsed, code = close_and_time(case, worker)
    case.assertEqual(code, DEADLINE_EXIT)
    case.assertLess(elapsed, DEADLINE + SLACK)
    deadline = time.monotonic() + 5
    while _alive(hosts[0][0]) and time.monotonic() < deadline:
        time.sleep(0.05)
    case.assertFalse(_alive(hosts[0][0]), "The scene host outlived its worker")


def shutdown_deadline_ends_a_stuck_teardown(case: NativeAcceptance) -> None:
    """A teardown that never finishes ends at the deadline with its own exit code, and the
    next worker in the workspace sweeps what it left."""
    workspace = case.directory / "stuck-workspace"
    workspace.mkdir()
    stuck = Worker(
        case.executable,
        workspace,
        case.inputs,
        case.directory / "stuck.stderr.log",
        test_arguments=("--test-render-fault", "teardown-hang"),
    )
    case.addCleanup(stuck.close)
    stuck.call("automation.capabilities", {})
    elapsed, code = close_and_time(case, stuck)
    case.assertEqual(code, DEADLINE_EXIT)
    case.assertFalse(_openmp_registration(stuck.process.pid).exists())
    case.assertLess(elapsed, DEADLINE + SLACK)
    case.assertIn("shutdown deadline passed", stuck.log_path.read_text(errors="replace"))
    case.assertEqual(len(list(workspace.glob("runtime_*"))), 1)
    successor = Worker(
        case.executable, workspace, case.inputs, case.directory / "successor.stderr.log"
    )
    case.addCleanup(successor.close)
    successor.call("automation.capabilities", {})
    case.assertEqual(len(list(workspace.glob("runtime_*"))), 1, "The abandoned runtime stayed")


APPLICATIONS = """
ObjC.import('AppKit');
var workspace = $.NSWorkspace.sharedWorkspace, rows = [];
var apps = workspace.runningApplications;
for (var i = 0; i < apps.count; i++) {
  var app = apps.objectAtIndex(i);
  rows.push(app.processIdentifier + " " + app.activationPolicy);
}
rows.push("front " + workspace.frontmostApplication.processIdentifier);
rows.join("\\n");
"""


def _applications() -> tuple[dict[int, int], int]:
    """Running applications' activation policies by process id, and the frontmost one."""
    listing = subprocess.run(
        ["osascript", "-l", "JavaScript", "-e", APPLICATIONS],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    pairs = dict(zip(listing[0::2], listing[1::2], strict=True))
    front = int(pairs.pop("front"))
    return {int(pid): int(policy) for pid, policy in pairs.items()}, front


def _helper(worker: Worker) -> int:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        listing = subprocess.run(
            ["ps", "-A", "-o", "pid=,ppid=,command="], capture_output=True, text=True, check=True
        ).stdout
        for line in listing.splitlines():
            pid, parent, command = line.split(None, 2)
            if int(parent) == worker.process.pid and "--parse-helper gl_probe" in command:
                return int(pid)
        time.sleep(0.05)
    raise AssertionError("The graphics probe helper did not start")


def automation_processes_stay_in_the_background(case: NativeAcceptance) -> None:
    """Workers, scene hosts and helpers are no regular applications and never take focus,
    and a worker still renders."""
    if sys.platform != "darwin":
        case.skipTest("activation policies are a macOS property")
    _policies, front = _applications()
    scenes = corpus(case)
    opened = case.success(
        "scene.open_read_only",
        {"path": str(scenes["B"]), "project_id": "standalone", "operation_id": "background"},
    )
    hosts = host_processes(case.worker)
    case.assertEqual(len(hosts), 1)
    prober = fault_worker(case, "hang", "background")
    assert prober.process.stdin is not None
    request = {"id": "probe", "method": "runtime.inspect", "params": {"probes": ["graphics"]}}
    prober.process.stdin.write(json.dumps(request).encode() + b"\n")
    prober.process.stdin.flush()
    helper = _helper(prober)
    policies, now_front = _applications()
    for name, pid in (
        ("worker", case.worker.process.pid),
        ("scene host", hosts[0][0]),
        ("probe worker", prober.process.pid),
        ("probe helper", helper),
    ):
        case.assertNotEqual(policies.get(pid, 2), 0, f"The {name} is a regular application")
    case.assertEqual(now_front, front, "An automation process took focus")
    case.assertTrue(case.preview(case.create("Background")).startswith(b"\x89PNG"))
    params = {"document_id": opened["document_id"], "expected_revision": "0"}
    case.success("scene.discard", {**params, "operation_id": "background-discard"})
    _elapsed, code = close_and_time(case, prober)
    case.assertEqual(code, DEADLINE_EXIT, "input ended while the probe helper hung")


def _graphics(case: NativeAcceptance, worker: Worker, *probes: str) -> dict[str, Any]:
    params = {"probes": list(probes)} if probes else {}
    return worker.call("runtime.inspect", params)["result"]["graphics"]


def _preflight(case: NativeAcceptance, worker: Worker) -> dict[str, Any]:
    created = worker.call(
        "scene.create",
        {
            "operation_id": "probe-scene",
            "name": "Probe",
            "width_px": 32,
            "height_px": 32,
            "frame_rate": 24,
        },
    )["result"]
    params = {"document_id": created["document_id"], "expected_revision": created["revision"]}
    return worker.call("render.preflight", {**params, "kind": "preview", "frame": 0})["result"]


def graphics_probe_timeout_is_not_a_verdict(case: NativeAcceptance) -> None:
    """A probe that does not finish in time leaves graphics unknown and is tried again; a
    probe that fails is the verdict and is kept."""
    hung = fault_worker(case, "hang", "graphics-hang")
    graphics = _graphics(case, hung, "graphics")
    case.assertEqual(graphics["probe"]["result"], "timeout", graphics)
    case.assertEqual(graphics["state"]["verified"], "not_run", graphics)
    case.assertIn("probe_timeout", graphics["state"]["reasons"])
    report = _preflight(case, hung)
    check = next(check for check in report["checks"] if check["id"] == "graphics")
    case.assertEqual(report["probes_run"], ["graphics"], report)
    case.assertEqual(check["details"]["probe_result"], "timeout", check)
    crashed = fault_worker(case, "crash", "graphics-crash")
    graphics = _graphics(case, crashed, "graphics")
    case.assertEqual(graphics["probe"]["result"], "crashed", graphics)
    case.assertEqual(graphics["state"]["verified"], "failed", graphics)
    case.assertEqual(_preflight(case, crashed)["probes_run"], [], "A failed probe was retried")


def end_of_input_during_an_analysis_job_is_bounded(case: NativeAcceptance) -> None:
    """Input ending while an analysis job runs in application-thread slices still exits in
    time and cleanly; meanwhile polls answered off that thread see the job's progress."""
    document, level = mcp_analysis_cases._heavy_level(case, drawings=200)
    job = mcp_analysis_cases._submit(case, document, level, check="regions")["job_id"]
    running = mcp_analysis_cases._wait_running(case, job)
    case.assertEqual(running["lane"], "analysis")
    case.assertFalse(running["terminal"], f"no progress was published before the end: {running}")
    elapsed, code = close_and_time(case, case.worker)
    case.assertEqual(code, 0)
    case.assertLess(elapsed, DEADLINE + SLACK)
    case.assertFalse(_openmp_registration(case.worker.process.pid).exists())
