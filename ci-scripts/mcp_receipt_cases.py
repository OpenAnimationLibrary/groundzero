"""Exercise bounded cancellation receipts after ordinary write admission fills."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

ORDINARY_RECEIPTS = 2048
CONTROL_RECEIPTS = 32


def _retain_completed_jobs(case: NativeAcceptance, document: str) -> list[dict[str, Any]]:
    jobs = []
    for _index in range(CONTROL_RECEIPTS - 1):
        job = case.mutate("render.preview", document, frame=0, width=4, height=4)
        status = case.wait_job(job["job_id"])
        case.assertEqual(status["status"], "completed", status)
        case.assertIs(status["writers_closed"], True)
        artifacts = case.success("job.result", {"job_id": job["job_id"]})
        case.assertEqual(len(artifacts["artifacts"]), 1)
        jobs.append({"status": status, "artifacts": artifacts})
    return jobs


def _fill_ordinary_receipts(
    case: NativeAcceptance, document: str
) -> tuple[dict[str, Any], dict[str, Any]]:
    remaining = ORDINARY_RECEIPTS - 1 - case.operations
    case.assertGreater(remaining, 0)
    first_params: dict[str, Any] = {}
    first_result: dict[str, Any] = {}
    for index in range(remaining):
        params = case.write_params(document, {"width_px": 32})
        result = case.success("scene.settings", params)
        case.assertEqual(result["revision"], case.revisions[document])
        if index == 0:
            first_params, first_result = params, result
    case.assertEqual(case.operations, ORDINARY_RECEIPTS - 1)
    return first_params, first_result


def _cancel_once(case: NativeAcceptance, params: dict[str, Any], document: str) -> dict[str, Any]:
    result = case.success("job.cancel", params)
    case.assertEqual(result["job_id"], params["job_id"])
    case.assertEqual(result["operation_id"], params["operation_id"])
    case.assertEqual(case.success("job.cancel", params), result)
    changed = {**params, "job_id": "unknown-job-for-receipt-conflict"}
    case.assertEqual(case.failure("job.cancel", changed)["code"], "operation_conflict")
    other_method = case.write_params(document, {"width_px": 32})
    other_method["operation_id"] = params["operation_id"]
    case.assertEqual(case.failure("scene.settings", other_method)["code"], "operation_conflict")
    fresh = case.write_params(None, {"job_id": params["job_id"]})
    case.assertEqual(case.failure("job.cancel", fresh)["code"], "resource_limit")
    return result


def _close_naturally(case: NativeAcceptance) -> None:
    stream = case.worker.process.stdin
    if stream is None:
        case.fail("Receipt-pressure worker did not retain its protocol input pipe")
    stream.close()
    case.worker.process.wait(timeout=30)
    diagnostics = case.worker.log_path.read_text(encoding="utf-8", errors="replace")
    case.assertEqual(case.worker.process.returncode, 0, diagnostics)


def receipt_capacity_preserves_bounded_cancellation(case: NativeAcceptance) -> None:
    """Keep bounded first-cancel receipts independent of ordinary write capacity.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Receipt pressure")
    effect = case.mutate("fx.create", document, type="colorCardFx", frame_count=120)
    case.mutate("fx.terminal.set", document, fx_id=effect["fx_id"], enabled=True)
    completed = _retain_completed_jobs(case, document)
    early_cancel = {
        "job_id": completed[0]["status"]["job_id"],
        "operation_id": "reserved-before-ordinary-capacity",
    }
    early_result = case.success("job.cancel", early_cancel)
    case.assertEqual(early_result["status"], "completed", early_result)
    completed[0]["cancel_params"] = early_cancel
    ordinary_params, ordinary_result = _fill_ordinary_receipts(case, document)
    before = case.success("scene.summary", {"document_id": document})
    # All 32 retained jobs must fit the independent 128 MiB render-output budget.
    job = case.mutate(
        "render.submit", document, start_frame=0, end_frame=119, width=480, height=480
    )
    case.assertEqual(case.operations, ORDINARY_RECEIPTS)
    case.assertEqual(job["status"], "preparing", job)
    case.assertIs(job["writers_closed"], False)
    # Only retained jobs have reserved cancellations; this one needs ordinary capacity.
    cancel = case.write_params(None, {"job_id": "unknown-job-before-cancellation"})
    case.assertEqual(case.failure("job.cancel", cancel)["code"], "resource_limit")
    pending = case.success("job.status", {"job_id": job["job_id"]})
    case.assertIn(pending["status"], {"preparing", "running", "finalizing"}, pending)
    case.assertIs(pending["writers_closed"], False)
    cancel["job_id"] = job["job_id"]
    canceled = _cancel_once(case, cancel, document)
    case.assertIn(canceled["status"], {"cancellation_requested", "canceled"}, canceled)
    terminal = case.wait_job(job["job_id"])
    case.assertEqual(terminal["status"], "canceled", terminal)
    case.assertIs(terminal["writers_closed"], True)
    case.assertEqual(case.success("job.cancel", cancel), canceled)
    denied = case.failure("job.result", {"job_id": job["job_id"]})
    case.assertEqual(denied["code"], "artifacts_unavailable")
    for retained in completed:
        job_id = retained["status"]["job_id"]
        params = retained.get("cancel_params") or case.write_params(None, {"job_id": job_id})
        result = _cancel_once(case, params, document)
        case.assertEqual(result["status"], "completed", result)
        case.assertIs(result["writers_closed"], True)
        status = case.success("job.status", {"job_id": job_id})
        case.assertEqual(status["cancellation"]["requested"], True)
        case.assertEqual(status["cancellation"]["effective"], False)
        unchanged = {key for key in status if key != "cancellation"}
        case.assertEqual(
            {key: status[key] for key in unchanged},
            {key: retained["status"][key] for key in unchanged},
        )
        result = case.success("job.result", {"job_id": job_id})
        case.assertEqual(result["artifacts"], retained["artifacts"]["artifacts"])
    exhausted = case.write_params(None, {"job_id": "unknown-job-after-reserve-full"})
    case.assertEqual(case.failure("job.cancel", exhausted)["code"], "resource_limit")
    fresh = case.write_params(document, {"width_px": 64})
    case.assertEqual(case.failure("scene.settings", fresh)["code"], "resource_limit")
    case.assertEqual(case.success("scene.settings", ordinary_params), ordinary_result)
    case.assertEqual(case.success("job.cancel", cancel), canceled)
    case.assertEqual(case.success("job.cancel", early_cancel), early_result)
    ordinary_conflict = {"job_id": job["job_id"], "operation_id": ordinary_params["operation_id"]}
    case.assertEqual(case.failure("job.cancel", ordinary_conflict)["code"], "operation_conflict")
    case.assertEqual(case.success("scene.summary", {"document_id": document}), before)
    case.assertEqual(len(case.success("automation.list_documents", {})["documents"]), 1)
    case.assertEqual(case.success("automation.capabilities", {})["protocol_version"], 3)
    evidence: dict[str, Any] = {
        "ordinary_receipts": ORDINARY_RECEIPTS,
        "control_receipts": CONTROL_RECEIPTS,
    }
    evidence.update(before_cancel=pending, cancellation=canceled, terminal=terminal)
    path = case.directory / "receipt-capacity.json"
    path.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    _close_naturally(case)
    evidence["worker_exit_code"] = case.worker.process.returncode
    path.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
