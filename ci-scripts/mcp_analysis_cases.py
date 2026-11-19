"""Exercise drawing analysis jobs end to end: admission, progress, cancel, rows and loss."""

from __future__ import annotations

import os
import signal
import subprocess
import time
from typing import TYPE_CHECKING, Any

from mcp_drawing_cases import INDEXED, RECTANGLE, _level, _style
from mcp_jobs_cases import COMMON_FIELDS
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

ANALYSIS_FIELDS = COMMON_FIELDS | {"analysis"}
RECEIPT_FIELDS = {"operation_id", "revision"}
TERMINAL = {"completed", "failed", "canceled", "deadline_exceeded"}
STROKES = 256
POINTS = 9
DRAWINGS = 30


def _square(x: int, y: int, style: int) -> dict[str, Any]:
    ring = [(0, 0), (20, 0), (40, 0), (40, 20), (40, 40), (20, 40), (0, 40), (0, 20), (0, 0)]
    return {
        "action": "add",
        "style_id": style,
        "closed": True,
        "points": [{"x": x + dx, "y": y + dy, "thickness": 1} for dx, dy in ring],
    }


def _number(index: int) -> dict[str, Any]:
    return {"number": index, "suffix": ""}


def _heavy_level(
    case: NativeAcceptance, drawings: int = DRAWINGS, strokes: int = STROKES
) -> tuple[str, str]:
    """A level of identical overlapping-square drawings whose region rebuild takes time."""
    document = case.mutate("scene.create", None, name="Analysis")["document_id"]
    level = case.mutate("level.create", document, name="Ink", format="vector")["level_id"]
    case.mutate(
        "palette.create_style",
        document,
        level_id=level,
        name="Second",
        color={"r": 200, "g": 30, "b": 30, "a": 255},
    )
    target = {"level_id": level, "drawing_id": _number(1)}
    case.mutate("drawing.create", document, **target, format="vector")
    squares = [_square((i % 16) * 30, (i // 16) * 30, 1 + i % 2) for i in range(strokes)]
    for start in range(0, strokes, 128):
        case.mutate(
            "vector.edit",
            document,
            **target,
            coordinate_space="drawing_native",
            sharing="all_exposures",
            edits=squares[start : start + 128],
        )
    case.mutate(
        "vector.fill",
        document,
        **target,
        coordinate_space="drawing_native",
        sharing="all_exposures",
        x=5,
        y=5,
        style_id=2,
    )
    for index in range(2, drawings + 1):
        case.mutate("drawing.clone", document, **target, destination_drawing_id=_number(index))
    return document, level


def _submit(case: NativeAcceptance, document: str, level: str, **extra: Any) -> dict[str, Any]:
    return case.mutate("drawing.analysis.submit", document, level_id=level, **extra)


def _refused(
    case: NativeAcceptance, document: str, level: str, code: str, **extra: Any
) -> dict[str, Any]:
    params = case.write_params(document, {"level_id": level, **extra})
    error = case.failure("drawing.analysis.submit", params)
    case.assertEqual(error["code"], code, error)
    return error


def _require_record(case: NativeAcceptance, record: dict[str, Any]) -> None:
    case.assertEqual(set(record) - RECEIPT_FIELDS, ANALYSIS_FIELDS, record)
    case.assertEqual((record["kind"], record["lane"]), ("drawing_analysis", "analysis"))
    case.assertEqual(record["executor"], "worker")
    case.assertEqual(record["progress"]["unit"], "drawings")
    analysis = record["analysis"]
    case.assertEqual(set(analysis), {"check", "estimate", "cost", "budget", "complete", "reasons"})
    case.assertEqual(
        set(analysis["cost"]),
        {
            "elapsed_ms",
            "drawings_done",
            "strokes_scanned",
            "regions_built",
            "pixels_scanned",
            "files_checked",
            "bytes_read",
            "helpers_started",
        },
    )
    case.assertEqual(record["terminal"], record["status"] in TERMINAL)
    if record["status"] in {"completed", "canceled", "deadline_exceeded"}:
        case.assertIsInstance(analysis["complete"], bool)
    else:
        case.assertIsNone(analysis["complete"])


def _wait_running(case: NativeAcceptance, job: str) -> dict[str, Any]:
    """Poll until the job reports finished drawings while it is still running."""
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        status = case.success("job.status", {"job_id": job})
        _require_record(case, status)
        if status["terminal"] or status["progress"]["done"] > 0:
            return status
        time.sleep(0.02)
    raise AssertionError("The analysis made no progress within 60 seconds")


def _rows(case: NativeAcceptance, job: str) -> list[dict[str, Any]]:
    """Every row of a job's result, read in pages of at most 7."""
    rows: list[dict[str, Any]] = []
    offset: int | None = 0
    while offset is not None:
        page = case.success("job.result", {"job_id": job, "offset": offset, "limit": 7})
        case.assertLessEqual(len(page["rows"]), 7)
        case.assertEqual(page["job_id"], job)
        rows.extend(page["rows"])
        offset = page["next_offset"]
    case.assertEqual(page["total_rows"], len(rows))
    return rows


def _inspect_all(case: NativeAcceptance, document: str, level: str) -> list[Any]:
    pages = []
    for index in (1, 2, DRAWINGS):
        target = {"level_id": level, "drawing_id": _number(index)}
        pages.append(case.success("vector.inspect", {"document_id": document, **target}))
    return pages


def _admission(case: NativeAcceptance, document: str, level: str) -> None:
    """The estimate is reported at submit; over-budget and invalid requests start nothing."""
    over = _refused(case, document, level, "resource_limit", check="regions", max_drawings=5)
    case.assertEqual(over["details"]["limit"], "max_drawings")
    case.assertEqual(over["details"]["estimate"]["drawings"], DRAWINGS)
    _refused(case, document, level, "invalid_params", check="regions", style_ids=[1])
    missing = {"number": 99, "suffix": ""}
    _refused(case, document, level, "not_found", check="roles", drawing_ids=[missing])
    case.assertEqual(case.success("job.list", {"kind": "drawing_analysis"})["total"], 0)
    case.schema_failure(
        "drawing.analysis.submit",
        case.write_params(document, {"level_id": level, "check": "unused_styles"}),
    )


def _progress_and_cancel(case: NativeAcceptance, document: str, level: str) -> None:
    """A running job reports progress and cost, refuses a second job, and keeps rows on cancel."""
    revision = case.revisions[document]
    before = _inspect_all(case, document, level)
    job = _submit(case, document, level, check="regions")
    _require_record(case, job)
    estimate = job["analysis"]["estimate"]
    case.assertEqual(
        estimate,
        {
            "drawings": DRAWINGS,
            "strokes": DRAWINGS * STROKES,
            "control_points": DRAWINGS * STROKES * POINTS,
            "pixels": 0,
            "files": 0,
            "bytes": 0,
        },
    )
    busy = _refused(case, document, level, "analysis_busy", check="roles")
    case.assertEqual(busy["details"]["active_job_id"], job["job_id"])
    running = _wait_running(case, job["job_id"])
    case.assertEqual(running["progress"]["total"], DRAWINGS)
    early = case.failure("job.result", {"job_id": job["job_id"]})
    case.assertEqual(early["code"], "result_unavailable", early)
    canceled = case.success("job.cancel", case.write_params(None, {"job_id": job["job_id"]}))
    case.assertIn(canceled["status"], {"cancellation_requested", "canceled", "completed"})
    ended = case.wait_job(job["job_id"])
    _require_record(case, ended)
    case.assertEqual(ended["status"], "canceled", ended)
    analysis = ended["analysis"]
    case.assertEqual((analysis["complete"], analysis["reasons"]), (False, ["canceled"]))
    case.assertEqual(analysis["cost"]["regions_built"] > 0, True)
    rows = _rows(case, job["job_id"])
    case.assertEqual(len(rows), ended["progress"]["done"])
    case.assertEqual(ended["outputs"]["result_rows"], len(rows))
    case.assertLess(len(rows), DRAWINGS)
    case.assertEqual({row["row_kind"] for row in rows}, {"region_summary"})
    case.assertEqual(case.revisions[document], revision)
    case.assertEqual(_inspect_all(case, document, level), before)


def _usage(rows: list[dict[str, Any]], drawing: int) -> dict[int, dict[str, Any]]:
    return {
        row["style_id"]: row
        for row in rows
        if row["row_kind"] == "style_usage" and row["drawing_id"]["number"] == drawing
    }


def _recount(case: NativeAcceptance, document: str, level: str) -> None:
    """A completed run's totals equal what vector.inspect and the region summary report."""
    ids = [_number(index) for index in (1, 2, 3)]
    regions = _submit(case, document, level, check="regions", drawing_ids=ids)
    done = case.wait_job(regions["job_id"])
    case.assertEqual((done["status"], done["analysis"]["complete"]), ("completed", True))
    summary = [row for row in _rows(case, regions["job_id"]) if row["row_kind"] == "region_summary"]
    case.assertEqual([row["drawing_id"] for row in summary], ids)
    case.assertEqual({row["built_on"] for row in summary}, {"detached_clone"})
    case.assertEqual(summary[0]["regions"], summary[0]["painted"] + summary[0]["unpainted"])
    case.assertGreater(summary[0]["painted"], 0)
    usage = _submit(case, document, level, check="style_usage", drawing_ids=ids)
    case.assertEqual(case.wait_job(usage["job_id"])["status"], "completed")
    rows = _rows(case, usage["job_id"])
    inspected = case.success(
        "vector.inspect", {"document_id": document, "level_id": level, "drawing_id": ids[0]}
    )
    strokes = {row["style_id"]: row["strokes"] for row in inspected["summary"]["style_usage"]}
    per_drawing = _usage(rows, 1)
    case.assertEqual({k: v["strokes"] for k, v in per_drawing.items() if v["strokes"]}, strokes)
    by_style = {row["style_id"]: row["regions"] for row in summary[0]["by_style"]}
    case.assertEqual({k: v["regions"] for k, v in per_drawing.items() if v["regions"]}, by_style)
    totals = {row["style_id"]: row for row in rows if row["row_kind"] == "style_total"}
    case.assertEqual(totals[1]["strokes"], 3 * strokes[1])
    case.assertEqual(totals[1]["drawings"], 3)
    case.assertTrue(totals[2]["defined"])
    only = _submit(case, document, level, check="style_usage", style_ids=[2], drawing_ids=ids)
    case.wait_job(only["job_id"])
    filtered = _rows(case, only["job_id"])
    case.assertEqual({row["style_id"] for row in filtered}, {2})
    roles = _submit(case, document, level, check="roles", drawing_ids=ids[:1])
    case.wait_job(roles["job_id"])
    case.assertEqual(
        _rows(case, roles["job_id"])[0],
        {
            "row_kind": "role_summary",
            "level_id": level,
            "drawing_id": ids[0],
            "line_art": STROKES,
            "colour_art": 0,
            "underlay": 0,
            "overlay": 0,
            "layered": False,
            "colour_first": None,
            "colour_last": None,
        },
    )


def _revision_bound(case: NativeAcceptance, document: str, level: str) -> None:
    """A write while the job runs ends it failed with revision_changed and no rows."""
    job = _submit(case, document, level, check="regions")
    target = {"level_id": level, "drawing_id": _number(DRAWINGS)}
    case.mutate(
        "vector.opacity.set",
        document,
        **target,
        sharing="all_exposures",
        stroke_indices=[0],
        opacity_per_mille=300,
    )
    ended = case.wait_job(job["job_id"])
    case.assertEqual(ended["status"], "failed", ended)
    case.assertEqual(ended["failure"]["code"], "revision_changed")
    case.assertEqual(ended["failure"]["phase"], "analyze")
    case.assertEqual(ended["source_revision"], job["source_revision"])
    case.assertNotEqual(ended["source_revision"], case.revisions[document])
    result = case.failure("job.result", {"job_id": job["job_id"]})
    case.assertEqual(result["code"], "result_unavailable", result)
    case.assertEqual(ended["outputs"]["result_rows"], 0)


def drawing_analysis_job(case: NativeAcceptance) -> None:
    """One analysis job from admission to rows, without changing the document.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, level = _heavy_level(case)
    _admission(case, document, level)
    _progress_and_cancel(case, document, level)
    _recount(case, document, level)
    _revision_bound(case, document, level)
    listed = case.success("job.list", {"kind": "drawing_analysis", "limit": 64})
    case.assertEqual(listed["total"], 6)
    case.assertTrue(all(row["terminal"] for row in listed["jobs"]))


def drawing_analysis_retention_and_replay(case: NativeAcceptance) -> None:
    """Retries return the same job; the 33rd retained analysis job is refused.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, level = _heavy_level(case, drawings=1)
    params = case.write_params(
        document, {"level_id": level, "check": "roles", "drawing_ids": [_number(1)]}
    )
    first = case.success("drawing.analysis.submit", params)
    case.assertEqual(case.success("drawing.analysis.submit", params), first)
    changed = case.failure("drawing.analysis.submit", {**params, "check": "regions"})
    case.assertEqual(changed["code"], "operation_conflict", changed)
    case.wait_job(first["job_id"])
    for _ in range(31):
        case.wait_job(_submit(case, document, level, check="roles")["job_id"])
    full = _refused(case, document, level, "resource_limit", check="roles")
    case.assertEqual(full["details"]["limit"], "retained_analysis_jobs")
    case.assertEqual(case.success("job.list", {"kind": "drawing_analysis"})["total"], 32)
    case.assertEqual(case.success("job.list", {"kind": "render_preview"})["total"], 0)


def drawing_analysis_toonz_raster(case: NativeAcceptance) -> None:
    """Toonz raster drawings count ink and paint pixels; roles need a vector level.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document = case.create("Raster analysis")
    target = _level(case, document, "toonzRaster")
    case.assertEqual(_style(case, document, target["level_id"]), 2)
    case.mutate(
        "raster.patch",
        document,
        **target,
        **RECTANGLE,
        sharing="all_exposures",
        pixels=INDEXED,
    )
    level = target["level_id"]
    _refused(case, document, level, "unsupported_type", check="roles")
    job = _submit(case, document, level, check="style_usage")
    case.assertEqual(job["analysis"]["estimate"]["pixels"], 6)
    ended = case.wait_job(job["job_id"])
    case.assertEqual(
        (ended["status"], ended["analysis"]["cost"]["pixels_scanned"]), ("completed", 6)
    )
    rows = _rows(case, job["job_id"])
    totals = {row["style_id"]: row for row in rows if row["row_kind"] == "style_total"}
    case.assertEqual(
        {k: (v["ink_pixels"], v["paint_pixels"]) for k, v in totals.items()},
        {0: (2, 2), 1: (2, 1), 2: (2, 3)},
    )
    case.assertTrue(totals[2]["defined"])


def _native_pids(case: NativeAcceptance) -> list[int]:
    listing = subprocess.run(
        ["ps", "-axo", "pid=,command="], capture_output=True, text=True, check=True
    ).stdout
    marker = f"--workspace {case.workspace}"
    pids = []
    for line in listing.splitlines():
        pid, _, command = line.strip().partition(" ")
        native = command.split(" ", 1)[0].endswith("/OpenToonz")
        if native and "--automation-worker" in command and marker in command:
            pids.append(int(pid))
    return pids


def drawing_analysis_lost_worker(case: NativeAcceptance) -> None:
    """After the native worker dies, its running analysis job is unknown, never completed.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, level = _heavy_level(case)
    job = _submit(case, document, level, check="regions")
    pids = _native_pids(case)
    case.assertEqual(len(pids), 1, pids)
    os.kill(pids[0], signal.SIGKILL)
    if case.through_sdk:
        lost = case.success("job.status", {"job_id": job["job_id"]})
        case.assertEqual((lost["status"], lost["kind"]), ("unknown", "drawing_analysis"))
        case.assertEqual(lost["worker_loss"]["cause"], "worker_exit")
        case.assertTrue(lost["terminal"])
        return
    case.worker.process.wait(timeout=30)
    fresh = Worker(
        case.executable, case.workspace, case.inputs, case.directory / "fresh.stderr.log"
    )
    case.addCleanup(fresh.close)
    answer = fresh.call("job.status", {"job_id": job["job_id"]})
    case.assertEqual(answer["error"]["code"], "unknown_job", answer)
    case.assertEqual(answer["error"]["details"]["reason"], "other_worker")


def drawing_analysis_polls_during_a_long_rebuild(case: NativeAcceptance) -> None:
    """Polls and a cancellation are answered while one large drawing's regions are built.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, level = _heavy_level(case, drawings=1, strokes=1600)
    job = _submit(case, document, level, check="regions")
    slowest = 0.0
    for _ in range(10):
        started = time.monotonic()
        status = case.success("job.status", {"job_id": job["job_id"]})
        slowest = max(slowest, time.monotonic() - started)
        case.assertIn(status["status"], {"preparing", "running", "completed"})
        time.sleep(0.02)
    case.assertLess(slowest, 0.5)
    started = time.monotonic()
    asked = case.success("job.cancel", case.write_params(None, {"job_id": job["job_id"]}))
    case.assertIn(asked["status"], {"cancellation_requested", "canceled", "completed"})
    ended = case.wait_job(job["job_id"])
    case.assertIn(ended["status"], {"canceled", "completed"})
    case.assertLess(time.monotonic() - started, 5)
    if ended["status"] == "canceled":
        case.assertEqual(ended["analysis"]["reasons"], ["canceled"])
        case.assertEqual(ended["cancellation"], {**ended["cancellation"], "effective": True})
