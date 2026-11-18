"""Swap a path component between its validation and its use, and require every tool to refuse.

The worker's test-only `--test-path-swap PATH TARGET STAGE` option replaces PATH with a symbolic
link to TARGET at the named stage of the first request that names something at or below PATH. The
adapter never passes it, so these cases run against the raw worker only.
"""

from __future__ import annotations

import uuid
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_file_tools import PATH_TOOLS, PathTool
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
    allowed = {"path_denied"} | (INPUT_REFUSALS if tool.mode == "read" else set())
    case.assertIn(response["error"]["code"], allowed, response)


def _expect_pinned(
    case: NativeAcceptance, tool: PathTool, parent: bool, holder: Path, response: dict[str, Any]
) -> None:
    """A swap after the directory was pinned cannot redirect: work finishes where it began."""
    if tool.mode == "read" and not parent:
        case.assertEqual(response.get("error", {}).get("code"), "path_denied", response)
    elif tool.mode == "read":
        case.assertNotIn("error", response, f"{tool.name} lost the pinned folder: {response}")
    elif parent:
        case.assertNotIn("error", response, f"{tool.name} lost the pinned folder: {response}")
        moved = Path(str(holder) + ".swapped")
        case.assertTrue(any(moved.iterdir()), f"{tool.name} published nothing in {moved}")
    elif "result" in response:
        case.assertFalse((holder / f"item{tool.suffix}").is_symlink())


def _race(case: NativeAcceptance, tool: PathTool, parent: bool, stage: str) -> None:
    """Race one tool: the swapped component is the holding directory or the final name."""
    marker = f"race-{uuid.uuid4().hex[:8]}"
    root = case.inputs if tool.mode == "read" else case.workspace
    holder = root / marker
    outside = case.directory / f"outside-{marker}"
    outside.mkdir()
    holder.mkdir()
    name = f"item{tool.suffix}"
    swap, destination = (holder, outside) if parent else (holder / name, outside / name)
    _hooked_worker(case, swap, destination, stage)
    state = tool.prepare(case)
    if tool.mode == "read":
        tool.place(case, state, holder / name)
        (outside / name).write_bytes(b"not a valid input\n")
    elif not parent:
        tool.place(case, state, outside / name)
    before = tree_state([outside])
    response = tool.drive(case, state, str(holder / name))
    _swap_was_applied(case, swap)
    case.assertEqual(tree_state([outside]), before, f"{tool.name} reached through the swap")
    if stage == "validated" and (tool.mode == "read" or parent):
        _expect_refusal(case, tool, response)
    elif stage == "pinned":
        _expect_pinned(case, tool, parent, holder, response)
    elif "result" in response:
        case.assertFalse((holder / name).is_symlink(), "a link stayed at the destination")


def swapped_paths_are_refused_after_validation(case: NativeAcceptance) -> None:
    """Refuse a request whose path was redirected after the check and before the open.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    for tool in PATH_TOOLS.values():
        if tool.mode == "published":
            continue
        for parent in (True, False):
            with case.subTest(tool=tool.name, swapped="directory" if parent else "file"):
                _race(case, tool, parent, "validated")


def swapped_paths_cannot_redirect_a_pinned_directory(case: NativeAcceptance) -> None:
    """Finish an operation where it began when the path is swapped after its folder was pinned.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    for tool in PATH_TOOLS.values():
        if tool.mode == "published":
            continue
        for parent in (True, False):
            with case.subTest(tool=tool.name, swapped="directory" if parent else "file"):
                _race(case, tool, parent, "pinned")
