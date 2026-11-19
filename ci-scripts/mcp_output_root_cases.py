"""Output roots: granted at launch, separate from every other root, part of the authority."""

from __future__ import annotations

import os
import threading
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_journal_cases import SCENE, restart
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


def _roots(case: NativeAcceptance, *names: str) -> dict[str, Path]:
    roots = {}
    for name in names:
        root = case.directory / f"out-{name}"
        root.mkdir()
        roots[name] = root.resolve()
    return roots


def _flags(roots: dict[str, Path], modify: tuple[str, ...] = ()) -> tuple[str, ...]:
    flags: list[str] = []
    for name, root in roots.items():
        flags += ["--output-root", f"{name}={root}"]
    for name in modify:
        flags += ["--output-modify", name]
    return tuple(flags)


def _granted(case: NativeAcceptance, flags: tuple[str, ...]) -> Worker:
    """A worker launched with output roots (in SDK mode the adapter passes them on)."""
    case.worker.close()
    case.worker = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / f"roots-{len(flags)}.stderr.log",
        test_arguments=flags,
    )
    case.addCleanup(case.worker.close)
    return case.worker


@acceptance_case
def output_roots_are_separate_canonical_and_never_widen(case: NativeAcceptance) -> None:
    """Output roots must be canonical directories outside the workspace, the input roots
    and each other, under names that are not reserved; a granted root is reported in the
    authority and does not widen what the path tools reach."""
    roots = _roots(case, "renders", "archive")
    inside = case.workspace / "inside"
    inside.mkdir()
    alias = case.directory / "alias"
    alias.symlink_to(roots["renders"])
    nested = roots["renders"] / "nested"
    nested.mkdir()
    refusals = [
        ["--output-root", f"workspace={roots['renders']}"],
        ["--output-root", f"input={roots['renders']}"],
        ["--output-root", f"Bad={roots['renders']}"],
        ["--output-root", f"renders={inside}"],
        ["--output-root", f"renders={case.workspace}"],
        ["--input-root", str(case.inputs), "--output-root", f"renders={case.inputs}"],
        ["--output-root", f"renders={alias}"],
        ["--output-root", f"renders={roots['renders']}", "--output-root", f"nested={nested}"],
        ["--output-modify", "renders"],
        [
            arg
            for index in range(9)
            for arg in ("--output-root", f"r{index}={case.directory / f'r{index}'}")
        ],
    ]
    for index in range(9):
        (case.directory / f"r{index}").mkdir()
    for flags in refusals:
        with case.subTest(flags=flags[:2]):
            case.launch_refused(flags, "output-" + str(refusals.index(flags)))
    _granted(case, _flags(roots, ("archive",)))
    authority = case.success("automation.capabilities", {})["authority"]
    case.assertEqual(
        authority["output_roots"],
        [
            {"name": "archive", "path": str(roots["archive"]), "modify": True},
            {"name": "renders", "path": str(roots["renders"]), "modify": False},
        ],
    )
    document = case.create("Never widen")
    error = case.failure(
        "scene.save_copy",
        case.write_params(document, {"directory": str(roots["renders"] / "package")}),
    )
    case.assertEqual(error["code"], "path_denied")
    case.assertEqual(os.listdir(roots["renders"]), ["nested"])


@acceptance_case
def receipts_conflict_when_roots_change(case: NativeAcceptance) -> None:
    """The output roots are part of the launch authority: a write recorded under one set of
    roots conflicts, as grant_changed, when retried under another."""
    roots = _roots(case, "renders", "extra")
    _granted(case, _flags({"renders": roots["renders"]}))
    params = case.write_params(None, SCENE)
    case.success("scene.create", params)
    _granted(case, _flags(roots))
    error = case.failure("scene.create", params)
    case.assertEqual(
        (error["code"], error["details"]["reason"]), ("operation_conflict", "grant_changed")
    )
    restart(case, "no-roots")
    error = case.failure("scene.create", params)
    case.assertEqual(error["details"]["reason"], "grant_changed")


def _root_calls(case: NativeAcceptance, document: str, path: str, root: str) -> dict[str, Any]:
    """Every tool that takes a path below an output root, called with one path."""
    return {
        "scene.save_copy": case.write_params(document, {"directory": path, "root": root}),
        "scene.open": case.write_params(None, {"path": path, "root": root}),
        "scene.package_info": {"path": path, "root": root},
        "package.remove": case.write_params(
            None, {"directory": path, "root": root, "expected_package_id": "package:" + "0" * 64}
        ),
        "scene.save_preflight": {
            "document_id": document,
            "expected_revision": case.revisions[document],
            "directory": path,
            "root": root,
        },
    }


def _codes(case: NativeAcceptance, document: str, path: str, root: str) -> dict[str, Any]:
    codes = {}
    for tool, params in _root_calls(case, document, path, root).items():
        response = case.worker.call(tool, params)
        if "error" in response:
            error = response["error"]
            codes[tool] = (error["code"], error["details"].get("reason"))
        else:
            check = {row["name"]: row for row in response["result"].get("checks", [])}
            codes[tool] = ("result", check.get("path", {}).get("code"))
    return codes


@acceptance_case
def output_root_paths_hold_the_boundary(case: NativeAcceptance) -> None:
    """Paths below an output root refuse traversal, links on the way and roots not granted
    or replaced by a link; a hard-linked package file is never removed; nothing is written
    outside the root."""
    root = _roots(case, "deliveries")["deliveries"]
    outside = case.directory / "outside"
    outside.mkdir()
    (root / "linked").symlink_to(outside)
    _granted(case, _flags({"deliveries": root}, ("deliveries",)))
    document = case.create("Boundary")
    for path in ("../escape", "a/../b", "/absolute", "a//b"):
        with case.subTest(path=path):
            codes = _codes(case, document, path, "deliveries")
            case.assertEqual(set(codes.values()), {("invalid_path", "grammar")}, codes)
    codes = _codes(case, document, "linked/pkg", "deliveries")
    preflight = codes.pop("scene.save_preflight")
    case.assertEqual(preflight, ("result", "path_denied"))
    case.assertEqual({code for code, _reason in codes.values()}, {"path_denied"}, codes)
    codes = _codes(case, document, "pkg", "absent")
    case.assertEqual({code for code, _reason in codes.values()}, {"root_unavailable"}, codes)
    case.assertEqual(os.listdir(outside), [])
    saved = case.mutate("scene.save_copy", document, directory="pkg", root="deliveries")
    os.link(root / "pkg" / "scene.tnz", case.directory / "outside-link.tnz")
    params = case.write_params(
        None,
        {"directory": "pkg", "root": "deliveries", "expected_package_id": saved["package_id"]},
    )
    error = case.failure("package.remove", params)
    case.assertEqual((error["code"], error["details"]["reason"]), ("invalid_package", "hard_link"))
    case.assertTrue((root / "pkg" / "scene.tnz").exists())
    moved = case.directory / "moved-root"
    root.rename(moved)
    root.symlink_to(moved)
    try:
        error = case.failure(
            "scene.save_copy", case.write_params(document, {"directory": "x", "root": "deliveries"})
        )
        case.assertEqual(error["code"], "root_unavailable")
    finally:
        root.unlink()
        moved.rename(root)
    case.assertEqual(sorted(os.listdir(root)), ["linked", "pkg"])


@acceptance_case(worker_only="needs --test-write-fault, which the adapter never passes")
def output_root_parent_swapped_during_publish_is_not_followed(case: NativeAcceptance) -> None:
    """A parent moved and replaced by a link while a save copies into it is not followed:
    the package is published where the pinned parent now lies, and the result and journal
    name that place in its stored spelling."""
    root = _roots(case, "deliveries")["deliveries"]
    (root / "shots").mkdir()
    outside = case.directory / "outside"
    outside.mkdir()
    flags = _flags({"deliveries": root}) + ("--test-write-fault", "journal:intent:hold")
    _granted(case, flags)
    document = case.create("Swapped")
    request = case.write_params(document, {"directory": "shots/pkg", "root": "deliveries"})
    answer: dict[str, Any] = {}
    saving = threading.Thread(
        target=lambda: answer.update(case.worker.call("scene.save_copy", request))
    )
    saving.start()
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline and not [
        name for name in os.listdir(root / "shots") if name.startswith(".groundzero-staging-")
    ]:
        time.sleep(0.05)
    (root / "shots").rename(root / "moved")
    (root / "shots").symlink_to(outside)
    saving.join(timeout=60)
    case.assertEqual(answer["result"]["location"]["path"], "moved/pkg")
    case.assertEqual(os.listdir(outside), [])
    case.assertTrue((root / "moved" / "pkg" / "manifest.json").is_file())
    opened = case.mutate("scene.open", None, path="moved/pkg", root="deliveries")
    case.assertEqual(opened["package_id"], answer["result"]["package_id"])
