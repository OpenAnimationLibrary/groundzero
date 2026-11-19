"""Qualify the file boundary of every path-taking tool, driven from the catalog."""

from __future__ import annotations

import json
import os
import time
import uuid
from pathlib import Path
from typing import TYPE_CHECKING, Any

import mcp_file_tools
from mcp_case import acceptance_case
from mcp_file_matrix import (
    ALIAS,
    BY_MODE,
    COMMON,
    DENY,
    PIPE_BLOCK_LIMIT,
    VOLUME,
    Layout,
    Scenario,
    attach_volume,
)
from mcp_file_tools import PATH_TOOLS, PathTool
from mcp_file_trace import (
    descendants,
    files_mentioning,
    home_logs_mentioning,
    tree_state,
    unified_log_mentions,
)

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance
    from mcp_worker import Worker

FOLDING = {"case alias of an existing name", "decomposed spelling of a composed name"}
FOLDING |= {"composed spelling of a decomposed name"}
PERMISSION = {"unreadable folder", "read-only folder"}
JOURNAL = "/.groundzero/journal/"
SCHEMA_REJECTION = "INVALID_ARGUMENTS"


@acceptance_case
def file_tools_are_all_qualified(case: NativeAcceptance) -> None:
    """Fail when a catalog tool names a path but the boundary table does not cover it.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    tools = case.capabilities()["tools"]
    problems = mcp_file_tools.unqualified_tools(tools)
    case.assertEqual(problems, [], "\n".join(problems))
    named = {tool["name"] for tool in tools if mcp_file_tools.path_parameters(tool["inputSchema"])}
    covered = (
        set(PATH_TOOLS)
        | set(mcp_file_tools.SHARED_TOOLS)
        | set(mcp_file_tools.PROJECT_TOOLS)
        | set(mcp_file_tools.PACKAGE_TOOLS)
        | set(mcp_file_tools.DELIVERY_TOOLS)
    )
    case.assertTrue(named <= covered, named - covered)


def _without_journal(files: dict[str, Any]) -> dict[str, Any]:
    return {path: state for path, state in files.items() if JOURNAL not in str(path)}


class Observation:
    """What a call left behind: the files under the roots and the processes below the worker."""

    def __init__(
        self, case: NativeAcceptance, roots: list[Path], worker: Worker | None = None
    ) -> None:
        self.worker = worker or case.worker
        self.roots = roots
        self.files = tree_state(roots)
        self.processes = descendants(self.worker.process.pid)

    def unchanged(self) -> tuple[bool, str]:
        """Compare with now: whether nothing changed, and what did if something did.

        The workspace journal is left out: a write refused after its admission records the
        admission and its release there by design.
        """
        files = _without_journal(tree_state(self.roots))
        before = _without_journal(self.files)
        added = sorted(set(files) - set(before))
        removed = sorted(set(before) - set(files))
        changed = sorted(p for p in set(files) & set(before) if files[p] != before[p])
        started = sorted(descendants(self.worker.process.pid) - self.processes)
        problems = {"created": added, "removed": removed, "changed": changed, "started": started}
        return not any(problems.values()), repr({k: v for k, v in problems.items() if v})


def _skip_reason(label: str, path: str) -> str:
    if label in PERMISSION and os.geteuid() == 0:
        return "permission bits do not bind the superuser this run uses"
    if label in FOLDING and not Path(path).exists():
        return "this volume keeps different spellings as different names"
    return ""


def _check_outcome(
    case: NativeAcceptance, tool: PathTool, scenario: Scenario, response: dict[str, Any], path: str
) -> bool:
    """Assert the response matches the scenario; return whether the call was a success."""
    label, _build, kind, codes = scenario
    error = response.get("error")
    allowed = codes(tool)
    if kind == DENY or (kind == ALIAS and tool.mode == "write"):
        case.assertIsNotNone(error, f"{tool.name} admitted {label}: {path!r}")
    if error is None:
        return True
    case.assertIn(error["code"], allowed | {SCHEMA_REJECTION}, f"{label}: {error}")
    case.assertTrue(error["message"], error)
    return False


def _run_scenario(
    case: NativeAcceptance, layout: Layout, state: dict[str, Any], scenario: Scenario
) -> None:
    label, build, _kind, _codes = scenario
    path = build(layout)
    reason = _skip_reason(label, path)
    if reason:
        case.skipTest(f"{layout.tool.name} / {label}: {reason}")
    watched = [
        case.workspace,
        case.inputs,
        layout.outside,
        *([layout.volume] if layout.volume else []),
    ]
    before = Observation(case, watched)
    began = time.monotonic()
    response = layout.tool.drive(case, state, path)
    if label.startswith("named pipe"):
        elapsed = time.monotonic() - began
        case.assertLess(elapsed, PIPE_BLOCK_LIMIT, f"{layout.tool.name} blocked on a pipe")
    admitted = _check_outcome(case, layout.tool, scenario, response, path)
    if not admitted:
        quiet, detail = before.unchanged()
        case.assertTrue(quiet, f"{layout.tool.name} / {label} left traces: {detail}")
    elif layout.tool.mode == "write":
        case.assertTrue(os.path.exists(os.path.normpath(path)), f"{label}: the output is missing")


def _scenarios(mode: str, volume: Path | None) -> list[Scenario]:
    return [*COMMON, *BY_MODE[mode], *(VOLUME if volume else [])]


def _tool_layout(
    case: NativeAcceptance, tool: PathTool, marker: str, volume: Path | None
) -> Layout:
    root = case.inputs if tool.mode == "read" else case.workspace
    base = root / f"{marker}-{tool.name.replace('.', '-')}"
    base.mkdir()
    outside = case.directory / "outside"
    outside.mkdir(exist_ok=True)
    state = tool.prepare(case)
    return Layout(case, tool, state, root, base, outside, volume, marker)


def _no_logs_name(case: NativeAcceptance, marker: str, started: float) -> None:
    case.assertEqual(
        home_logs_mentioning(marker.encode()), [], "a per-user log names a denied path"
    )
    stray = [
        path
        for path in files_mentioning(marker.encode(), [case.directory])
        if path.suffix not in {".log", ".dmg"}
        and marker not in path.name
        and marker not in str(path.parent)
        and JOURNAL not in str(path)
    ]
    case.assertEqual(stray, [], "a file outside the logs records a denied path")
    _journal_names_only_admitted_paths(case, marker)
    mentioned = unified_log_mentions(marker, started)
    if mentioned is None:
        print(f"note: no readable system log on {os.uname().sysname}; its check was skipped")
    case.assertFalse(mentioned, "the system log names a denied path")


def _journal_names_only_admitted_paths(case: NativeAcceptance, marker: str) -> None:
    """The journal keeps the results of admitted calls, the targets of admitted
    publications, the places packages were registered or staged and the admitted
    packages an open reads (its pin), paths included, and nothing of a refused call."""
    for segment in case.directory.glob("**/.groundzero/journal/seg-*.jsonl"):
        for line in segment.read_bytes().splitlines():
            if marker.encode() not in line:
                continue
            record = json.loads(line[65:])
            kind = (record["type"], record.get("state"))
            case.assertIn(
                kind,
                {
                    ("outcome", "completed"),
                    ("intent", None),
                    ("package", None),
                    ("stage", None),
                    ("pin", None),
                },
                f"the journal records a refused path: {kind}",
            )


def run_matrix(case: NativeAcceptance, mode: str) -> None:
    """Drive every tool of one mode through every boundary scenario, with traces after denials.

    Args:
        case: A fresh native-worker acceptance fixture.
        mode: read, write or published: which family of PATH_TOOLS to drive.
    """
    started = time.time()
    marker = "gzq" + uuid.uuid4().hex[:10]
    volume, why = attach_volume(case)
    if volume is None:
        print(f"note: another-volume scenarios skipped: {why}")
    tools = [tool for tool in PATH_TOOLS.values() if tool.mode == mode]
    case.assertTrue(tools, mode)
    for tool in tools:
        layout = _tool_layout(case, tool, marker, volume)
        for scenario in _scenarios(mode, volume):
            with case.subTest(tool=tool.name, scenario=scenario[0]):
                _run_scenario(case, layout, layout.state, scenario)
    _no_logs_name(case, marker, started)


@acceptance_case
def read_tools_hold_the_file_boundary(case: NativeAcceptance) -> None:
    """Hold every input-reading tool to the boundary scenarios (the files-in family)."""
    run_matrix(case, "read")


@acceptance_case
def write_tools_hold_the_file_boundary(case: NativeAcceptance) -> None:
    """Hold every output-writing tool to the boundary scenarios (the files-out family)."""
    run_matrix(case, "write")


@acceptance_case
def published_tools_hold_the_file_boundary(case: NativeAcceptance) -> None:
    """Hold every tool that reopens a worker publication to the boundary scenarios."""
    run_matrix(case, "published")
