"""Output roots: granted at launch, separate from every other root, part of the authority."""

from __future__ import annotations

import os
from pathlib import Path
from typing import TYPE_CHECKING

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
