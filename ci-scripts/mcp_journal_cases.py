"""Durable receipts: the workspace journal across worker restarts, crashes and damage."""

from __future__ import annotations

import json
import threading
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_failure_cases import _fault_worker
from mcp_file_publish_cases import staging_left
from mcp_scene_access_cases import corpus, open_scene, wait
from mcp_worker import FULL_GRANTS, Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

CRASH_EXIT_CODE = 86
FAULT_FLAG = "needs --test-write-fault, which the adapter never passes"
SCENE = {
    "name": "Durable",
    "width_px": 32,
    "height_px": 32,
    "width_in": 32 / 120,
    "height_in": 32 / 120,
    "frame_rate": 24,
}


def journal_directory(case: NativeAcceptance) -> Path:
    return case.workspace / ".groundzero" / "journal"


def records(case: NativeAcceptance) -> list[dict[str, Any]]:
    """Every verified record of the journal's segments, in file order."""
    found = []
    for segment in sorted(journal_directory(case).glob("seg-*.jsonl")):
        for line in segment.read_bytes().splitlines():
            found.append(json.loads(line[65:]))
    return found


def restart(case: NativeAcceptance, name: str, *flags: str, grants: str = FULL_GRANTS) -> Worker:
    """Close the case's worker and start another on the same workspace."""
    case.worker.close()
    case.worker = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / f"{name}.stderr.log",
        test_arguments=flags,
        grants=grants,
    )
    case.addCleanup(case.worker.close)
    return case.worker


def instance(case: NativeAcceptance) -> str:
    return case.success("automation.capabilities", {})["worker"]["instance_id"]


def status(case: NativeAcceptance, operation: str) -> dict[str, Any]:
    return case.success("operation.status", {"operation_id": operation})


def created_scene(case: NativeAcceptance) -> tuple[dict[str, Any], dict[str, Any]]:
    """Create a scene and return its request and result."""
    params = case.write_params(None, SCENE)
    result = case.success("scene.create", params)
    case.revisions[result["document_id"]] = result["revision"]
    return params, result


@acceptance_case
def receipts_survive_restart(case: NativeAcceptance) -> None:
    """An exact retry in a new worker returns the recorded result with a receipt, never runs
    again, and the old worker's document id is reported as another worker's."""
    first = instance(case)
    params, created = created_scene(case)
    document = created["document_id"]
    case.assertTrue(document.startswith(f"document_{first[:8]}_"), document)
    restart(case, "restarted")
    replay = case.success("scene.create", params)
    receipt = replay.pop("receipt")
    case.assertEqual(replay, created)
    case.assertEqual(
        (receipt["source"], receipt["recorded_by_worker"], receipt["handles_expired"]),
        ("journal", first, True),
    )
    case.assertIsInstance(receipt["recorded_at_ms"], int)
    gone = case.failure("scene.summary", {"document_id": document})
    case.assertEqual(gone["code"], "document_not_found")
    case.assertEqual(
        (gone["details"]["reason"], gone["details"]["worker_instance_id"]), ("other_worker", first)
    )
    case.assertIs(gone["details"]["worker_alive"], False)
    case.assertEqual(case.success("automation.list_documents", {})["documents"], [])
    answer = status(case, params["operation_id"])
    case.assertEqual((answer["state"], answer["retry"]), ("completed", "returns_recorded_result"))
    case.assertEqual(
        answer["recorded_by"], {"worker_instance_id": first, "current": False, "alive": False}
    )
    case.assertEqual((answer["method"], answer["outcome"]["revision"]), ("scene.create", "1"))
    case.assertIs(answer["authority_matches"], True)
    changed = case.failure("scene.create", {**params, "name": "Other"})
    case.assertEqual(changed["code"], "operation_conflict")
    journal = case.success("journal.status", {})
    case.assertEqual((journal["state"], journal["writers_total"]), ("ok", 2))
    case.assertEqual([row["alive"] for row in journal["writers"]], [True, False])
    build = case.success("automation.capabilities", {})["worker"]["build"]["build_digest"]
    starts = [row for row in records(case) if row["type"] == "worker_start"]
    case.assertEqual([row["build_digest"] for row in starts], [build, build])
    admits = [row for row in records(case) if row.get("operation_id") == params["operation_id"]]
    case.assertEqual([row["type"] for row in admits], ["admit", "outcome"])


@acceptance_case
def replay_under_other_grants_conflicts(case: NativeAcceptance) -> None:
    """A retry under a different launch authority is refused, not replayed."""
    params, _created = created_scene(case)
    restart(case, "narrower", grants="inspect,draft")
    error = case.failure("scene.create", params)
    case.assertEqual(
        (error["code"], error["details"]["reason"]), ("operation_conflict", "grant_changed")
    )
    answer = status(case, params["operation_id"])
    case.assertIs(answer["authority_matches"], False)
    case.assertEqual(answer["retry"], "returns_recorded_error")


@acceptance_case
def journal_paths_are_reserved(case: NativeAcceptance) -> None:
    """No tool reads or writes under .groundzero: the journal is the worker's own."""
    _params, created = created_scene(case)
    document = created["document_id"]
    reserved = case.workspace / ".groundzero" / "journal" / "copy"
    for directory in (str(reserved), ".groundzero/copy", ".GroundZero/copy"):
        error = case.failure(
            "scene.save_copy", case.write_params(document, {"directory": directory})
        )
        case.assertEqual((error["code"], error["details"]["reason"]), ("path_denied", "reserved"))
    error = case.failure(
        "scene.open", case.write_params(None, {"path": str(journal_directory(case) / "FORMAT")})
    )
    case.assertEqual((error["code"], error["details"]["reason"]), ("path_denied", "reserved"))
    case.assertFalse(reserved.exists())


@acceptance_case
def newer_journal_format_refuses_writes(case: NativeAcceptance) -> None:
    """A journal written by a newer build is read but never written by this one."""
    params, _created = created_scene(case)
    case.worker.close()
    (journal_directory(case) / "FORMAT").write_text(
        json.dumps({"format": "groundzero-journal", "version": 2}) + "\n"
    )
    restart(case, "older-build")
    journal = case.success("journal.status", {})
    case.assertEqual(
        (journal["available"], journal["state"], journal["unavailable_reason"]),
        (False, "unavailable", "newer_format"),
    )
    capabilities = case.success("automation.capabilities", {})["journal"]
    case.assertEqual((capabilities["available"], capabilities["state"]), (False, "unavailable"))
    error = case.failure("scene.create", case.write_params(None, SCENE))
    case.assertEqual(
        (error["code"], error["details"]["reason"]), ("journal_unavailable", "newer_format")
    )
    case.assertEqual(status(case, params["operation_id"])["state"], "completed")
    case.assertEqual(case.success("automation.list_documents", {})["documents"], [])


@acceptance_case
def damaged_segment_is_quarantined_and_reported(case: NativeAcceptance) -> None:
    """A damaged line is quarantined with its segment, reported, and writes go on; an id
    the damage may have hidden is unknown_damaged rather than not_found."""
    lost, _created = created_scene(case)
    kept, _second = created_scene(case)
    case.worker.close()
    segment = next(journal_directory(case).glob("seg-*.jsonl"))
    lines = segment.read_bytes().splitlines(keepends=True)
    index = next(
        position
        for position, line in enumerate(lines)
        if f'"operation_id":"{lost["operation_id"]}"'.encode() in line and b'"admit"' in line
    )
    lines[index] = lines[index].replace(b'"admit"', b'"admiT"')
    segment.write_bytes(b"".join(lines))
    restart(case, "after-damage")
    journal = case.success("journal.status", {})
    case.assertEqual(journal["state"], "damaged")
    case.assertEqual(len(journal["damage"]), 1)
    case.assertEqual(len(journal["last_recovery"]["quarantined_segments"]), 1)
    case.assertEqual(len(list((journal_directory(case) / "quarantine").iterdir())), 1)
    case.assertEqual(status(case, lost["operation_id"])["state"], "unknown_damaged")
    case.assertEqual(status(case, kept["operation_id"])["state"], "completed")
    created_scene(case)


def _held_admission(case: NativeAcceptance, params: dict[str, Any]) -> Worker:
    """Start a worker that holds its admission of params for ten seconds, and send it."""
    holder = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / "holder.stderr.log",
        test_arguments=(
            "--test-write-fault",
            "journal:admit:hold",
            "--test-fault-operation",
            params["operation_id"],
        ),
    )
    case.addCleanup(holder.close)
    sent = threading.Thread(target=_ignore_failure, args=(holder, params), daemon=True)
    sent.start()
    deadline = time.monotonic() + 10
    while status(case, params["operation_id"])["state"] != "in_progress":
        case.assertLess(time.monotonic(), deadline, "the holder never admitted the operation")
        time.sleep(0.05)
    return holder


def _ignore_failure(worker: Worker, params: dict[str, Any]) -> None:
    try:
        worker.call("scene.create", params)
    except (RuntimeError, TimeoutError):
        pass


@acceptance_case(worker_only=FAULT_FLAG)
def two_workers_one_writer_per_record(case: NativeAcceptance) -> None:
    """While one worker runs an operation another refuses it as in progress; once the first
    is killed the operation is interrupted, never run again, and listed as unresolved."""
    params = case.write_params(None, SCENE)
    holder = _held_admission(case, params)
    busy = case.failure("scene.create", params)
    case.assertEqual(busy["code"], "operation_in_progress")
    case.assertEqual(
        (busy["details"]["recorded_by"]["alive"], busy["details"]["recorded_by"]["current"]),
        (True, False),
    )
    holder.process.kill()
    holder.process.wait(timeout=10)
    interrupted = case.failure("scene.create", params)
    case.assertEqual(interrupted["code"], "operation_interrupted")
    case.assertEqual(interrupted["details"]["verdict"]["kind"], "lost_with_worker")
    answer = status(case, params["operation_id"])
    case.assertEqual((answer["state"], answer["retry"]), ("interrupted", "refused_interrupted"))
    case.assertIs(answer["verdict"]["reconciled"], True)
    unresolved = case.success("operation.list", {})["operations"]
    case.assertEqual([row["operation_id"] for row in unresolved], [params["operation_id"]])
    case.assertEqual(unresolved[0]["verdict_kind"], "lost_with_worker")
    case.assertEqual(case.success("automation.list_documents", {})["documents"], [])
    mine = [row for row in records(case) if row.get("operation_id") == params["operation_id"]]
    case.assertEqual([row["type"] for row in mine], ["admit", "reconcile"])
    case.assertNotEqual(mine[0]["w"], mine[1]["w"])


@acceptance_case(worker_only=FAULT_FLAG)
def journal_lock_timeout_admits_nothing(case: NativeAcceptance) -> None:
    """When the append lock is not obtained in time the write is refused before anything."""
    restart(case, "locked", "--test-write-fault", "journal:lock")
    params = case.write_params(None, SCENE)
    error = case.failure("scene.create", params)
    case.assertEqual(error["code"], "journal_busy")
    case.assertEqual(case.success("automation.list_documents", {})["documents"], [])
    case.assertEqual(status(case, params["operation_id"])["state"], "not_found")
    case.assertIs(case.success("journal.status", {})["available"], True)


@acceptance_case(worker_only=FAULT_FLAG)
def outcome_flush_failure_returns_result_and_stops_writes(case: NativeAcceptance) -> None:
    """A change whose outcome cannot be flushed is still returned; the journal then refuses
    every later write of this worker, and the next worker reconciles the operation."""
    params = case.write_params(None, SCENE)
    flags = (
        "--test-write-fault",
        "journal:flush",
        "--test-fault-operation",
        params["operation_id"],
    )
    restart(case, "flush", *flags)
    created = case.success("scene.create", params)
    case.assertEqual(case.success("scene.create", params), created)
    journal = case.success("journal.status", {})
    case.assertEqual((journal["available"], journal["unavailable_reason"]), (False, "flush_failed"))
    refused = case.failure("scene.create", case.write_params(None, SCENE))
    case.assertEqual(refused["code"], "journal_unavailable")
    restart(case, "after-flush")
    case.assertEqual(status(case, params["operation_id"])["state"], "interrupted")


@acceptance_case
def job_retry_after_restart_returns_recorded_job(case: NativeAcceptance) -> None:
    """A retried job start in a new worker returns the recorded job, starts no render, and
    the job is answered from the journal."""
    _params, created = created_scene(case)
    document = created["document_id"]
    preview = case.write_params(document, {"frame": 0, "width": 8, "height": 8})
    job = case.success("render.preview", preview)
    case.assertEqual(case.wait_job(job["job_id"])["status"], "completed")
    restart(case, "after-job")
    renders = case.workspace / "renders"
    before = set(renders.iterdir()) if renders.exists() else set()
    replay = case.success("render.preview", preview)
    case.assertEqual(replay.pop("receipt")["handles_expired"], True)
    case.assertEqual(replay["job_id"], job["job_id"])
    after = set(renders.iterdir()) if renders.exists() else set()
    case.assertLessEqual(after, before, "the retry started a render")
    recorded = case.success("job.status", {"job_id": job["job_id"]})
    case.assertEqual((recorded["status"], recorded["observed_by"]), ("completed", "journal"))
    listed = case.success("job.list", {})["jobs"]
    case.assertEqual(
        [(row["job_id"], row["observed_by"]) for row in listed], [(job["job_id"], "journal")]
    )
    unavailable = case.failure("job.result", {"job_id": job["job_id"]})
    case.assertEqual(
        (unavailable["code"], unavailable["details"]["reason"]),
        ("artifacts_unavailable", "previous_worker"),
    )


# Crash points, the call crashed there, then what a later worker reports for it and what
# an exact retry returns (None: the recorded result).
CRASHES = (
    ("journal:before-admit", "scene.settings", "not_found", None, "document_not_found"),
    ("journal:admit", "scene.create", "interrupted", "lost_with_worker", "operation_interrupted"),
    (
        "journal:handler",
        "scene.settings",
        "interrupted",
        "lost_with_worker",
        "operation_interrupted",
    ),
    ("package:level", "scene.save_copy", "interrupted", "not_published", "operation_interrupted"),
    ("journal:intent", "scene.save_copy", "interrupted", "not_published", "operation_interrupted"),
    ("publish:renamed", "scene.save_copy", "interrupted", "published", "operation_interrupted"),
    ("journal:outcome", "scene.save_copy", "completed", None, None),
    ("journal:torn", "scene.settings", "not_found", None, "document_not_found"),
)
FAULT_OPERATION = "crashed-operation"


def _crashed_request(case: NativeAcceptance, point: str, tool: str) -> dict[str, Any]:
    """Start a worker armed to crash at point in one write, build a scene, and send it."""
    _fault_worker(
        case,
        point.replace(":", "-"),
        "--test-write-fault",
        f"{point}:crash",
        "--test-fault-operation",
        FAULT_OPERATION,
    )
    document, _level, _column = case.populated_scene()
    arguments = {
        "scene.create": dict(SCENE),
        "scene.settings": {"width_px": 64},
        "scene.save_copy": {"directory": str(case.workspace / "package")},
    }[tool]
    request = case.write_params(None if tool == "scene.create" else document, arguments)
    request["operation_id"] = FAULT_OPERATION
    with case.assertRaises(RuntimeError):
        case.worker.call(tool, request)
    case.assertEqual(case.worker.process.wait(timeout=30), CRASH_EXIT_CODE)
    return request


def _assert_publication(case: NativeAcceptance, verdict: dict[str, Any] | None) -> None:
    target = case.workspace / "package"
    published = verdict is not None and verdict["kind"] == "published"
    case.assertEqual(target.exists(), published or verdict is None)
    case.assertEqual(staging_left(case.workspace), [])
    if verdict is not None and verdict["evidence"]["target"] is not None:
        found = "matches" if published else "absent"
        case.assertEqual(
            (verdict["evidence"]["target"], verdict["evidence"]["found"]), ("package", found)
        )


def _assert_after_crash(
    case: NativeAcceptance, row: tuple[Any, ...], request: dict[str, Any]
) -> None:
    point, tool, state, kind, retry = row
    answer = status(case, FAULT_OPERATION)
    case.assertEqual(answer["state"], state, answer)
    verdict = answer["verdict"]
    case.assertEqual(verdict and verdict["kind"], kind)
    if tool == "scene.save_copy":
        _assert_publication(case, verdict)
    unresolved = [row["operation_id"] for row in case.success("operation.list", {})["operations"]]
    case.assertEqual(unresolved, [FAULT_OPERATION] if state == "interrupted" else [])
    if retry is None:
        case.assertEqual(case.success(tool, request)["receipt"]["handles_expired"], True)
    else:
        case.assertEqual(case.failure(tool, request)["code"], retry)
    if point == "journal:torn":
        recovery = case.success("journal.status", {})["last_recovery"]
        case.assertGreater(recovery["truncated_tail_bytes"], 0)
        case.assertTrue(list((journal_directory(case) / "quarantine").glob("tail-*")))


@acceptance_case(worker_only=FAULT_FLAG)
def crash_windows_are_reconciled_never_rerun(case: NativeAcceptance) -> None:
    """A worker killed at each crash window leaves a journal from which the next worker tells
    what happened, and an exact retry never runs the operation a second time."""
    for row in CRASHES:
        with case.subTest(point=row[0]):
            request = _crashed_request(case, row[0], row[1])
            restart(case, row[0].replace(":", "-") + "-successor")
            _assert_after_crash(case, row, request)


@acceptance_case(worker_only=FAULT_FLAG)
def journal_on_an_unsupported_volume_refuses_writes(case: NativeAcceptance) -> None:
    """Where the journal cannot be kept every write is refused; reads still answer."""
    restart(case, "network-volume", "--test-write-fault", "journal:volume")
    journal = case.success("journal.status", {})
    case.assertEqual(
        (journal["available"], journal["unavailable_reason"]), (False, "unsupported_volume")
    )
    error = case.failure("scene.create", case.write_params(None, SCENE))
    case.assertEqual(
        (error["code"], error["details"]["reason"]), ("journal_unavailable", "unsupported_volume")
    )
    case.assertEqual(case.success("automation.list_documents", {})["documents"], [])
    refused = case.failure("operation.status", {"operation_id": "operation-1"})
    case.assertEqual(refused["code"], "journal_unavailable")


@acceptance_case(worker_only="needs --test-journal-limits, which the adapter never passes")
def old_receipts_become_stubs_and_expire(case: NativeAcceptance) -> None:
    """Past the retention limit an old settled operation keeps only a stub: its retry is
    receipt_expired and never runs, while recent ones still return their results."""
    small = ("--test-journal-limits", "small")
    restart(case, "small", *small)
    oldest, _created = created_scene(case)
    document = _created["document_id"]
    recent = {}
    # About 1 KiB a write: two 64 KiB segments, 16 full receipts kept, the rest stubs.
    for _index in range(70):
        recent = case.write_params(document, {"width_px": 32 + _index % 2})
        case.revisions[document] = case.success("scene.settings", recent)["revision"]
    restart(case, "compacting", *small)
    expired = case.failure("scene.create", oldest)
    case.assertEqual(expired["code"], "receipt_expired")
    case.assertEqual(
        (expired["details"]["state"], expired["details"]["method"]), ("completed", "scene.create")
    )
    answer = status(case, oldest["operation_id"])
    case.assertEqual((answer["state"], answer["retry"]), ("expired", "refused_expired"))
    case.assertEqual(case.success("scene.settings", recent)["receipt"]["handles_expired"], True)
    journal = case.success("journal.status", {})
    case.assertGreater(journal["retained"]["receipt_stubs"], 0)
    case.assertLessEqual(journal["retained"]["full_receipts"], 16 + 64)
    case.assertGreater(journal["segments"], 1)


@acceptance_case(worker_only=FAULT_FLAG)
def job_outcome_lost_with_its_worker_is_unknown(case: NativeAcceptance) -> None:
    """A worker that dies after its job ended but before recording the end leaves the job
    unknown to the next worker, while the job start itself stays a recorded result."""
    restart(case, "job-terminal", "--test-write-fault", "journal:job-terminal:crash")
    _params, created = created_scene(case)
    preview = case.write_params(created["document_id"], {"frame": 0, "width": 8, "height": 8})
    job = case.success("render.preview", preview)["job_id"]
    case.assertEqual(case.worker.process.wait(timeout=30), CRASH_EXIT_CODE)
    restart(case, "after-job-terminal")
    lost = case.success("job.status", {"job_id": job})
    case.assertEqual((lost["status"], lost["observed_by"]), ("unknown", "journal"))
    case.assertTrue(lost["terminal"])
    case.assertEqual(lost["worker_loss"]["cause"], "worker_exit_unobserved")
    case.assertIsInstance(lost["worker_loss"]["outputs_present"], bool)
    started = status(case, preview["operation_id"])
    case.assertEqual((started["state"], started["outcome"]["job_id"]), ("completed", job))
    case.assertEqual(case.success("render.preview", preview)["job_id"], job)
    case.assertEqual(case.success("journal.status", {})["last_recovery"]["reconciled_jobs"], 1)


def _create_many(case: NativeAcceptance, worker: Worker, prefix: str, failures: list[str]) -> None:
    for index in range(40):
        response = worker.call("scene.create", {**SCENE, "operation_id": f"{prefix}-{index}"})
        if "result" not in response:
            failures.append(f"{prefix}-{index}: {response}")
            continue
        discard = {"operation_id": f"{prefix}-discard-{index}"}
        discard.update(document_id=response["result"]["document_id"], expected_revision="1")
        worker.call("scene.discard", discard)


@acceptance_case
def concurrent_workers_append_whole_records(case: NativeAcceptance) -> None:
    """Two workers writing to one workspace at once append whole, verified records under one
    strictly increasing sequence, and every operation gets exactly one outcome."""
    other = Worker(case.executable, case.workspace, case.inputs, case.directory / "other.log")
    case.addCleanup(other.close)
    for worker in (case.worker, other):
        worker.call("automation.list_documents", {})
    failures: list[str] = []
    threads = [
        threading.Thread(target=_create_many, args=(case, worker, prefix, failures))
        for worker, prefix in ((case.worker, "first"), (other, "second"))
    ]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=120)
    case.assertEqual(failures, [])
    written = [row for row in records(case) if row["type"] != "segment"]
    sequence = [row["seq"] for row in written]
    case.assertEqual(sequence, list(range(1, len(sequence) + 1)))
    outcomes: dict[str, int] = {}
    for row in written:
        if row["type"] == "outcome":
            outcomes[row["operation_id"]] = outcomes.get(row["operation_id"], 0) + 1
    admitted = {row["operation_id"] for row in written if row["type"] == "admit"}
    case.assertEqual(len(admitted), 160)
    case.assertEqual(outcomes, dict.fromkeys(admitted, 1))
    case.assertEqual(len({row["w"] for row in written if row["type"] == "admit"}), 2)


def _terminal_job_records(case: NativeAcceptance, job: str) -> list[dict[str, Any]]:
    return [
        row
        for row in records(case)
        if row["type"] == "job" and row["job_id"] == job and row["record"].get("terminal")
    ]


@acceptance_case
def scene_host_job_end_is_recorded(case: NativeAcceptance) -> None:
    """A job run in a read-only document's scene host gets its terminal record once the
    worker observes its end, so a later worker reports how it ended, not unknown."""
    scene = corpus(case)["B"]
    document = open_scene(case, case.worker, scene, "standalone")["document_id"]
    params = {"document_id": document, "expected_revision": "0", "frame": 0, "width": 16}
    params.update(height=16, operation_id=f"host-preview-{case.operations}")
    job = case.success("render.preview", params)["job_id"]
    case.assertEqual(wait(case, case.worker, job)["status"], "completed")
    deadline = time.monotonic() + 10
    while not _terminal_job_records(case, job):
        case.assertLess(time.monotonic(), deadline, "the host job's end was never recorded")
        time.sleep(0.1)
    restart(case, "after-host-job")
    ended = case.success("job.status", {"job_id": job})
    case.assertEqual((ended["status"], ended["observed_by"]), ("completed", "journal"))


@acceptance_case(worker_only="needs --test-journal-limits, which the adapter never passes")
def retention_runs_between_requests(case: NativeAcceptance) -> None:
    """A long-running worker keeps its journal within its limits between requests, one
    sealed segment at a time, without a restart."""
    restart(case, "small-running", "--test-journal-limits", "small")
    first, created = created_scene(case)
    document = created["document_id"]
    # About 1 KiB a write: a sealed segment; 16 full receipts kept, 55 stubs.
    for index in range(70):
        params = case.write_params(document, {"width_px": 32 + index % 2})
        case.revisions[document] = case.success("scene.settings", params)["revision"]
    deadline = time.monotonic() + 15
    while status(case, first["operation_id"])["state"] != "expired":
        case.assertLess(time.monotonic(), deadline, "no retention step ran")
        time.sleep(0.2)
    journal = case.success("journal.status", {})
    case.assertGreater(journal["retained"]["receipt_stubs"], 0)
    case.assertLess(journal["retained"]["full_receipts"], 71)
    case.assertEqual(case.success("scene.create", first)["document_id"], document)


@acceptance_case(worker_only="needs --test-write-fault and --test-journal-limits")
def crash_during_compaction_loses_nothing(case: NativeAcceptance) -> None:
    """A worker killed while compacting leaves the old segment or the new one, never
    neither: every operation still answers as before."""
    small = ("--test-journal-limits", "small")
    # Normal limits while filling, so no retention step runs before the crash.
    restart(case, "fill")
    first, created = created_scene(case)
    document = created["document_id"]
    last = first
    for index in range(70):
        last = case.write_params(document, {"width_px": 32 + index % 2})
        case.revisions[document] = case.success("scene.settings", last)["revision"]
    case.worker.close()
    before = sorted(path.name for path in journal_directory(case).glob("seg-*.jsonl"))
    crashed = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / "compaction-crash.stderr.log",
        test_arguments=(*small, "--test-write-fault", "journal:compaction:crash"),
    )
    case.addCleanup(crashed.close)
    case.assertEqual(crashed.process.wait(timeout=60), CRASH_EXIT_CODE)
    # The crash came at the step that renames the compacted copy over the
    # segment: every earlier segment is still there.
    after = sorted(path.name for path in journal_directory(case).glob("seg-*.jsonl"))
    case.assertLessEqual(set(before), set(after))
    restart(case, "after-compaction-crash", *small)
    case.assertEqual(status(case, last["operation_id"])["state"], "completed")
    case.assertIn(status(case, first["operation_id"])["state"], {"completed", "expired"})
    case.assertEqual(case.success("journal.status", {})["state"], "ok")


@acceptance_case(worker_only=FAULT_FLAG)
def recipe_resume_lists_and_settles_unresolved(case: NativeAcceptance) -> None:
    """A recipe whose worker died mid-way is resumed by listing its unresolved writes and
    settling each: the interrupted save is found published, nothing runs twice and there is
    one package per save. Steps are inline; move onto mcp_recipe_run.py at integration."""
    saved = "recipe-save-b"
    restart(
        case,
        "recipe",
        "--test-write-fault",
        "publish:renamed:crash",
        "--test-fault-operation",
        saved,
    )
    document, _level, _column = case.populated_scene()
    first = case.mutate("scene.save_copy", document, directory=str(case.workspace / "save-a"))
    case.mutate("scene.settings", document, frame_rate=12)
    second = case.write_params(document, {"directory": str(case.workspace / "save-b")})
    second["operation_id"] = saved
    with case.assertRaises(RuntimeError):
        case.worker.call("scene.save_copy", second)
    case.assertEqual(case.worker.process.wait(timeout=30), CRASH_EXIT_CODE)
    restart(case, "resumed")
    unresolved = case.success("operation.list", {})["operations"]
    case.assertEqual([row["operation_id"] for row in unresolved], [saved])
    settled = status(case, saved)
    case.assertEqual(settled["verdict"]["kind"], "published")
    case.assertEqual(settled["verdict"]["evidence"]["target"], "save-b")
    case.assertEqual(case.failure("scene.save_copy", second)["code"], "operation_interrupted")
    packages = sorted(path.name for path in case.workspace.glob("save-*"))
    case.assertEqual(packages, ["save-a", "save-b"])
    case.assertEqual(first["package_directory"], str(case.workspace / "save-a"))
    case.assertEqual(case.success("operation.list", {})["operations"], unresolved)
    # The reconciler registered the published package, so the recipe goes on from it.
    resumed = case.mutate("scene.open", None, path=str(case.workspace / "save-b"))
    case.assertEqual(resumed["admission"], "workspace_registry")
