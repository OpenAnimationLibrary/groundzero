"""Package removal: only verified registered packages, only their unchanged listed files."""

from __future__ import annotations

import hashlib
import os
import shutil
import threading
import time
import uuid
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_journal_cases import restart
from mcp_output_root_cases import _flags, _granted, _roots
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

FAULT_FLAG = "needs --test-write-fault, which the adapter never passes"


def _saved(case: NativeAcceptance, name: str) -> tuple[Path, str]:
    document, _level, _column = case.populated_scene()
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / name))
    return Path(saved["package_directory"]), saved["package_id"]


def _tree(root: Path) -> dict[str, str]:
    return {
        str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(root.rglob("*"))
        if path.is_file() and ".groundzero" not in path.parts and "runtime_" not in str(path)
    }


def _remove(case: NativeAcceptance, directory: str, package_id: str, **extra: Any) -> Any:
    params = {"directory": directory, "expected_package_id": package_id, **extra}
    return case.write_params(None, params)


def _refused(case: NativeAcceptance, directory: Path, package_id: str) -> tuple[str, Any]:
    error = case.failure("package.remove", _remove(case, str(directory), package_id))
    return error["code"], error["details"].get("reason")


@acceptance_case(worker_only=FAULT_FLAG)
def package_removal_keeps_unowned_and_pinned_files(case: NativeAcceptance) -> None:
    """Removal refuses an unregistered directory, an extra or changed file, a hard link
    and a package another worker is opening; each refusal deletes nothing."""
    package, package_id = _saved(case, "kept")
    copies = {}
    for name in ("edited", "extra", "changed", "linked"):
        copies[name] = case.workspace / name
        shutil.copytree(package, copies[name])
    with (copies["edited"] / "manifest.json").open("a") as manifest:
        manifest.write(" ")
    (copies["extra"] / "notes.txt").write_text("mine\n")
    scene = copies["changed"] / "scene.tnz"
    scene.write_bytes(scene.read_bytes().replace(b"<", b"<", 1)[:-1] + b" ")
    os.link(copies["linked"] / "scene.tnz", case.workspace / "outside-link.tnz")
    before = _tree(case.workspace)
    expected = {
        "edited": ("unregistered_package", "manifest_changed"),
        "extra": ("invalid_package", "unlisted_entry"),
        "changed": ("invalid_package", "changed_file"),
        "linked": ("invalid_package", "hard_link"),
    }
    for name, refusal in expected.items():
        with case.subTest(copy=name):
            case.assertEqual(_refused(case, copies[name], package_id), refusal)
    case.assertEqual(_tree(case.workspace), before)
    _opened_elsewhere(case, package, package_id)
    case.assertEqual(_tree(case.workspace), before)


def _opened_elsewhere(case: NativeAcceptance, package: Path, package_id: str) -> None:
    """A second worker held inside scene.open pins the package."""
    other = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / "opener.stderr.log",
        test_arguments=("--test-write-fault", "reopen:copy:hold"),
    )
    case.addCleanup(other.close)
    request = {"path": str(package), "operation_id": f"open-{uuid.uuid4()}"}
    opening = threading.Thread(target=other.call, args=("scene.open", request))
    opening.start()
    # The opener holds after copying its first file into a private copy.
    deadline = time.monotonic() + 20
    while not list(case.workspace.glob("runtime_*/opened-*/*")) and time.monotonic() < deadline:
        time.sleep(0.05)
    error = case.failure("package.remove", _remove(case, str(package), package_id))
    case.assertEqual(
        (error["code"], error["details"]["operation_id"]), ("in_use", request["operation_id"])
    )
    opening.join(timeout=120)


@acceptance_case(worker_only=FAULT_FLAG)
def removal_race_leaves_residue_not_loss(case: NativeAcceptance) -> None:
    """A file added to a package while it is being removed stays, named as residue in the
    trash the result reports; the listed files are gone."""
    restart(case, "racing-removal", "--test-write-fault", "remove:trashed:hold")
    package, package_id = _saved(case, "raced")
    request = _remove(case, str(package), package_id)
    digest = hashlib.sha256(request["operation_id"].encode()).hexdigest()[:16]
    trash = case.workspace / f".groundzero-trash-{digest}"
    result: dict[str, Any] = {}
    removing = threading.Thread(
        target=lambda: result.update(case.worker.call("package.remove", request))
    )
    removing.start()
    deadline = time.monotonic() + 20
    while not trash.exists() and time.monotonic() < deadline:
        time.sleep(0.05)
    (trash / "late.txt").write_text("written during removal\n")
    removing.join(timeout=60)
    removed = result["result"]
    case.assertEqual((removed["residue"], removed["residue_count"]), (["late.txt"], 1))
    case.assertEqual(removed["trash"]["path"], trash.name)
    case.assertEqual(sorted(path.name for path in trash.iterdir()), ["late.txt"])
    case.assertFalse(package.exists())


@acceptance_case
def removal_in_output_roots_needs_modify(case: NativeAcceptance) -> None:
    """Removing a package from an output root needs --output-modify for that root."""
    roots = _roots(case, "deliveries")
    _granted(case, _flags(roots))
    document, _level, _column = case.populated_scene()
    saved = case.mutate("scene.save_copy", document, directory="pkg", root="deliveries")
    params = _remove(case, "pkg", saved["package_id"], root="deliveries")
    case.assertEqual(case.failure("package.remove", params)["code"], "modify_not_granted")
    case.assertEqual(os.listdir(roots["deliveries"]), ["pkg"])
    _granted(case, _flags(roots, ("deliveries",)))
    params = _remove(case, "pkg", saved["package_id"], root="deliveries")
    removed = case.success("package.remove", params)
    case.assertEqual(
        (removed["removed"], removed["residue"], removed["trash"], removed["files_removed"]),
        (True, [], None, saved["files"]),
    )
    case.assertEqual(removed["bytes_freed"], saved["bytes"])
    case.assertEqual(os.listdir(roots["deliveries"]), [])


@acceptance_case
def other_copies_stay_admissible_after_removal(case: NativeAcceptance) -> None:
    """Removal keeps the registration: a copy of the same bytes elsewhere still opens, and
    the removal is answered again from its receipt."""
    package, package_id = _saved(case, "original")
    copy = case.workspace / "copy"
    shutil.copytree(package, copy)
    request = _remove(case, str(package), package_id)
    removed = case.success("package.remove", request)
    case.assertEqual((removed["removed"], removed["trash"]), (True, None))
    case.assertFalse(package.exists())
    case.assertEqual(case.success("package.remove", request)["package_id"], package_id)
    opened = case.mutate("scene.open", None, path=str(copy))
    case.assertEqual((opened["package_id"], opened["relocated"]), (package_id, True))
