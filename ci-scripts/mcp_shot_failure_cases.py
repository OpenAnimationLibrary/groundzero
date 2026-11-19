"""Failure injection at the level of the first shot: named failures with typed ends.

The shot is the first-shot journey (`mcp_shot.py`) in miniature: a harbor and a courier on a peg,
a sky gradient, a glow and two overs, built with the same tools in the same order. Each case
names a failure, injects it, and requires what an artist's shot needs: a typed error, nothing
published, the draft as it was, and a picture that an independent oracle (`check_compact_frame`)
still accepts after the shot is resumed. A shot that is structurally valid and renders without
error but looks wrong must fail the oracle, so the oracle's own sensitivity is a case too.
"""

from __future__ import annotations

import hashlib
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_failure_cases import SEQUENCE, _cancel, _ends, _fault_worker
from mcp_file_publish_cases import staging_left
from mcp_journal_cases import CRASH_EXIT_CODE, FAULT_FLAG, restart, status
from mcp_shot import (
    COMPACT_COURIER_TRAVEL,
    COMPACT_FRAMES,
    COMPACT_SIZE,
    DPI,
    Shot,
    check_compact_frame,
    compact_shot,
    set_motion,
    set_parameters,
)

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

PROBE_FRAMES = (0, 5, COMPACT_FRAMES - 1)
SHOT_SEQUENCE = {**SEQUENCE, "width": COMPACT_SIZE[0], "height": COMPACT_SIZE[1]}


def new_shot(case: NativeAcceptance) -> tuple[Shot, str]:
    """A shot client on the case's current worker, whose operation ids no earlier one used.

    Operation ids are kept by the workspace's journal across workers, so a client started after
    a restart must not reuse one.
    """
    (case.directory / "previews").mkdir(exist_ok=True)
    shot = Shot(case.worker, case.directory)
    shot.workspace = case.workspace
    case.operations += 1
    builds = case.operations
    shot.operations = builds * 10_000
    return shot, str(builds)


def build(case: NativeAcceptance) -> tuple[Shot, dict[str, Any]]:
    """Build the compact shot on the case's worker."""
    shot, tag = new_shot(case)
    return shot, compact_shot(shot, tag)


def accepted(shot: Shot, document: str, label: str) -> None:
    """Render the probe frames of a shot and require the oracle to accept each."""
    for frame in PROBE_FRAMES:
        pixels = shot.preview(document, f"{label}-{frame}", frame, size=COMPACT_SIZE)
        check_compact_frame(pixels, frame)


def picture_digest(shot: Shot, document: str, label: str) -> str:
    """A digest of the frame-5 render, to show that a refused edit changed nothing."""
    pixels = shot.preview(document, label, 5, size=COMPACT_SIZE)
    return hashlib.sha256(repr(pixels).encode()).hexdigest()


def graph_of(shot: Shot, document: str) -> dict[str, Any]:
    """The FX graph, as the worker reports it."""
    return shot.read("fx.graph", {"document_id": document})


def refusal(shot: Shot, method: str, document: str | None, **params: Any) -> dict[str, Any]:
    """Send a write that must be refused and return its error."""
    shot.operations += 1
    request = {**params, "operation_id": f"shot-{shot.operations}"}
    if document is not None:
        request.update(document_id=document, expected_revision=shot.revisions[document])
    response = shot.worker.call(method, request)
    if "error" not in response:
        raise AssertionError(f"{method} was expected to be refused: {response}")
    return response["error"]


def wrong_pictures(shot: Shot, handles: dict[str, Any]) -> dict[str, Any]:
    """Valid edits that leave a shot renderable and wrong, each with what the oracle says."""
    document = handles["document"]
    first, last = COMPACT_COURIER_TRAVEL
    return {
        "the sky drawn over the harbor": lambda: shot.write(
            "fx.connect",
            document,
            fx_id=handles["environment"],
            port="Source1",
            source_fx_id=handles["sky"],
        ),
        "the courier dropped from the final over": lambda: shot.write(
            "fx.connect",
            document,
            fx_id=handles["final"],
            port="Source1",
            source_fx_id=handles["environment"],
        ),
        "the courier's flight key moved": lambda: set_motion(
            shot,
            document,
            handles["peg"],
            {"x": ("inch", [(0, first / DPI), (COMPACT_FRAMES - 1, first / DPI)])},
        ),
        "the sky upside down": lambda: set_parameters(
            shot, document, handles["sky"], {"startPoint": [0, -27], "endPoint": [0, 27]}
        ),
    }


@acceptance_case
def shot_that_renders_wrongly_fails_the_independent_oracle(case: NativeAcceptance) -> None:
    """A valid, renderable shot with a wrong picture is refused by the oracle, the right one not."""
    shot, handles = build(case)
    accepted(shot, handles["document"], "right")
    for name in wrong_pictures(shot, handles):
        with case.subTest(wrong=name):
            fresh, wrong = build(case)
            wrong_pictures(fresh, wrong)[name]()
            with case.assertRaises(AssertionError) as caught:
                accepted(fresh, wrong["document"], "wrong")
            case.assertRegex(str(caught.exception), r"Frame \d+")


def unchanged(shot: Shot, handles: dict[str, Any], before: dict[str, Any], label: str) -> None:
    """The draft is what it was: same graph, same revision, same picture."""
    document = handles["document"]
    case_graph = graph_of(shot, document)
    require_equal(case_graph, before["graph"], f"{label}: the graph changed")
    revision = shot.read("scene.summary", {"document_id": document})["revision"]
    require_equal(revision, before["revision"], f"{label}: the revision changed")
    require_equal(picture_digest(shot, document, label), before["picture"], f"{label}: the picture")


def require_equal(actual: object, expected: object, message: str) -> None:
    if actual != expected:
        raise AssertionError(message)


def snapshot(shot: Shot, handles: dict[str, Any]) -> dict[str, Any]:
    """What a refused edit must leave alone."""
    document = handles["document"]
    return {
        "graph": graph_of(shot, document),
        "revision": shot.read("scene.summary", {"document_id": document})["revision"],
        "picture": picture_digest(shot, document, "before"),
    }


def tree_digest(directory: Path) -> str:
    """A digest of every file under a directory, by relative name and content."""
    digest = hashlib.sha256()
    for path in sorted(directory.rglob("*")):
        digest.update(str(path.relative_to(directory)).encode())
        if path.is_file():
            digest.update(path.read_bytes())
    return digest.hexdigest()


@acceptance_case
def shot_edit_failures_end_typed_and_change_nothing(case: NativeAcceptance) -> None:
    """A bad port, a cycle, a stale draft, a discarded draft and a missing asset each end in a
    typed error that leaves the graph, the revision and the picture as they were."""
    shot, handles = build(case)
    document = handles["document"]
    before = snapshot(shot, handles)
    refusals = {
        "a port the effect does not have": (
            "fx.connect",
            {"fx_id": handles["final"], "port": "Nope", "source_fx_id": handles["sky"]},
            "UNKNOWN_PORT",
        ),
        "a cycle through the final over": (
            "fx.connect",
            {"fx_id": handles["environment"], "port": "Source2", "source_fx_id": handles["final"]},
            "GRAPH_CYCLE",
        ),
        "an effect connected to itself": (
            "fx.connect",
            {"fx_id": handles["final"], "port": "Source1", "source_fx_id": handles["final"]},
            "GRAPH_CYCLE",
        ),
        "an asset that is not there": (
            "level.import",
            {"path": str(case.inputs / "nowhere.png"), "name": "Ghost", "dpi": 128},
            "invalid_asset",
        ),
    }
    for name, (method, params, code) in refusals.items():
        with case.subTest(failure=name):
            error = refusal(shot, method, document, **params)
            case.assertEqual(error["code"], code, error)
            unchanged(shot, handles, before, name.split()[-1])
    with case.subTest(failure="a stale draft"):
        current = shot.revisions[document]
        shot.write("scene.settings", document, frame_rate=25)
        later = shot.revisions[document]
        shot.revisions[document] = current
        error = refusal(shot, "scene.settings", document, frame_rate=26)
        case.assertEqual(error["code"], "revision_conflict", error)
        shot.revisions[document] = later
        case.assertEqual(shot.read("scene.summary", {"document_id": document})["frame_rate"], 25)
    with case.subTest(failure="a discarded draft"):
        shot.write("scene.discard", document)
        parameter = {"fx_id": handles["sky"], "parameter": "startPoint", "value": [0, 1]}
        shot.revisions[document] = "1"
        error = refusal(shot, "fx.parameter.set", document, **parameter)
        case.assertEqual(error["code"], "document_not_found", error)


@acceptance_case
def shot_package_failures_publish_nothing_and_keep_what_exists(case: NativeAcceptance) -> None:
    """Saving over an existing destination is refused and leaves it byte for byte; a package
    that lost an asset or whose asset changed is refused on reopen and leaves no draft."""
    shot, handles = build(case)
    document = handles["document"]
    saved = shot.write("scene.save_copy", document, directory=str(case.workspace / "kept"))
    package = Path(saved["package_directory"])
    kept = tree_digest(package)
    error = refusal(shot, "scene.save_copy", document, directory=str(package))
    case.assertEqual(error["code"], "already_exists", error)
    case.assertEqual(tree_digest(package), kept)
    case.assertEqual(staging_left(case.workspace), [])
    asset = sorted((package / "assets").glob("*"))[0]
    original = asset.read_bytes()
    asset.write_bytes(original + b"\0")
    changed = refusal(shot, "scene.open", None, path=str(package))
    case.assertIn(changed["code"], {"invalid_package", "package_changed"}, changed)
    asset.unlink()
    missing = refusal(shot, "scene.open", None, path=str(package))
    case.assertEqual(missing["code"], "invalid_package", missing)
    asset.write_bytes(original)
    case.assertEqual(tree_digest(package), kept)
    reopened = shot.write("scene.open", None, path=str(package))
    accepted(shot, reopened["document_id"], "reopened")


def resumed(case: NativeAcceptance, package: Path, label: str) -> None:
    """Open the saved shot on the case's current worker and require the oracle's acceptance."""
    shot, _tag = new_shot(case)
    reopened = shot.write("scene.open", None, path=str(package))
    accepted(shot, reopened["document_id"], label)


def held_render(case: NativeAcceptance, flag: str, name: str) -> tuple[Path, str]:
    """A shot saved to a package, then a render of it held by a test flag; returns both."""
    _fault_worker(case, name, "--test-render-fault", flag)
    shot, handles = build(case)
    document = handles["document"]
    saved = shot.write("scene.save_copy", document, directory=str(case.workspace / "package"))
    job = shot.write("render.submit", document, **SHOT_SEQUENCE)["job_id"]
    return Path(saved["package_directory"]), job


def wait_for(case: NativeAcceptance, job: str, state: str) -> dict[str, Any]:
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        current = case.success("job.status", {"job_id": job})
        if current["status"] == state:
            return current
        time.sleep(0.05)
    case.fail(f"job {job} never reached {state}")
    raise AssertionError


def nothing_published(case: NativeAcceptance, job: str) -> None:
    error = case.worker.call("job.result", {"job_id": job})["error"]
    case.assertEqual(error["code"], "artifacts_unavailable", error)
    case.assertFalse((case.workspace / "renders" / job).exists())
    case.assertEqual(list(case.workspace.rglob("frame*.png")), [])


@acceptance_case(worker_only="needs --test-render-fault, which the adapter never passes")
def shot_render_canceled_during_finalization_publishes_nothing_and_resumes(
    case: NativeAcceptance,
) -> None:
    """Cancel the shot's render after its frames are written and before they are published:
    the job ends canceled with nothing published, and the saved shot resumes and renders right."""
    package, job = held_render(case, "hold-publication", "finalizing")
    held = wait_for(case, job, "finalizing")
    case.assertIs(held["writers_closed"], True)
    _cancel(case, job)
    _ends(case, job, "canceled")
    nothing_published(case, job)
    restart(case, "resume-after-cancel")
    resumed(case, package, "after-cancel")


@acceptance_case(worker_only="needs --test-render-fault, which the adapter never passes")
def shot_render_failing_after_its_last_frame_publishes_nothing_and_resumes(
    case: NativeAcceptance,
) -> None:
    """Fail the shot's render right after its final frame: failed with a reason, nothing
    published, and the saved shot resumes and renders right."""
    package, job = held_render(case, "fail-after-final-frame", "final")
    ended = _ends(case, job, "failed")
    case.assertTrue(ended["failure"]["message"])
    nothing_published(case, job)
    restart(case, "resume-after-failure")
    resumed(case, package, "after-failure")


@acceptance_case(worker_only="kills the worker and needs --test-render-fault")
def shot_worker_killed_mid_render_is_reported_unknown_and_the_shot_resumes(
    case: NativeAcceptance,
) -> None:
    """Kill the worker while the shot's render holds written frames: the next worker reports the
    job lost, adopts nothing, and the saved shot resumes and renders right."""
    package, job = held_render(case, "hold-publication", "killed")
    wait_for(case, job, "finalizing")
    left = case.workspace / "renders" / job
    case.assertTrue(any((left / "frames").glob("*.png")))
    case.worker.process.kill()
    case.worker.process.wait(timeout=10)
    restart(case, "resume-after-kill")
    lost = case.success("job.status", {"job_id": job})
    case.assertEqual((lost["status"], lost["observed_by"]), ("unknown", "journal"))
    case.assertEqual(lost["worker_loss"]["cause"], "worker_exit_unobserved")
    result = case.failure("job.result", {"job_id": job})
    case.assertEqual(result["details"]["reason"], "previous_worker")
    case.assertFalse(left.exists())
    case.assertEqual(list(case.workspace.rglob("frame*.png")), [])
    resumed(case, package, "after-kill")


def crashed_save(case: NativeAcceptance, point: str) -> tuple[Path, dict[str, Any]]:
    """Arm a crash at a window of the shot's save, make the call, and return the request."""
    operation = "crashed-shot-save"
    _fault_worker(
        case,
        point.replace(":", "-"),
        "--test-write-fault",
        f"{point}:crash",
        "--test-fault-operation",
        operation,
    )
    shot, handles = build(case)
    document = handles["document"]
    target = case.workspace / "package"
    request = {
        "directory": str(target),
        "operation_id": operation,
        "document_id": document,
        "expected_revision": shot.revisions[document],
    }
    with case.assertRaises(RuntimeError):
        case.worker.call("scene.save_copy", request)
    case.assertEqual(case.worker.process.wait(timeout=30), CRASH_EXIT_CODE)
    return target, request


# The windows of the shot's save: where the worker died, what the next one calls the operation,
# whether the package was published, and what an exact retry answers (None: the recorded result).
SAVE_WINDOWS = (
    ("package:level", "interrupted", "not_published", "operation_interrupted"),
    ("journal:intent", "interrupted", "not_published", "operation_interrupted"),
    ("publish:renamed", "interrupted", "published", "operation_interrupted"),
    ("journal:outcome", "completed", None, None),
)


@acceptance_case(worker_only=FAULT_FLAG)
def shot_save_windows_are_reconciled_and_the_shot_resumes(case: NativeAcceptance) -> None:
    """A worker killed in each window of the shot's save: the next worker says what happened,
    an exact retry never runs the save again, a package that exists is whole and reopens to a
    shot the oracle accepts, and one that does not exist is saved by a new operation."""
    for point, state, kind, retry in SAVE_WINDOWS:
        with case.subTest(window=point):
            target, request = crashed_save(case, point)
            restart(case, point.replace(":", "-") + "-successor")
            answer = status(case, request["operation_id"])
            case.assertEqual(answer["state"], state, answer)
            case.assertEqual(answer["verdict"] and answer["verdict"]["kind"], kind)
            case.assertEqual(staging_left(case.workspace), [])
            published = kind == "published" or kind is None
            case.assertEqual(target.exists(), published)
            if retry is None:
                case.assertTrue(case.success("scene.save_copy", request)["receipt"])
            else:
                case.assertEqual(case.failure("scene.save_copy", request)["code"], retry)
            if published:
                resumed(case, target, point.replace(":", "-"))
                continue
            case.assertFalse(target.exists(), "a retry ran the interrupted save again")
            gone = case.failure("scene.summary", {"document_id": request["document_id"]})
            case.assertEqual(gone["details"]["reason"], "other_worker", gone)
            case.assertEqual(case.success("automation.list_documents", {})["documents"], [])
