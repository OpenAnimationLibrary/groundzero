"""Acceptance cases for job.discard: releasing finished jobs, the retention limit, restarts."""

from __future__ import annotations

import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_failure_cases import _fault_worker, _scene
from mcp_journal_cases import restart
from mcp_scene_access_cases import corpus, open_scene, wait

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

PREVIEW = {"frame": 0, "width": 16, "height": 16}
RETAINED = 32
LISTED_RELEASABLE = 8
RESULT_FIELDS = {
    "job_id",
    "kind",
    "final_status",
    "discarded",
    "freed_bytes",
    "retained_jobs",
    "operation_id",
}


def _preview(case: NativeAcceptance, document: str) -> str:
    job = case.mutate("render.preview", document, **PREVIEW)["job_id"]
    case.assertEqual(case.wait_job(job)["status"], "completed")
    return job


def _discard(case: NativeAcceptance, job: str) -> dict[str, Any]:
    return case.success("job.discard", case.write_params(None, {"job_id": job}))


def _tree_bytes(directory: Path) -> int:
    return sum(path.stat().st_size for path in directory.rglob("*") if path.is_file())


def _require_discarded(
    case: NativeAcceptance, error: dict[str, Any], job: str, status: str = "completed"
) -> None:
    case.assertEqual(error["code"], "job_discarded", error)
    details = error["details"]
    case.assertEqual((details["job_id"], details["final_status"]), (job, status), error)


def _require_releasable(
    case: NativeAcceptance, error: dict[str, Any], lane: str, oldest: list[str]
) -> None:
    details = error["details"]
    case.assertEqual(
        (details["lane"], details["limit"], details["max_retained_jobs"], details["retained"]),
        (lane, f"retained_{lane}_jobs", RETAINED, RETAINED),
        error,
    )
    case.assertEqual((details["releasable_count"], details["next"]), (RETAINED, "job.discard"))
    rows = details["releasable"]
    case.assertEqual([row["job_id"] for row in rows], oldest[:LISTED_RELEASABLE])
    case.assertEqual(set(rows[0]), {"job_id", "kind", "status", "document_id", "ended_ms"}, rows[0])
    case.assertIn("job.discard", error["message"])


@acceptance_case
def job_discard_releases_a_completed_render(case: NativeAcceptance) -> None:
    """A completed render's files go with its slot; every later read of the job or its
    artifact answers job_discarded, and an exact retry returns the recorded result."""
    document = case.create()
    job = _preview(case, document)
    artifact = case.success("job.result", {"job_id": job})["artifacts"][0]["artifact_id"]
    directory = case.workspace / "renders" / job
    held = _tree_bytes(directory)
    params = case.write_params(None, {"job_id": job})
    released = case.success("job.discard", params)
    case.assertEqual(set(released), RESULT_FIELDS, released)
    case.assertEqual(
        (released["job_id"], released["kind"], released["final_status"], released["discarded"]),
        (job, "render_preview", "completed", True),
    )
    case.assertEqual(released["freed_bytes"], held)
    case.assertEqual(released["retained_jobs"], {"render": 0, "analysis": 0})
    case.assertFalse(directory.exists(), "the job's private directory is still there")
    case.assertEqual(case.success("job.discard", params), released)
    for method in ("job.status", "job.result"):
        _require_discarded(case, case.failure(method, {"job_id": job}), job)
    for method in ("job.cancel", "job.discard"):
        _require_discarded(
            case, case.failure(method, case.write_params(None, {"job_id": job})), job
        )
    read = case.failure("artifact.read", {"artifact_id": artifact})
    _require_discarded(case, read, job)
    case.assertEqual(read["details"]["artifact_id"], artifact)
    case.assertEqual(case.success("job.list", {})["total"], 0)


@acceptance_case
def job_retention_limit_names_jobs_to_release(case: NativeAcceptance) -> None:
    """At 32 retained renders the next one is refused with the oldest releasable jobs and the
    call that frees them; after one discard the worker renders again."""
    document = case.create()
    jobs = [_preview(case, document) for _ in range(RETAINED)]
    refused = case.failure("render.preview", case.write_params(document, PREVIEW))
    case.assertEqual(refused["code"], "render_limit", refused)
    _require_releasable(case, refused, "render", jobs)
    check = {"document_id": document, "kind": "preview", "frame": 0}
    report = case.success(
        "render.preflight", {**check, "expected_revision": case.revisions[document]}
    )
    retention = next(check for check in report["checks"] if check["id"] == "retention")
    case.assertEqual((retention["status"], retention["code"]), ("fail", "render_limit"))
    case.assertEqual(retention["details"]["releasable"], refused["details"]["releasable"])
    case.assertEqual(_discard(case, jobs[0])["retained_jobs"]["render"], RETAINED - 1)
    newer = _preview(case, document)
    listed = [row["job_id"] for row in case.success("job.list", {"limit": 64})["jobs"]]
    case.assertEqual(listed, [newer, *jobs[:0:-1]])


@acceptance_case
def job_discard_releases_analysis_jobs(case: NativeAcceptance) -> None:
    """The analysis lane fills and is released the same way; a discarded check's rows are
    answered job_discarded."""
    document = case.create()
    checks = []
    for _ in range(RETAINED):
        job = case.mutate("scene.check.submit", document, check="inventory")["job_id"]
        case.assertEqual(case.wait_job(job)["status"], "completed")
        checks.append(job)
    refused = case.failure(
        "scene.check.submit", case.write_params(document, {"check": "inventory"})
    )
    case.assertEqual(
        (refused["code"], refused["details"]["limit"]),
        ("resource_limit", "retained_analysis_jobs"),
    )
    _require_releasable(case, refused, "analysis", checks)
    released = _discard(case, checks[0])
    case.assertEqual((released["kind"], released["freed_bytes"]), ("scene_check", 0))
    case.assertEqual(released["retained_jobs"], {"render": 0, "analysis": RETAINED - 1})
    _require_discarded(case, case.failure("job.result", {"job_id": checks[0]}), checks[0])
    case.mutate("scene.check.submit", document, check="inventory")


@acceptance_case(
    worker_only="needs --test-render-fault hold-launch, which the adapter never passes"
)
def job_discard_refuses_a_running_job(case: NativeAcceptance) -> None:
    """A running job is never touched: discard answers in_use and names job.cancel; once the
    canceled job is terminal it is released."""
    _fault_worker(case, "held", "--test-render-fault", "hold-launch")
    document = _scene(case)
    job = case.mutate("render.submit", document, start_frame=0, end_frame=3, width=32, height=32)
    job_id = job["job_id"]
    refused = case.failure("job.discard", case.write_params(None, {"job_id": job_id}))
    case.assertEqual(refused["code"], "in_use", refused)
    case.assertEqual(
        (refused["details"]["reason"], refused["details"]["next"]), ("not_terminal", "job.cancel")
    )
    case.assertTrue((case.workspace / "renders" / job_id).is_dir())
    case.assertFalse(case.success("job.status", {"job_id": job_id})["terminal"])
    case.success("job.cancel", case.write_params(None, {"job_id": job_id}))
    case.assertEqual(case.wait_job(job_id)["status"], "canceled")
    released = _discard(case, job_id)
    case.assertEqual((released["final_status"], released["freed_bytes"]), ("canceled", 0))
    _require_discarded(
        case, case.failure("job.status", {"job_id": job_id}), job_id, status="canceled"
    )


@acceptance_case
def job_discard_refuses_ids_it_does_not_hold(case: NativeAcceptance) -> None:
    """A discard of an id never issued here, or issued by another worker, is unknown_job and
    changes nothing."""
    document = case.create()
    kept = _preview(case, document)
    worker = case.success("automation.capabilities", {})["worker"]["instance_id"]
    own = f"job_{worker[:8]}_00000000-0000-4000-8000-000000000000"
    other = f"job_{'0' * 8 if worker[:8] != '0' * 8 else '1' * 8}_{own[13:]}"
    for job, reason in ((own, "not_issued"), (other, "other_worker")):
        error = case.failure("job.discard", case.write_params(None, {"job_id": job}))
        case.assertEqual((error["code"], error["details"]["reason"]), ("unknown_job", reason))
    case.assertEqual(case.success("job.list", {})["total"], 1)
    case.assertTrue((case.workspace / "renders" / kept).is_dir())


@acceptance_case
def job_discard_survives_restart(case: NativeAcceptance) -> None:
    """After a restart the discarded job stays discarded: reads answer job_discarded from the
    journal, job.list omits it, and an exact retry returns the recorded result."""
    document = case.create()
    job = _preview(case, document)
    kept = _preview(case, document)
    params = case.write_params(None, {"job_id": job})
    released = case.success("job.discard", params)
    restart(case, "after-discard")
    for method in ("job.status", "job.result"):
        _require_discarded(case, case.failure(method, {"job_id": job}), job)
    listed = case.success("job.list", {})["jobs"]
    case.assertEqual([(row["job_id"], row["observed_by"]) for row in listed], [(kept, "journal")])
    replay = case.success("job.discard", params)
    case.assertEqual(replay.pop("receipt")["handles_expired"], True)
    case.assertEqual(replay, released)
    other = case.failure("job.discard", case.write_params(None, {"job_id": kept}))
    case.assertEqual((other["code"], other["details"]["reason"]), ("unknown_job", "other_worker"))


@acceptance_case
def job_discard_releases_a_scene_host_job(case: NativeAcceptance) -> None:
    """A read-only document's render runs in its scene host; the worker forwards the discard,
    and afterwards answers the job and its artifact as discarded."""
    document = open_scene(case, case.worker, corpus(case)["B"], "standalone")["document_id"]
    params = {"document_id": document, "expected_revision": "0", "frame": 0, "width": 16}
    params.update(height=16, operation_id=f"host-release-{time.monotonic_ns()}")
    job = case.success("render.preview", params)["job_id"]
    case.assertEqual(wait(case, case.worker, job)["status"], "completed")
    artifact = case.success("job.result", {"job_id": job})["artifacts"][0]["artifact_id"]
    released = _discard(case, job)
    case.assertEqual((released["final_status"], released["discarded"]), ("completed", True))
    case.assertGreater(released["freed_bytes"], 0)
    case.assertEqual(released["retained_jobs"], {"render": 0, "analysis": 0})
    _require_discarded(case, case.failure("job.status", {"job_id": job}), job)
    _require_discarded(case, case.failure("artifact.read", {"artifact_id": artifact}), job)
    case.assertEqual(case.success("job.list", {"document_id": document})["total"], 0)
