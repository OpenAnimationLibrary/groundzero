"""Delivery of render output into output roots: delivery.preflight and delivery.publish."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import subprocess
import threading
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_delivery_verify import verify
from mcp_journal_cases import restart, status
from mcp_output_root_cases import _flags, _granted, _roots
from mcp_write_fault_cases import CRASH_EXIT_CODE, FAULT_FLAG

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

ROOT = "deliveries"
SIZE = {"width": 32, "height": 32}
CHECKS = [
    "root",
    "path",
    "parent",
    "collision",
    "jobs",
    "artifacts",
    "filesystem",
    "space",
    "journal",
]
NOT_INCLUDED = [
    {"class": "render_snapshot", "reason": "private_to_job"},
    {"class": "scene_package", "reason": "publish_with_scene_save_copy"},
    {"class": "audio", "reason": "renders_are_video_only"},
    {"class": "job_records", "reason": "in_the_journal"},
]


def _render(case: NativeAcceptance, document: str, frames: int = 4, **extra: Any) -> str:
    """A completed render_sequence job of `frames` frames at 32x32."""
    job = case.mutate(
        "render.submit", document, start_frame=0, end_frame=frames - 1, **SIZE, **extra
    )["job_id"]
    case.assertEqual(case.wait_job(job)["status"], "completed")
    return job


def _items(*pairs: tuple[str, str]) -> list[dict[str, str]]:
    return [{"job_id": job, "folder": folder} for job, folder in pairs]


def _request(path: str, items: list[dict[str, str]], root: str = ROOT) -> dict[str, Any]:
    return {"destination": {"root": root, "path": path}, "items": items}


def _preflight(
    case: NativeAcceptance, path: str, items: list[dict[str, str]], **extra: Any
) -> dict[str, Any]:
    return case.success("delivery.preflight", {**_request(path, items), **extra})


def _publish_params(
    case: NativeAcceptance, path: str, items: list[dict[str, str]], root: str = ROOT
) -> dict[str, Any]:
    return case.write_params(None, _request(path, items, root))


def _states(result: dict[str, Any]) -> dict[str, tuple[str, str | None]]:
    return {check["name"]: (check["state"], check["code"]) for check in result["checks"]}


def _visible(directory: Path) -> list[str]:
    """Entries a test cares about: a volume's own hidden files are left out."""
    return sorted(
        name for name in os.listdir(directory) if not name.startswith(".") or "groundzero" in name
    )


def _cause(error: dict[str, Any]) -> str:
    return error["details"].get("cause_code", error["code"])


def _artifacts(case: NativeAcceptance, job: str) -> dict[str, dict[str, Any]]:
    """The job's verified artifacts by the file name they are published under."""
    rows = case.success("job.result", {"job_id": job})["artifacts"]
    named = {}
    for row in rows:
        extension = "exr" if row["kind"] == "exr" else "png"
        named[f"frame.{row['frame'] + 1:04d}.{extension}"] = row
    return named


def _tree_digest(directory: Path) -> str:
    """The journal's digest of a published tree: its sorted path, size and SHA-256 lines."""
    lines = sorted(
        f"{path.relative_to(directory).as_posix()}\0{path.stat().st_size}\0"
        + hashlib.sha256(path.read_bytes()).hexdigest()
        for path in directory.rglob("*")
        if path.is_file()
    )
    return hashlib.sha256("\n".join(lines).encode()).hexdigest()


def _shot(case: NativeAcceptance) -> tuple[str, str, str]:
    """A populated scene with a PNG and a half-float EXR sequence of it."""
    document, _level, _column = case.populated_scene()
    png = _render(case, document)
    exr = _render(case, document, frames=2, profile="rgba-half-exr")
    return document, png, exr


@acceptance_case
def delivery_publishes_verified_render_files(case: NativeAcceptance) -> None:
    """A preflight names every file with its size and digest and writes nothing; the
    publication copies exactly those bytes into a new directory with delivery.json, an
    independent verifier accepts it, and an exact retry returns the recorded result and
    writes nothing; the same name is never published twice."""
    root = _roots(case, ROOT)[ROOT]
    (root / "shots").mkdir()
    _granted(case, _flags({ROOT: root}))
    document, png, exr = _shot(case)
    items = _items((png, "frames"), (exr, "linear"))
    planned = _preflight(case, "shots/take1", items, limit=3)
    case.assertEqual((planned["ready"], planned["first_failure"]), (True, None))
    case.assertEqual([check["name"] for check in planned["checks"]], CHECKS)
    case.assertEqual((planned["files_total"], planned["next_offset"]), (6, 3))
    rest = _preflight(case, "shots/take1", items, offset=3)
    case.assertIsNone(rest["next_offset"])
    rows = planned["files"] + rest["files"]
    expected = {f"frames/{name}": row for name, row in _artifacts(case, png).items()}
    expected |= {f"linear/{name}": row for name, row in _artifacts(case, exr).items()}
    case.assertEqual([row["path"] for row in rows], list(expected))
    for row in rows:
        artifact = expected[row["path"]]
        case.assertEqual(
            (row["size_bytes"], row["sha256"]), (artifact["size_bytes"], artifact["sha256"])
        )
    case.assertEqual(planned["not_included"], NOT_INCLUDED)
    case.assertEqual(planned["plan"]["location"]["path"], "shots/take1")
    case.assertEqual(os.listdir(root / "shots"), [])
    params = _publish_params(case, "shots/take1", items)
    published = case.success("delivery.publish", params)
    case.assertEqual(
        (published["location"]["root"], published["location"]["path"], published["files"]),
        (ROOT, "shots/take1", 7),
    )
    delivery = root / "shots" / "take1"
    report = verify(delivery, published["manifest_sha256"])
    case.assertEqual(report["sha256"], {path: row["sha256"] for path, row in expected.items()})
    case.assertEqual(
        published["bytes"],
        sum(row["size_bytes"] for row in expected.values())
        + (delivery / "delivery.json").stat().st_size,
    )
    case.assertEqual(published["content_sha256"], _tree_digest(delivery))
    manifest = json.loads((delivery / "delivery.json").read_bytes())
    _require_manifest(case, manifest, params, document, png)
    case.assertEqual(_visible(root / "shots"), ["take1"])
    before = {path: path.stat().st_ino for path in delivery.rglob("*")}
    case.assertEqual(case.success("delivery.publish", params), published)
    case.assertEqual({path: path.stat().st_ino for path in delivery.rglob("*")}, before)
    changed = {**params, "items": items[:1]}
    case.assertEqual(case.failure("delivery.publish", changed)["code"], "operation_conflict")
    again = case.failure("delivery.publish", _publish_params(case, "shots/take1", items))
    case.assertEqual(again["code"], "already_exists")
    after = _preflight(case, "shots/take1", items)
    case.assertEqual(_states(after)["collision"], ("fail", "already_exists"))
    case.assertEqual(_visible(root / "shots"), ["take1"])


def _require_manifest(
    case: NativeAcceptance,
    manifest: dict[str, Any],
    params: dict[str, Any],
    document: str,
    png: str,
) -> None:
    """delivery.json names the publication, the producer and each job as recorded."""
    request = {key: value for key, value in params.items() if key != "operation_id"}
    canonical = json.dumps(request, sort_keys=True, separators=(",", ":"))
    case.assertEqual(
        manifest["publication"]["request_sha256"], hashlib.sha256(canonical.encode()).hexdigest()
    )
    case.assertEqual(manifest["publication"]["operation_id"], params["operation_id"])
    build = case.success("automation.capabilities", {})["worker"]["build"]
    case.assertEqual(manifest["producer"]["build_digest"], build["build_digest"])
    case.assertEqual(manifest["not_included"], NOT_INCLUDED)
    record = case.success("job.status", {"job_id": png})
    folder = manifest["folders"][0]
    case.assertEqual(
        (folder["job_id"], folder["document_id"], folder["source_revision"], folder["profile"]),
        (png, document, record["source_revision"], "rgba8-png"),
    )
    case.assertEqual(folder["frames"]["count"], 4)
    case.assertEqual(folder["conversions"], record["effective"]["conversions"])
    case.assertEqual(manifest["folders"][1]["profile"], "rgba-half-exr")


@acceptance_case
def delivery_refuses_jobs_it_cannot_publish(case: NativeAcceptance) -> None:
    """A discarded job says so, an unknown id and a job that is not a render are refused,
    folders and jobs are named once; the preflight reports the job check and nothing is
    written."""
    root = _roots(case, ROOT)[ROOT]
    _granted(case, _flags({ROOT: root}))
    document, _level, _column = case.populated_scene()
    kept, released = _render(case, document, frames=1), _render(case, document, frames=1)
    case.success("job.discard", case.write_params(None, {"job_id": released}))
    check = case.mutate("scene.check.submit", document, check="inventory")["job_id"]
    case.assertEqual(case.wait_job(check)["status"], "completed")
    refusals = {
        released: ("job_discarded", None),
        "job_00000000_00000000-0000-0000-0000-000000000000": ("unknown_job", "other_worker"),
        check: ("unsupported_target", "not_a_render_job"),
    }
    for job, (code, reason) in refusals.items():
        with case.subTest(job=job):
            items = _items((kept, "kept"), (job, "other"))
            planned = _preflight(case, "take", items)
            case.assertEqual(planned["first_failure"], "jobs")
            case.assertEqual(_states(planned)["jobs"], ("fail", code))
            error = case.failure("delivery.publish", _publish_params(case, "take", items))
            case.assertEqual((error["code"], error["details"].get("reason")), (code, reason))
    case.assertEqual(
        case.failure("delivery.publish", _publish_params(case, "take", _items((released, "a"))))[
            "details"
        ]["job_id"],
        released,
    )
    for items, reason in (
        (_items((kept, "same"), (check, "same")), "duplicate_folder"),
        (_items((kept, "a"), (kept, "b")), "duplicate_job"),
        (_items((kept, "delivery.json")), "reserved_folder"),
    ):
        error = case.failure("delivery.publish", _publish_params(case, "take", items))
        case.assertEqual((error["code"], error["details"]["reason"]), ("invalid_params", reason))
    case.assertEqual(os.listdir(root), [])


@acceptance_case
def delivery_paths_hold_the_output_root_boundary(case: NativeAcceptance) -> None:
    """Destinations refuse traversal, a link on the way, a missing parent, a root not
    granted and a root replaced by a link; nothing is written outside the root."""
    root = _roots(case, ROOT)[ROOT]
    outside = case.directory / "outside"
    outside.mkdir()
    (root / "linked").symlink_to(outside)
    _granted(case, _flags({ROOT: root}))
    document, _level, _column = case.populated_scene()
    items = _items((_render(case, document, frames=1), "frames"))
    for path in ("../escape", "a/../b", "/absolute", "a//b", ".groundzero-x"):
        with case.subTest(path=path):
            for tool in ("delivery.preflight", "delivery.publish"):
                params = _request(path, items)
                if tool == "delivery.publish":
                    params = _publish_params(case, path, items)
                error = case.failure(tool, params)
                case.assertEqual(error["code"], "invalid_path", (tool, error))
    planned = _preflight(case, "linked/take", items)
    case.assertEqual(_states(planned)["path"], ("fail", "path_denied"))
    error = case.failure("delivery.publish", _publish_params(case, "linked/take", items))
    case.assertEqual((error["code"], error["details"]["check"]), ("path_denied", "path"))
    planned = _preflight(case, "absent/take", items)
    case.assertEqual(_states(planned)["parent"], ("fail", "missing_parent"))
    error = case.failure("delivery.publish", _publish_params(case, "absent/take", items))
    case.assertEqual(error["code"], "missing_parent")
    for tool, params in (
        ("delivery.preflight", _request("take", items, "absent")),
        ("delivery.publish", _publish_params(case, "take", items, "absent")),
    ):
        case.assertEqual(case.failure(tool, params)["code"], "root_unavailable")
    moved = case.directory / "moved-root"
    root.rename(moved)
    root.symlink_to(moved)
    try:
        planned = _preflight(case, "take", items)
        case.assertEqual(_states(planned)["root"], ("fail", "root_unavailable"))
        error = case.failure("delivery.publish", _publish_params(case, "take", items))
        case.assertEqual(error["code"], "root_unavailable")
    finally:
        root.unlink()
        moved.rename(root)
    case.assertEqual(os.listdir(outside), [])
    case.assertEqual(os.listdir(root), ["linked"])


@acceptance_case(worker_only=FAULT_FLAG)
def delivery_parent_swapped_during_copy_is_not_followed(case: NativeAcceptance) -> None:
    """A parent moved and replaced by a link while a delivery is copied is not followed: the
    delivery is published where the pinned parent now lies, and the result names that
    place in its stored spelling."""
    root = _roots(case, ROOT)[ROOT]
    (root / "shots").mkdir()
    outside = case.directory / "outside"
    outside.mkdir()
    _granted(case, _flags({ROOT: root}) + ("--test-write-fault", "journal:intent:hold"))
    document, _level, _column = case.populated_scene()
    items = _items((_render(case, document, frames=2), "frames"))
    params = _publish_params(case, "shots/take", items)
    answer: dict[str, Any] = {}
    publishing = threading.Thread(
        target=lambda: answer.update(case.worker.call("delivery.publish", params))
    )
    publishing.start()
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline and not [
        name for name in os.listdir(root / "shots") if name.startswith(".groundzero-staging-")
    ]:
        time.sleep(0.05)
    (root / "shots").rename(root / "moved")
    (root / "shots").symlink_to(outside)
    publishing.join(timeout=60)
    case.assertEqual(answer["result"]["location"]["path"], "moved/take", answer)
    case.assertEqual(os.listdir(outside), [])
    verify(root / "moved" / "take", answer["result"]["manifest_sha256"])


FAULT_OPERATION = "delivery-under-test"


_FAULTED = {"operation": FAULT_OPERATION}


def _fault_render(case: NativeAcceptance, name: str, *flags: str) -> tuple[Path, list[dict]]:
    """A worker with one output root and a test fault that fires only in the publication
    named FAULT_OPERATION, and a 3-frame render to publish."""
    root = case.directory / f"out-{name}"
    root.mkdir()
    # One workspace journal serves every worker of a case, so each one's faulted
    # publication has its own operation id.
    _FAULTED["operation"] = f"{FAULT_OPERATION}-{name}"
    fault = ("--test-fault-operation", _FAULTED["operation"])
    restart(case, name, "--output-root", f"{ROOT}={root.resolve()}", *flags, *fault)
    document, _level, _column = case.populated_scene()
    return root.resolve(), _items((_render(case, document, frames=3), "frames"))


def _fault_params(case: NativeAcceptance, items: list[dict[str, str]]) -> dict[str, Any]:
    return {**_publish_params(case, "take", items), "operation_id": _FAULTED["operation"]}


def _require_progress(
    case: NativeAcceptance, error: dict[str, Any], copied: int, failed: str | None
) -> None:
    details = error["details"]
    case.assertEqual(
        (details["files_total"], details["files_copied"], details["failed_file"]),
        (4, copied, failed),
        error,
    )
    case.assertEqual(details["not_reached"], 4 - copied - (failed is not None), error)
    case.assertEqual((details["published"], details["container_left"]), (False, False), error)


def _raced(case: NativeAcceptance, root: Path, params: dict[str, Any]) -> dict[str, Any]:
    """Publish while holding before the rename, and take the name meanwhile."""
    answer: dict[str, Any] = {}
    publishing = threading.Thread(
        target=lambda: answer.update(case.worker.call("delivery.publish", params))
    )
    publishing.start()
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline and not [
        name for name in os.listdir(root) if name.startswith(".groundzero-staging-")
    ]:
        time.sleep(0.05)
    (root / "take").write_text("other party\n")
    publishing.join(timeout=60)
    return answer["error"]


@acceptance_case(worker_only=FAULT_FLAG)
def delivery_failures_report_file_by_file_and_leave_nothing(case: NativeAcceptance) -> None:
    """A full volume, a job file changed since its render and a name taken just before the
    rename each fail the publication, report how far it got file by file and leave nothing
    under the final name; a retry returns the recorded failure and writes nothing."""
    root, items = _fault_render(case, "no-space", "--test-write-fault", "publish:no-space")
    params = _fault_params(case, items)
    error = case.failure("delivery.publish", params)
    case.assertEqual(
        (_cause(error), error["details"]["reason"], error["details"]["stage"]),
        ("delivery_write_failed", "no_space", "copy"),
    )
    _require_progress(case, error, 1, "frames/frame.0002.png")
    case.assertEqual(case.failure("delivery.publish", params), error)
    case.assertEqual(os.listdir(root), [])
    root, items = _fault_render(case, "changed", "--test-write-fault", "publish:staged-changed")
    error = case.failure("delivery.publish", _fault_params(case, items))
    case.assertEqual(
        (_cause(error), error["details"]["reason"]), ("delivery_write_failed", "staged_changed")
    )
    _require_progress(case, error, 0, "frames/frame.0001.png")
    case.assertEqual(os.listdir(root), [])
    root, items = _fault_render(case, "raced", "--test-write-fault", "journal:intent:hold")
    error = _raced(case, root, _fault_params(case, items))
    case.assertEqual(
        (_cause(error), error["details"]["reason"], error["details"]["stage"]),
        ("delivery_write_failed", "name_taken", "publish"),
    )
    _require_progress(case, error, 4, None)
    case.assertEqual((root / "take").read_text(), "other party\n")
    case.assertEqual(os.listdir(root), ["take"])


@acceptance_case(worker_only=FAULT_FLAG)
def delivery_interrupted_publication_is_recovered(case: NativeAcceptance) -> None:
    """A worker killed while a delivery is copied leaves its staging container, which the
    next worker holding the root removes (not published); one killed after the rename
    leaves a complete delivery that the next worker reconciles as published."""
    for point, (kind, present) in {
        "publish:copied": ("not_published", False),
        "publish:renamed": ("published", True),
    }.items():
        with case.subTest(point=point):
            name = point.replace(":", "-")
            root, items = _fault_render(case, name, "--test-write-fault", f"{point}:crash")
            params = _fault_params(case, items)
            with case.assertRaises(RuntimeError):
                case.worker.call("delivery.publish", params)
            case.assertEqual(case.worker.process.wait(timeout=30), CRASH_EXIT_CODE)
            staged = [name for name in os.listdir(root) if name.startswith(".groundzero-staging")]
            case.assertEqual(len(staged), 1)
            restart(case, name + "-after", "--output-root", f"{ROOT}={root}")
            verdict = status(case, params["operation_id"])["verdict"]
            case.assertEqual((verdict["kind"], verdict["reconciled"]), (kind, True))
            case.assertEqual(os.listdir(root), ["take"] if present else [])
            if present:
                manifest = (root / "take" / "delivery.json").read_bytes()
                verify(root / "take", hashlib.sha256(manifest).hexdigest())


@acceptance_case
def delivery_retry_after_restart_writes_nothing(case: NativeAcceptance) -> None:
    """After the worker restarts, an exact retry of a publication is answered from the
    workspace journal with the recorded result and writes nothing."""
    root = _roots(case, ROOT)[ROOT]
    _granted(case, _flags({ROOT: root}))
    document, _level, _column = case.populated_scene()
    params = _publish_params(case, "take", _items((_render(case, document, frames=2), "frames")))
    published = case.success("delivery.publish", params)
    before = {path: path.stat().st_ino for path in root.rglob("*")}
    _granted(case, _flags({ROOT: root}))
    replayed = case.success("delivery.publish", params)
    case.assertEqual(replayed["receipt"]["source"], "journal")
    replayed.pop("receipt")
    case.assertEqual(replayed, published)
    case.assertEqual({path: path.stat().st_ino for path in root.rglob("*")}, before)


def _image_root(case: NativeAcceptance, name: str, size: str, fs: str) -> Path:
    """A disk image of its own volume, mounted as a canonical directory for an output root."""
    image, mount = case.directory / f"{name}.dmg", case.directory / name
    mount.mkdir()
    volume = name.replace("-", "").upper()[:11]
    for command in (
        ["hdiutil", "create", "-size", size, "-fs", fs, "-volname", volume, str(image)],
        ["hdiutil", "attach", str(image), "-mountpoint", str(mount), "-nobrowse"],
    ):
        subprocess.run(command, check=True, capture_output=True)
    case.addCleanup(
        subprocess.run, ["hdiutil", "detach", "-force", str(mount)], capture_output=True
    )
    return mount.resolve()


@acceptance_case
def delivery_to_other_volumes(case: NativeAcceptance) -> None:
    """A delivery publishes onto another volume by copy; a volume too small is refused
    before anything is written, and one without a no-replace rename is refused as
    publication_unsupported with nothing left under any name."""
    if platform.system() != "Darwin":
        case.skipTest("disk images are made with hdiutil")
    other = _image_root(case, "other-volume", "64m", "APFS")
    small = _image_root(case, "small-volume", "8m", "APFS")
    fat = _image_root(case, "fat", "40m", "MS-DOS FAT32")
    _granted(case, _flags({"other": other, "small": small, "fat": fat}))
    document, _level, _column = case.populated_scene()
    items = _items((_render(case, document, frames=2), "frames"))
    planned = case.success("delivery.preflight", _request("take", items, "other"))
    case.assertEqual(planned["plan"]["same_volume_as_workspace"], False)
    published = case.success("delivery.publish", _publish_params(case, "take", items, "other"))
    verify(other / "take", published["manifest_sha256"])
    case.assertEqual(_visible(other), ["take"])
    filler = small / "filler"
    filler.write_bytes(bytes(max(shutil.disk_usage(small).free - 2 * 1024 * 1024, 0)))
    planned = case.success("delivery.preflight", _request("take", items, "small"))
    case.assertEqual(_states(planned)["space"], ("fail", "insufficient_space"))
    error = case.failure("delivery.publish", _publish_params(case, "take", items, "small"))
    case.assertEqual((error["code"], error["details"]["check"]), ("insufficient_space", "space"))
    case.assertEqual(_visible(small), ["filler"])
    error = case.failure("delivery.publish", _publish_params(case, "take", items, "fat"))
    case.assertEqual(error["code"], "publication_unsupported", error)
    details = error["details"]
    case.assertEqual(
        (details["files_total"], details["files_copied"], details["published"]), (3, 3, False)
    )
    case.assertEqual(_visible(fat), [])
    planned = case.success("delivery.preflight", _request("take", items, "fat"))
    case.assertEqual(_states(planned)["filesystem"], ("fail", "publication_unsupported"))
    refused = case.failure("delivery.publish", _publish_params(case, "take", items, "fat"))
    case.assertEqual(
        (refused["code"], refused["details"]["check"]), ("publication_unsupported", "filesystem")
    )
