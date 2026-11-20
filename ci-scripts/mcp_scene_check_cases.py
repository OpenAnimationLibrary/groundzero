"""Acceptance cases for scene.check.submit: unused-style scans and dependency inventories."""

from __future__ import annotations

import os
import shutil
import signal
import subprocess
import time
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_failure_cases import _descendants
from mcp_scene_access_cases import base_package, corpus, open_scene
from mcp_studio_palette_cases import call
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

# The second level has enough drawings that a slow scan (the slow-scan test fault sleeps
# 25 ms after each drawing or cast level) is still running when a test acts on it.
FIRST_DRAWINGS = 2
SECOND_DRAWINGS = 30
TERMINAL = {"completed", "failed", "canceled", "deadline_exceeded"}


def _slow_worker(case: NativeAcceptance) -> None:
    """Replace the case's worker with one whose scene checks run slowly.

    The fault travels in the environment so a worker the adapter starts gets it too.
    """
    case.worker.close()
    os.environ["GROUNDZERO_TEST_CHILD_FAULT"] = "slow-scan"
    try:
        case.worker = Worker(
            case.executable, case.workspace, case.inputs, case.directory / "slow-scan.stderr.log"
        )
        case.worker.call("automation.capabilities", {})
    finally:
        del os.environ["GROUNDZERO_TEST_CHILD_FAULT"]
    case.addCleanup(case.worker.close)


def _stroke(style: int, offset: int) -> dict[str, Any]:
    points = [{"x": offset + x, "y": y, "thickness": 1} for x, y in ((0, 0), (5, 5), (10, 0))]
    return {"action": "add", "style_id": style, "closed": False, "points": points}


def _level(
    case: NativeAcceptance, document: str, name: str, drawings: int, column: int
) -> dict[str, Any]:
    """A vector level whose drawings use style 1 and, on even drawings, its style Used;
    its style Unused is painted by none. Every drawing is exposed in its own column."""
    level = case.mutate("level.create", document, name=name, format="vector")["level_id"]
    red = {"r": 255, "g": 0, "b": 0, "a": 255}
    used = case.mutate("palette.create_style", document, level_id=level, name="Used", color=red)
    unused = case.mutate("palette.create_style", document, level_id=level, name="Unused", color=red)
    track = case.mutate("column.create", document, name=f"Column {column}")["column_id"]
    for number in range(1, drawings + 1):
        target = {"level_id": level, "drawing_id": {"number": number, "suffix": ""}}
        case.mutate("drawing.create", document, **target, format="vector")
        strokes = [_stroke(1, number)]
        if number % 2 == 0:
            strokes.append(_stroke(used["style_id"], number))
        edit = {**target, "coordinate_space": "drawing_native", "sharing": "all_exposures"}
        case.mutate("vector.edit", document, **edit, edits=strokes)
        case.mutate(
            "column.expose", document, column_id=track, **target, start_frame=number - 1, length=1
        )
    return {"level_id": level, "used": used["style_id"], "unused": unused["style_id"]}


def _scene(case: NativeAcceptance) -> tuple[str, list[dict[str, Any]]]:
    document = case.create("Scene check")
    levels = [
        _level(case, document, "First", FIRST_DRAWINGS, 1),
        _level(case, document, "Second", SECOND_DRAWINGS, 2),
    ]
    return document, levels


def _submit(case: NativeAcceptance, document: str, check: str, **extra: Any) -> dict[str, Any]:
    return case.success(
        "scene.check.submit", case.write_params(document, {"check": check, **extra})
    )


def _rows(case: NativeAcceptance, job: str) -> dict[str, Any]:
    return case.success("job.result", {"job_id": job, "limit": 256})


def _unused(result: dict[str, Any]) -> set[tuple[str, int]]:
    return {(row["palette_owner"]["id"], row["style_id"]) for row in result["rows"]}


@acceptance_case(name="scene_check_unused_styles_job")
def unused_styles_job(case: NativeAcceptance) -> None:
    """Report exactly the styles no drawing uses, with estimate and cost, and leave the
    document and its revision unchanged."""
    document, levels = _scene(case)
    revision = case.revisions[document]
    before = case.success("scene.summary", {"document_id": document})
    job = _submit(case, document, "unused_styles")
    case.assertEqual(
        (job["kind"], job["lane"], job["executor"]), ("scene_check", "analysis", "worker")
    )
    case.assertEqual(job["analysis"]["estimate"]["drawings"], FIRST_DRAWINGS + SECOND_DRAWINGS)
    ended = case.wait_job(job["job_id"])
    case.assertEqual(ended["status"], "completed", ended)
    case.assertEqual(ended["analysis"]["cost"]["drawings_done"], FIRST_DRAWINGS + SECOND_DRAWINGS)
    case.assertIs(ended["analysis"]["complete"], True)
    result = _rows(case, job["job_id"])
    case.assertEqual(_unused(result), {(level["level_id"], level["unused"]) for level in levels})
    case.assertEqual((result["complete"], result["reasons"]), (True, []))
    for row in result["rows"]:
        case.assertEqual(row["row_kind"], "unused_style")
        case.assertEqual(row["name"], "Unused")
    case.assertEqual(case.revisions[document], revision)
    case.assertEqual(case.success("scene.summary", {"document_id": document}), before)


def _running(case: NativeAcceptance, job: str) -> None:
    status = case.success("job.status", {"job_id": job})
    case.assertFalse(status["terminal"], status)


@acceptance_case(name="scene_check_cancel_write_and_busy")
def scan_cancel_write_and_busy(case: NativeAcceptance) -> None:
    """A canceled scan ends at once with the rows of the palettes it finished; a second check
    is analysis_busy while a render may still start; a write fails a running scan."""
    _slow_worker(case)
    document, levels = _scene(case)
    job = _submit(case, document, "unused_styles")["job_id"]
    time.sleep(0.2)
    _running(case, job)
    busy = case.failure("scene.check.submit", case.write_params(document, {"check": "inventory"}))
    case.assertEqual((busy["code"], busy["details"]["active_job_id"]), ("analysis_busy", job))
    canceled = case.success("job.cancel", case.write_params(None, {"job_id": job}))
    case.assertEqual((canceled["status"], canceled["terminal"]), ("canceled", True))
    case.assertIs(canceled["cancellation"]["effective"], True)
    result = _rows(case, job)
    case.assertEqual((result["complete"], result["reasons"]), (False, ["canceled"]))
    case.assertEqual(_unused(result), {(levels[0]["level_id"], levels[0]["unused"])})
    written = _submit(case, document, "unused_styles")["job_id"]
    render = case.mutate("render.preview", document, frame=0, width=8, height=8)
    _running(case, written)
    blue = {"r": 0, "g": 0, "b": 255, "a": 255}
    case.mutate(
        "palette.create_style", document, level_id=levels[0]["level_id"], name="Late", color=blue
    )
    case.assertEqual(case.wait_job(render["job_id"])["status"], "completed")
    failed = case.wait_job(written)
    case.assertEqual(failed["status"], "failed", failed)
    case.assertEqual(failed["failure"]["code"], "revision_changed")
    refused = case.failure("job.result", {"job_id": written})
    case.assertEqual(refused["code"], "result_unavailable")


@acceptance_case(name="scene_check_inventory_cancel_keeps_partial_rows")
def inventory_cancel_keeps_partial_rows(case: NativeAcceptance) -> None:
    """A canceled inventory keeps the rows of the levels it reached, marked incomplete."""
    _slow_worker(case)
    document = case.create("Inventory")
    for index in range(24):
        case.mutate("level.create", document, name=f"Level {index}", format="vector")
    job = _submit(case, document, "inventory")["job_id"]
    time.sleep(0.15)
    canceled = case.success("job.cancel", case.write_params(None, {"job_id": job}))
    case.assertEqual(canceled["status"], "canceled", canceled)
    result = _rows(case, job)
    case.assertEqual((result["complete"], result["reasons"]), (False, ["canceled"]))
    case.assertGreater(result["total_rows"], 0)
    case.assertLess(result["total_rows"], 24)
    for row in result["rows"]:
        case.assertEqual((row["row_kind"], row["sheet_path"]), ("dependency", []))
        case.assertEqual(row["item"]["owner_kind"], "level")


@acceptance_case(name="scene_check_retention_refuses_the_33rd_check")
def retention_refuses_the_33rd_check(case: NativeAcceptance) -> None:
    """A worker retains 32 analysis jobs; the 33rd is refused before it starts."""
    document = case.create("Retention")
    for _ in range(32):
        job = _submit(case, document, "inventory")["job_id"]
        case.assertEqual(case.wait_job(job)["status"], "completed")
    refused = case.failure(
        "scene.check.submit", case.write_params(document, {"check": "inventory"})
    )
    case.assertEqual(
        (refused["code"], refused["details"]["limit"]), ("resource_limit", "retained_analysis_jobs")
    )
    listed = case.success("job.list", {"kind": "scene_check", "limit": 64})
    case.assertEqual(listed["total"], 32)


def _host_wait(case: NativeAcceptance, worker: Worker, job: str) -> dict[str, Any]:
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        status = call(case, worker, "job.status", {"job_id": job})
        if status["status"] in TERMINAL:
            return status
        time.sleep(0.05)
    case.fail(f"Scene check {job} did not finish")


@acceptance_case(name="scene_check_read_only_runs_in_the_host")
def read_only_checks_run_in_the_host(case: NativeAcceptance) -> None:
    """Scene checks of a read-only document run in its scene host, bound to its sources;
    inventory rows name the sheet that exposes each level."""
    worker = case.worker
    scenes = corpus(case)
    document = open_scene(case, worker, scenes["B"], "standalone")["document_id"]
    params = {"document_id": document, "expected_revision": "0", "operation_id": "ro-scan"}
    job = call(case, worker, "scene.check.submit", {**params, "check": "unused_styles"})
    case.assertEqual(job["executor"], "scene_host")
    case.assertTrue(job["inputs"]["source_fingerprint"].startswith("source:"))
    ended = _host_wait(case, worker, job["job_id"])
    case.assertEqual(ended["status"], "completed", ended)
    result = call(case, worker, "job.result", {"job_id": job["job_id"], "limit": 256})
    case.assertIs(result["complete"], True, result)
    inventory = call(
        case,
        worker,
        "scene.check.submit",
        {**params, "operation_id": "ro-inventory", "check": "inventory"},
    )
    case.assertEqual(_host_wait(case, worker, inventory["job_id"])["status"], "completed")
    rows = call(case, worker, "job.result", {"job_id": inventory["job_id"], "limit": 256})
    dependencies = case.success(
        "scene.dependencies", {"document_id": document, "expected_revision": "0", "limit": 128}
    )
    case.assertEqual([row["item"] for row in rows["rows"]], dependencies["items"])
    listed = call(case, worker, "job.list", {"document_id": document})
    case.assertEqual(
        {row["job_id"] for row in listed["jobs"]}, {job["job_id"], inventory["job_id"]}
    )


def _native_worker(case: NativeAcceptance) -> int:
    """The native worker process below the MCP client and adapter."""
    below = _descendants(case.worker.process.pid)
    listing = subprocess.run(
        ["ps", "-A", "-o", "pid=,command="], capture_output=True, text=True, check=True
    ).stdout
    for line in listing.splitlines():
        pid, command = line.strip().split(None, 1)
        if int(pid) in below and "--automation-worker" in command and "--scene-host" not in command:
            return int(pid)
    case.fail("The adapter's native worker was not found")


@acceptance_case(name="scene_check_analysis_job_after_worker_loss")
def analysis_job_after_worker_loss(case: NativeAcceptance) -> None:
    """Through the adapter, a running scene check whose worker is lost reads unknown, with
    the loss, from job.status and job.list."""
    if not case.through_sdk:
        case.skipTest("sdk-only: the adapter's job ledger answers after the worker is lost")
    _slow_worker(case)
    document, _levels = _scene(case)
    job = _submit(case, document, "unused_styles")["job_id"]
    time.sleep(0.2)
    _running(case, job)
    os.kill(_native_worker(case), signal.SIGKILL)
    deadline = time.monotonic() + 30
    status: dict[str, Any] = {}
    while time.monotonic() < deadline:
        response = case.worker.call("job.status", {"job_id": job})
        status = response.get("result", {})
        if status.get("status") == "unknown":
            break
        time.sleep(0.2)
    case.assertEqual(status.get("status"), "unknown", status)
    case.assertIn("worker_loss", status)
    case.assertEqual((status["kind"], status["lane"]), ("scene_check", "analysis"))
    listed = case.success("job.list", {})
    case.assertEqual([row["status"] for row in listed["jobs"]], ["unknown"])


def _findings(case: NativeAcceptance, document: str) -> set[tuple[str, str]]:
    job = _submit(case, document, "compatibility")["job_id"]
    case.assertEqual(case.wait_job(job)["status"], "completed")
    result = _rows(case, job)
    case.assertIs(result["complete"], True, result)
    for row in result["rows"]:
        case.assertEqual((row["row_kind"], row["reader"]), ("compatibility", "legacy_opentoonz"))
        expected = "info" if row["certainty"] == "unknown_support" else "warning"
        case.assertEqual(row["severity"], expected, row)
    return {(row["code"], row["certainty"]) for row in result["rows"]}


@acceptance_case(name="scene_check_compatibility_codes_and_certainty")
def compatibility_check_codes_and_certainty(case: NativeAcceptance) -> None:
    """An offset layer is a refusal risk for a legacy reader; the findings survive a save and
    reopen unchanged, and scene.validate reports none of them."""
    document, _levels = _scene(case)
    peg = case.mutate("object.create", document, name="Peg", kind="pegbar")["object_id"]
    case.mutate("offset_layer.create", document, object_id=peg, name="Sway")
    findings = _findings(case, document)
    case.assertIn(("compat.offset_layers", "refusal_risk"), findings)
    case.assertIn(("compat.cast_asset_identities", "confirmed_loss"), findings)
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "compat"))
    reopened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    case.assertEqual(_findings(case, reopened), findings)
    issues = case.success(
        "scene.validate",
        {"document_id": document, "expected_revision": case.revisions[document], "limit": 128},
    )["items"]
    case.assertFalse([item for item in issues if item["code"].startswith("compat.")])


@acceptance_case(name="scene_check_compatibility_marks_unknown_support")
def compatibility_check_marks_unknown_support(case: NativeAcceptance) -> None:
    """An effect only a Groundzero build has is reported as unknown support at info severity,
    never as a confirmed loss or a refusal risk, and the check changes nothing."""
    base = base_package(case)
    effect = (
        "<fxnode> <STD_lut3DBakeFx id='4000000'> <name> Bake </name> </STD_lut3DBakeFx> </fxnode>"
    )
    text = (base / "scene.tnz").read_text().replace("<internal>", "<internal>" + effect, 1)
    path = case.inputs / "unknown-support" / "shot.tnz"
    shutil.copytree(base / "assets", path.parent / "assets")
    path.write_text(text)
    worker = case.worker
    document = open_scene(case, worker, path, "standalone")["document_id"]
    params = {"document_id": document, "expected_revision": "0", "operation_id": "unknown-support"}
    job = call(case, worker, "scene.check.submit", {**params, "check": "compatibility"})
    case.assertEqual(_host_wait(case, worker, job["job_id"])["status"], "completed")
    result = call(case, worker, "job.result", {"job_id": job["job_id"], "limit": 256})
    case.assertIs(result["complete"], True, result)
    rows = [row for row in result["rows"] if row["code"] == "compat.groundzero_effect"]
    case.assertEqual(len(rows), 1, result["rows"])
    case.assertEqual((rows[0]["certainty"], rows[0]["severity"]), ("unknown_support", "info"))
    case.assertEqual(
        (rows[0]["row_kind"], rows[0]["reader"]), ("compatibility", "legacy_opentoonz")
    )
