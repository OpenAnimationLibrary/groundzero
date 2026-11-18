"""Acceptance cases for the job tools: states, identity, retries, listing and stale ids."""

from __future__ import annotations

import hashlib
import time
import uuid
from typing import TYPE_CHECKING, Any

from mcp_failure_cases import _fault_worker, _scene
from mcp_live import verified_build_digest
from mcp_movie_cases import _fixture, _media_directory, _options, _start_configured_worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

COMMON_FIELDS = {
    "job_id",
    "kind",
    "lane",
    "executor",
    "document_id",
    "source_revision",
    "status",
    "terminal",
    "worker",
    "plugin_policy",
    "limits",
    "inputs",
    "progress",
    "cancellation",
    "termination",
    "failure",
    "stalled",
    "timing",
    "outputs",
}
RENDER_FIELDS = COMMON_FIELDS | {
    "frame_count",
    "completed_frames",
    "failed_frames",
    "writers_closed",
    "width",
    "height",
    "start_frame",
    "end_frame",
    "step",
    "frame_rate",
    "format",
    "profile",
    "color_space",
    "color_space_gamma",
    "alpha",
    "fx_provenance",
    "effective",
}
SUMMARY_FIELDS = {
    "job_id",
    "kind",
    "lane",
    "document_id",
    "source_revision",
    "status",
    "terminal",
    "timing",
    "observed_by",
}
PREVIEW = {"frame": 0, "width": 32, "height": 32}


def require_record(case: NativeAcceptance, record: dict[str, Any]) -> None:
    """Require a render job record with exactly the contract's fields and bounds.

    A write's result also carries the worker's receipt fields."""
    extra = {"assistance", "operation_id", "revision"}
    case.assertEqual(set(record) - extra, RENDER_FIELDS, record)
    case.assertLessEqual(len(record["job_id"]), 80)
    case.assertEqual(record["lane"], "render")
    case.assertEqual(record["plugin_policy"], "builtin_only")
    case.assertEqual(set(record["timing"]), {"submitted_ms", "started_ms", "ended_ms"})
    case.assertEqual(set(record["cancellation"]), {"requested", "requested_ms", "effective"})
    case.assertEqual(set(record["termination"]), {"mode", "forced_processes"})
    case.assertEqual(record["terminal"], record["status"] in TERMINAL)
    failure = record["failure"]
    if failure is not None:
        case.assertEqual(set(failure), {"code", "phase", "message"})
        case.assertLessEqual(len(failure["message"]), 2048)


TERMINAL = {"completed", "failed", "canceled", "deadline_exceeded"}


def _held(case: NativeAcceptance) -> str:
    """A worker whose render jobs wait before their first frame until canceled."""
    _fault_worker(case, "held", "--test-render-fault", "hold-launch")
    return _scene(case)


def job_states_and_refused_second_submit(case: NativeAcceptance) -> None:
    """A second render while one runs is render_busy, creates nothing, and lists one job."""
    document = _held(case)
    job = case.mutate("render.submit", document, start_frame=0, end_frame=3, width=32, height=32)
    require_record(case, job)
    case.assertEqual((job["status"], job["kind"]), ("preparing", "render_sequence"))
    refused = case.failure("render.preview", case.write_params(document, PREVIEW))
    case.assertEqual(refused["code"], "render_busy", refused)
    case.assertEqual(refused["details"]["job_id"], job["job_id"])
    listed = case.success("job.list", {"state": "active"})
    case.assertEqual([row["job_id"] for row in listed["jobs"]], [job["job_id"]])
    case.assertEqual(len(list((case.workspace / "renders").iterdir())), 1)
    canceled = case.success("job.cancel", case.write_params(None, {"job_id": job["job_id"]}))
    case.assertIn(canceled["status"], {"cancellation_requested", "canceled"})
    ended = case.wait_job(job["job_id"])
    case.assertEqual(ended["status"], "canceled", ended)
    case.assertEqual(ended["cancellation"]["effective"], True)
    case.assertEqual(ended["termination"], {"mode": "cooperative", "forced_processes": []})
    case.assertIsNone(ended["failure"])
    timing = ended["timing"]
    case.assertLessEqual(timing["submitted_ms"], ended["cancellation"]["requested_ms"])
    case.assertLessEqual(ended["cancellation"]["requested_ms"], timing["ended_ms"])


def job_record_identity_inputs_limits(case: NativeAcceptance) -> None:
    """A job names its worker and build, its admitted request, limits and outputs.

    The job record, automation.capabilities and runtime.inspect report one worker identity:
    the registry's instance id and the shared build digest.
    """
    worker = case.success("automation.capabilities", {})["worker"]
    case.assertEqual(case.success("runtime.inspect", {})["worker"], worker)
    document = case.create()
    job = case.mutate("render.preview", document, **PREVIEW)
    record = case.wait_job(job["job_id"])
    require_record(case, record)
    case.assertEqual(record["worker"], worker)
    case.assertEqual(record["worker"]["build"]["build_digest"], _build_digest(case))
    case.assertTrue(record["job_id"].startswith(f"job_{worker['instance_id'][:8]}_"))
    case.assertEqual((record["kind"], record["executor"]), ("render_preview", "worker"))
    case.assertEqual(
        record["limits"],
        {"deadline_ms": 600000, "storage_reserved_bytes": 69632, "cancel_grace_ms": 30000},
    )
    request = {**PREVIEW, "document_id": document}
    case.assertEqual(record["inputs"]["request"], request)
    directory = case.workspace / record["outputs"]["directory"]
    case.assertEqual(directory, case.workspace / "renders" / record["job_id"])
    manifest = (directory / "snapshot" / "manifest.json").read_bytes()
    case.assertEqual(
        record["inputs"]["snapshot_manifest_sha256"], hashlib.sha256(manifest).hexdigest()
    )
    case.assertIsNone(record["inputs"]["source_fingerprint"])
    case.assertEqual(record["progress"], {"unit": "frames", "done": 1, "total": 1})
    case.assertEqual(record["outputs"]["artifact_count"], 1)
    result = case.success("job.result", {"job_id": record["job_id"]})
    case.assertEqual(set(result), {"job", "artifacts"})
    case.assertEqual(result["job"], case.success("job.status", {"job_id": record["job_id"]}))


def _build_digest(case: NativeAcceptance) -> str:
    return verified_build_digest(case.native_executable)


def submit_retry_returns_same_job(case: NativeAcceptance) -> None:
    """An exact retry of a job-starting call returns the stored job and starts no render."""
    document = case.create()
    params = case.write_params(document, PREVIEW)
    first = case.success("render.preview", params)
    retry = case.success("render.preview", params)
    case.assertEqual(retry, first)
    case.wait_job(first["job_id"])
    case.assertEqual(case.success("job.list", {})["total"], 1)
    case.assertEqual(
        [path.name for path in (case.workspace / "renders").iterdir()], [first["job_id"]]
    )
    changed = case.failure("render.preview", {**params, "frame": 1})
    case.assertEqual(changed["code"], "operation_conflict")


def repeated_status_and_cancel(case: NativeAcceptance) -> None:
    """Status is stable once terminal; a late cancellation is recorded and changes nothing."""
    document = case.create()
    job = case.mutate("render.preview", document, **PREVIEW)
    done = case.wait_job(job["job_id"])
    case.assertEqual(done["status"], "completed")
    case.assertEqual(case.success("job.status", {"job_id": job["job_id"]}), done)
    cancel = case.write_params(None, {"job_id": job["job_id"]})
    late = case.success("job.cancel", cancel)
    case.assertEqual(late["status"], "completed")
    case.assertEqual(late["cancellation"]["requested"], True)
    case.assertEqual(late["cancellation"]["effective"], False)
    case.assertIsNone(late["termination"]["mode"])
    case.assertEqual(case.success("job.cancel", cancel), late)
    case.assertEqual(len(case.success("job.result", {"job_id": job["job_id"]})["artifacts"]), 1)


def stale_job_ids(case: NativeAcceptance) -> None:
    """An id from another worker or never issued is refused and says which."""
    worker = case.success("automation.capabilities", {})["worker"]["instance_id"]
    issued_here = f"job_{worker[:8]}_{uuid.uuid4()}"
    other = f"job_{'0' * 8 if worker[:8] != '0' * 8 else '1' * 8}_{uuid.uuid4()}"
    for job, reason in ((issued_here, "not_issued"), (other, "other_worker")):
        for method in ("job.status", "job.result"):
            error = case.failure(method, {"job_id": job})
            case.assertEqual(error["code"], "unknown_job")
            case.assertEqual(error["details"], {"reason": reason, "worker_instance_id": worker})
    artifact = case.failure("artifact.read", {"artifact_id": f"artifact_{other[4:]}"})
    case.assertEqual(
        (artifact["code"], artifact["details"]["reason"]), ("unknown_artifact", "other_worker")
    )


def job_list_filters_and_paging(case: NativeAcceptance) -> None:
    """job.list filters by document, kind and state, pages newest first, and keeps discarded
    documents' jobs."""
    first = case.create("First")
    second = case.create("Second")
    jobs = []
    for document, method in ((first, "render.preview"), (second, "render.preview")):
        jobs.append(case.mutate(method, document, **PREVIEW)["job_id"])
        case.wait_job(jobs[-1])
    sequence = case.mutate("render.submit", first, start_frame=0, end_frame=1, width=8, height=8)
    jobs.append(sequence["job_id"])
    case.wait_job(jobs[-1])
    listed = case.success("job.list", {"limit": 2})
    case.assertEqual([row["job_id"] for row in listed["jobs"]], jobs[::-1][:2])
    case.assertEqual((listed["total"], listed["next_offset"]), (3, 2))
    for row in listed["jobs"]:
        case.assertEqual(set(row), SUMMARY_FIELDS)
        case.assertEqual(row["observed_by"], "worker")
    rest = case.success("job.list", {"offset": 2, "limit": 2})
    case.assertEqual(
        ([row["job_id"] for row in rest["jobs"]], rest["next_offset"]), ([jobs[0]], None)
    )
    kinds = case.success("job.list", {"kind": "render_sequence"})["jobs"]
    case.assertEqual([row["job_id"] for row in kinds], [jobs[2]])
    case.assertEqual(case.success("job.list", {"state": "active"})["total"], 0)
    case.mutate("scene.discard", second)
    kept = case.success("job.list", {"document_id": second})
    case.assertEqual([row["job_id"] for row in kept["jobs"]], [jobs[1]])
    case.assertEqual(len(case.success("job.result", {"job_id": jobs[1]})["artifacts"]), 1)
    missing = case.failure("job.list", {"document_id": "document_never_opened"})
    case.assertEqual(missing["code"], "document_not_found")
    case.assertEqual(
        listed["limits"],
        {"retained_render_jobs": 32, "retained_analysis_jobs": 32, "active_per_lane": 1},
    )


def stalled_cancellation_stays_non_terminal(case: NativeAcceptance) -> None:
    """Writers still open 30 s after cancellation show stalled without a terminal claim."""
    _fault_worker(case, "stall", "--test-render-fault", "hold-writers")
    document = _scene(case)
    job = case.mutate("render.submit", document, start_frame=0, end_frame=3, width=32, height=32)
    case.success("job.cancel", case.write_params(None, {"job_id": job["job_id"]}))
    started = time.monotonic()
    early = case.success("job.status", {"job_id": job["job_id"]})
    case.assertEqual((early["status"], early["stalled"]), ("cancellation_requested", False))
    while time.monotonic() - started < 31:
        case.success("scene.summary", {"document_id": document})
        time.sleep(0.5)
    late = case.success("job.status", {"job_id": job["job_id"]})
    case.assertEqual((late["status"], late["terminal"]), ("cancellation_requested", False))
    case.assertIs(late["stalled"], True)
    again = case.success("job.cancel", case.write_params(None, {"job_id": job["job_id"]}))
    case.assertIs(again["stalled"], True)
    ended = case.wait_job(job["job_id"])
    case.assertEqual((ended["status"], ended["stalled"]), ("canceled", False), ended)


def movie_cancel_while_encoding_is_forced(case: NativeAcceptance) -> None:
    """Cancelling an MP4 job while its encoder runs reports the encoder as killed."""
    _start_configured_worker(case, _media_directory(case))
    document, _effect = _fixture(case)
    job = case.mutate("render.submit", document, **{**_options(), "end_frame": 129})["job_id"]
    deadline = time.monotonic() + 60
    status = case.success("job.status", {"job_id": job})
    while status["completed_frames"] < 3 and time.monotonic() < deadline:
        time.sleep(0.02)
        status = case.success("job.status", {"job_id": job})
    case.assertFalse(status["terminal"], status)
    case.success("job.cancel", case.write_params(None, {"job_id": job}))
    ended = case.wait_job(job)
    case.assertEqual(ended["status"], "canceled", ended)
    case.assertEqual(ended["termination"], {"mode": "forced", "forced_processes": ["encoder"]})
