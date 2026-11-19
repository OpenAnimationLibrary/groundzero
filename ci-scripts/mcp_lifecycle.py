"""Generate and check the lifecycle grid: what every kind of thing a scene holds can do.

`mcp_lifecycle.json` lists each resource family (a level, a column, a camera, a curve, an effect,
a palette page, a sound clip and so on) against eight operations: inspect, create, modify,
duplicate, reorder, remove, persist and recover. Every cell names the tools that do it, or a
disposition: planned (with the census outcome that will deliver it), unsupported, restricted or
no_undo (each with its reason). The check fails when

- a family lacks a cell, so no resource can be created without a stated way to remove it, save it
  and bring it back;
- a cell names a tool that is not in the census, or a planned outcome that is not planned there;
- a tool of the census that writes appears in no cell and is not listed under `outside`;
- an `evidence` file does not exercise the cell (a persist or recover cell must be shown by a case
  that names its tool and one of the family's own create or modify tools).

`--document PATH` writes the grid as a Markdown document, which is not kept in the repository.
`--executable` adds the comparison with a live worker's catalog. The default check needs no
worker and runs in the static layer.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

from mcp_census import SCRIPTS, load_census

GRID_PATH = SCRIPTS / "mcp_lifecycle.json"
OPERATIONS = (
    "inspect",
    "create",
    "modify",
    "duplicate",
    "reorder",
    "remove",
    "persist",
    "recover",
)
REASONED = {"unsupported", "restricted", "no_undo"}
DISPOSITIONS = REASONED | {"planned"}
FAMILY_ID = re.compile(r"^[a-z][a-z_]*$")
EVIDENCE_OPERATIONS = {"persist", "recover"}
WORK_OPERATIONS = ("create", "modify")


def load_grid(path: Path = GRID_PATH) -> dict[str, Any]:
    """Read the grid data file."""
    return json.loads(path.read_text(encoding="utf-8"))


def _cell_problems(name: str, cell: Any, census: dict[str, Any]) -> list[str]:
    if not isinstance(cell, dict):
        return [f"{name}: the cell must be an object with `tools` or a `disposition`"]
    allowed = {"tools", "disposition", "reason", "outcome", "evidence"}
    problems = [f"{name}: unknown field {key!r}" for key in sorted(set(cell) - allowed)]
    if "tools" in cell:
        problems += _tool_problems(name, cell, census)
    elif cell.get("disposition") in DISPOSITIONS:
        problems += _disposition_problems(name, cell, census)
    else:
        problems.append(
            f"{name}: name the tools that do it, or a disposition from {sorted(DISPOSITIONS)}"
        )
    return problems


def _tool_problems(name: str, cell: dict[str, Any], census: dict[str, Any]) -> list[str]:
    tools = cell["tools"]
    if not tools or not isinstance(tools, list):
        return [f"{name}: `tools` must be a non-empty list"]
    found = [
        f"{name}: tool {tool} is not in the census" for tool in tools if tool not in census["tools"]
    ]
    if "disposition" in cell:
        found.append(f"{name}: a cell with tools has no disposition")
    return found


def _disposition_problems(name: str, cell: dict[str, Any], census: dict[str, Any]) -> list[str]:
    kind = cell["disposition"]
    if kind in REASONED:
        return (
            [] if str(cell.get("reason", "")).strip() else [f"{name}: a {kind} cell needs a reason"]
        )
    outcomes = {outcome["id"]: outcome for outcome in census["outcomes"]}
    outcome = outcomes.get(cell.get("outcome", ""))
    if outcome is None:
        return [
            f"{name}: a planned cell names a census outcome; {cell.get('outcome')!r} is not one"
        ]
    if outcome["disposition"]["kind"] != "planned":
        return [
            f"{name}: outcome {outcome['id']} is {outcome['disposition']['kind']} in the census, "
            "not planned: name its tools in this cell"
        ]
    return []


def _evidence_problems(
    name: str, operation: str, cell: dict[str, Any], family: dict[str, Any], root: Path
) -> list[str]:
    evidence = cell.get("evidence")
    if operation in EVIDENCE_OPERATIONS and "tools" in cell and evidence is None:
        return [f"{name}: a cell with tools needs `evidence`, a case file that exercises it"]
    if evidence is None:
        return []
    files = [evidence] if isinstance(evidence, str) else evidence
    missing = [file for file in files if not (root / file).is_file()]
    if missing:
        return [f"{name}: evidence file {file} does not exist in ci-scripts" for file in missing]
    text = "\n".join((root / file).read_text(encoding="utf-8") for file in files)
    own = [tool for op in WORK_OPERATIONS for tool in family["cells"][op].get("tools", [])]
    problems = [f"{name}: {files} never name {tool}" for tool in cell["tools"] if tool not in text]
    if own and not any(tool in text for tool in own):
        problems.append(f"{name}: {files} name none of the family's tools {sorted(set(own))}")
    return problems


def _family_problems(family: dict[str, Any], census: dict[str, Any], root: Path) -> list[str]:
    name = family.get("id", "?")
    problems = []
    if not FAMILY_ID.match(str(name)) or not str(family.get("name", "")).strip():
        problems.append(f"family {name}: needs a snake_case id and a name")
    cells = family.get("cells", {})
    for operation in OPERATIONS:
        label = f"{name}.{operation}"
        if operation not in cells:
            problems.append(
                f"{label}: family {name} lacks a cell for {operation}; add a tool list or a "
                "disposition (planned, unsupported, restricted or no_undo) to mcp_lifecycle.json"
            )
            continue
        problems += _cell_problems(label, cells[operation], census)
        if isinstance(cells[operation], dict):
            problems += _evidence_problems(label, operation, cells[operation], family, root)
    problems += [f"{name}: unknown operation {op!r}" for op in sorted(set(cells) - set(OPERATIONS))]
    return problems


def _placed(grid: dict[str, Any]) -> set[str]:
    placed: set[str] = set()
    for family in grid["families"]:
        for cell in family.get("cells", {}).values():
            placed.update(cell.get("tools", []) if isinstance(cell, dict) else [])
    return placed


def _outside_problems(grid: dict[str, Any], census: dict[str, Any], placed: set[str]) -> list[str]:
    problems = []
    for tool, reason in sorted(grid["outside"].items()):
        if tool not in census["tools"]:
            problems.append(f"outside: {tool} is not in the census")
        if tool in placed:
            problems.append(f"outside: {tool} is also in a family cell; keep one")
        if not str(reason).strip():
            problems.append(f"outside: {tool} needs a reason")
    return problems


def _coverage_problems(grid: dict[str, Any], census: dict[str, Any]) -> list[str]:
    placed = _placed(grid)
    problems = _outside_problems(grid, census, placed)
    for tool, entry in census["tools"].items():
        if entry["writes"] and tool not in placed and tool not in grid["outside"]:
            problems.append(
                f"{tool} writes but is in no lifecycle cell: add it to the create, modify, "
                "remove or other cell of its family in ci-scripts/mcp_lifecycle.json (or a new "
                "family with all eight cells), or list it under `outside` with a reason"
            )
    return problems


def check(grid: dict[str, Any], census: dict[str, Any], root: Path = SCRIPTS) -> list[str]:
    """Return every way the grid is incomplete or out of step with the census."""
    problems: list[str] = []
    if grid.get("operations") != list(OPERATIONS):
        problems.append(f"operations must be exactly {list(OPERATIONS)}")
    identifiers = [family.get("id") for family in grid.get("families", [])]
    problems += [
        f"family {name} is listed twice"
        for name in sorted(set(identifiers))
        if identifiers.count(name) > 1
    ]
    for family in grid.get("families", []):
        problems += _family_problems(family, census, root)
    return problems + _coverage_problems(grid, census)


def check_live(grid: dict[str, Any], catalog: dict[str, Any]) -> list[str]:
    """Require every tool a cell names to be in a live worker's catalog."""
    live = {tool["name"] for tool in catalog["tools"]}
    named = {
        tool
        for family in grid["families"]
        for cell in family["cells"].values()
        for tool in cell.get("tools", [])
    }
    local = {name for name in named if name.split(".")[0] in {"live", "toolsets"}}
    return [f"the live worker has no tool {tool}" for tool in sorted(named - live - local)]


def _label(cell: dict[str, Any], outcomes: dict[str, Any]) -> str:
    if "tools" in cell:
        return "<br>".join(f"`{tool}`" for tool in cell["tools"])
    if cell["disposition"] == "planned":
        milestone = outcomes[cell["outcome"]]["disposition"]["milestone"]
        return f"planned {milestone} (`{cell['outcome']}`)"
    return cell["disposition"].replace("_", " ")


def render_document(grid: dict[str, Any], census: dict[str, Any]) -> str:
    """Render the grid as Markdown: the table, the gaps by operation and every reason."""
    outcomes = {outcome["id"]: outcome for outcome in census["outcomes"]}
    lines = [
        "# Lifecycle grid",
        "",
        "Generated by `ci-scripts/mcp_lifecycle.py --document` from `mcp_lifecycle.json` and "
        "`mcp_census.json`; not kept in the repository.",
        "",
        "| Family | " + " | ".join(OPERATIONS) + " |",
        "|---|" + "---|" * len(OPERATIONS),
    ]
    for family in grid["families"]:
        row = [_label(family["cells"][op], outcomes) for op in OPERATIONS]
        lines.append(f"| {family['name']} | " + " | ".join(row) + " |")
    lines += [
        "",
        "## Gaps by operation",
        "",
        "| Operation | tools | planned | unsupported | restricted | no undo |",
    ]
    lines.append("|---|---:|---:|---:|---:|---:|")
    for op in OPERATIONS:
        kinds = [family["cells"][op].get("disposition", "tools") for family in grid["families"]]
        counts = [
            kinds.count(kind)
            for kind in ("tools", "planned", "unsupported", "restricted", "no_undo")
        ]
        lines.append(f"| {op} | " + " | ".join(map(str, counts)) + " |")
    lines += ["", "## Dispositions", ""]
    for family in grid["families"]:
        for op in OPERATIONS:
            cell = family["cells"][op]
            if cell.get("disposition") in REASONED:
                kind = cell["disposition"].replace("_", " ")
                lines.append(f"- {family['name']}, {op}: {kind}: {cell['reason']}")
    lines += ["", "## Tools outside the families", ""]
    lines += [f"- `{tool}`: {reason}" for tool, reason in sorted(grid["outside"].items())]
    return "\n".join(lines) + "\n"


def main() -> int:
    """Check the grid, optionally against a live worker, and optionally write the document."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, help="Compare with this worker's catalog")
    parser.add_argument("--document", type=Path, help="Write the grid document to this file")
    args = parser.parse_args()
    grid, census = load_grid(), load_census()
    problems = check(grid, census)
    if args.executable:
        from mcp_census import fetch_catalog

        problems += check_live(grid, fetch_catalog(args.executable.resolve(strict=True))[0])
    if args.document and not problems:
        args.document.write_text(render_document(grid, census), encoding="utf-8")
    kinds = [
        cell.get("disposition", "tools") for f in grid["families"] for cell in f["cells"].values()
    ]
    print(
        f"lifecycle: {len(grid['families'])} families x {len(OPERATIONS)} operations; "
        + ", ".join(
            f"{kind} {kinds.count(kind)}"
            for kind in ("tools", "planned", "unsupported", "restricted", "no_undo")
        )
    )
    for problem in problems:
        print(f"lifecycle: {problem}")
    return int(bool(problems))


if __name__ == "__main__":
    sys.exit(main())
