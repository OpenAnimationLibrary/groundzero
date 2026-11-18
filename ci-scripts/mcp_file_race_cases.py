"""Swap a path component between its validation and its use, and require every tool to refuse.

The worker's test-only `--test-path-swap PATH TARGET STAGE` option replaces PATH with a symbolic
link to TARGET at the named stage of the first request that names something at or below PATH. The
adapter never passes it, so these cases run against the raw worker only.
"""

from __future__ import annotations

import uuid
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_file_tools import PATH_TOOLS, READ_ONLY_REFUSED, PathTool
from mcp_file_trace import tree_state
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

INPUT_REFUSALS = {"invalid_asset", "asset_unavailable", "io_error"}


def _hooked_worker(case: NativeAcceptance, swap: Path, target: Path, stage: str) -> None:
    """Replace the case's worker with one that swaps `swap` for a link to `target` at `stage`."""
    worker = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / f"race-{uuid.uuid4().hex[:6]}.stderr.log",
        test_arguments=("--test-path-swap", str(swap), str(target), stage),
    )
    case.addCleanup(worker.close)
    case.worker = worker


def _swap_was_applied(case: NativeAcceptance, swap: Path) -> None:
    case.assertTrue(Path(str(swap) + ".fired").exists(), f"the hook never swapped {swap}")


def _expect_refusal(case: NativeAcceptance, tool: PathTool, response: dict[str, Any]) -> None:
    case.assertIn("error", response, f"{tool.name} followed a swapped path: {response}")
    allowed = {"path_denied"} | (
        INPUT_REFUSALS | READ_ONLY_REFUSED if tool.mode == "read" else set()
    )
    case.assertIn(response["error"]["code"], allowed, response)


def _expect_nothing_replaced(
    case: NativeAcceptance, tool: PathTool, holder: Path, response: dict[str, Any]
) -> None:
    """An output whose name was taken after it was approved fails; it never replaces the link."""
    case.assertIn("error", response, f"{tool.name} replaced what appeared at its destination")
    case.assertTrue((holder / f"item{tool.suffix}").is_symlink())


def _expect_pinned(
    case: NativeAcceptance, tool: PathTool, kind: str, holder: Path, response: dict[str, Any]
) -> None:
    """A swap after the directory was pinned cannot redirect: work finishes where it began."""
    if kind == "file" and tool.mode == "read":
        case.assertEqual(response.get("error", {}).get("code"), "path_denied", response)
        return
    if kind == "file":
        _expect_nothing_replaced(case, tool, holder, response)
        return
    case.assertNotIn("error", response, f"{tool.name} lost the pinned folder: {response}")
    if tool.mode == "write":
        moved = Path(str(holder) + ".swapped")
        relative = Path("inner") / f"item{tool.suffix}" if kind == "ancestor" else None
        found = (moved / relative).exists() if relative else any(moved.iterdir())
        case.assertTrue(found, f"{tool.name} published nothing under {moved}")


def _race(case: NativeAcceptance, tool: PathTool, kind: str, stage: str) -> None:
    """Race one tool; kind is the swapped component: the folder, an ancestor or the file."""
    marker = f"race-{uuid.uuid4().hex[:8]}"
    root = case.inputs if tool.mode == "read" else case.workspace
    holder = root / marker
    outside = case.directory / f"outside-{marker}"
    where, far = (holder / "inner", outside / "inner") if kind == "ancestor" else (holder, outside)
    for folder in (where, far):
        folder.mkdir(parents=True)
    name = f"item{tool.suffix}"
    swap, destination = (holder / name, outside / name) if kind == "file" else (holder, outside)
    _hooked_worker(case, swap, destination, stage)
    state = tool.prepare(case)
    if tool.mode == "read":
        tool.place(case, state, where / name)
        if stage == "pinned" and tool.suffix:
            (far / name).write_bytes(b"not a valid input\n")
        else:
            tool.place(case, state, far / name)
    elif kind == "file":
        tool.place(case, state, outside / name)
    before = tree_state([outside])
    response = tool.drive(case, state, str(where / name))
    _swap_was_applied(case, swap)
    case.assertEqual(tree_state([outside]), before, f"{tool.name} reached through the swap")
    if stage == "validated" and (tool.mode == "read" or kind != "file"):
        _expect_refusal(case, tool, response)
    elif stage == "pinned":
        _expect_pinned(case, tool, kind, holder, response)
    else:
        _expect_nothing_replaced(case, tool, holder, response)


KINDS = ("directory", "ancestor", "file")


def swapped_paths_are_refused_after_validation(case: NativeAcceptance) -> None:
    """Refuse a request whose path was redirected after the check and before the open.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    for tool in PATH_TOOLS.values():
        if tool.mode == "published":
            continue
        for kind in KINDS:
            with case.subTest(tool=tool.name, swapped=kind):
                _race(case, tool, kind, "validated")


def swapped_paths_cannot_redirect_a_pinned_directory(case: NativeAcceptance) -> None:
    """Finish an operation where it began when the path is swapped after its folder was pinned.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    for tool in PATH_TOOLS.values():
        if tool.mode == "published" or not tool.pinned:
            continue
        for kind in KINDS:
            with case.subTest(tool=tool.name, swapped=kind):
                _race(case, tool, kind, "pinned")
