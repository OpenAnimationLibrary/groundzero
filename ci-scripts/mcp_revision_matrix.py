"""Check the revision table against the tool catalog and generate the revision coverage document.

The table in `mcp_revision_matrix.json` drives the acceptance check in `mcp_revision_cases.py`.
This script needs no worker: it requires an entry for every writing tool the census lists, a
listed reason for every skipped path, and a current generated document. `--update` rewrites the
document.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

import mcp_census

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "ci-scripts"
MATRIX_PATH = SCRIPTS / "mcp_revision_matrix.json"
ACCEPTANCE_PATH = SCRIPTS / "mcp_acceptance.py"
KINDS = ("document", "new_document", "read_only_document", "discard", "file", "worker_state")
ENTRY_FIELDS = (
    "group",
    "kind",
    "fixture",
    "domain",
    "class",
    "changes",
    "bumps",
    "reads",
    "reject",
)
NOOP_POLICIES = ("preserves", "rejects", "bumps")
FILE_KINDS = ("file", "worker_state")
OUTSIDE_KINDS = (*FILE_KINDS, "read_only_document")
# Effects on a document that advance its revision when native state changes.
ADVANCING_DOCUMENT_EFFECTS = ("writes", "creates", "discards")
DOMAIN_ORDER = (
    "Scenes and levels",
    "Timeline and columns",
    "Stage and animation",
    "Effects",
    "Drawings, vector art and palettes",
    "Rigging",
    "Audio, storyboard and lip-sync",
    "Document lifecycle, files and jobs",
)


def load_matrix(path: Path = MATRIX_PATH) -> dict[str, Any]:
    """Read the revision table."""
    return json.loads(path.read_text(encoding="utf-8"))


def dump_matrix(matrix: dict[str, Any]) -> str:
    """Serialize the table with a stable layout."""
    return json.dumps(matrix, indent=1, ensure_ascii=False) + "\n"


def entry_tool(key: str, entry: dict[str, Any]) -> str:
    """Return the tool an entry exercises; a scenario entry names it explicitly."""
    return entry.get("tool", key)


def entry_help(tool: str) -> str:
    """Say which file and entry to add for a writing tool the table lacks."""
    example = {
        tool: {
            "group": "an existing group such as scene",
            "kind": "document",
            "fixture": "an existing fixture that holds what the call needs",
            "domain": "an existing domain heading",
            "class": "content",
            "changes": "what the call changes",
            "bumps": "document revision",
            "reads": [{"tool": "scene.summary"}],
            "success": {"args": {"...": "arguments of a call that changes state"}},
            "reject": [{"args": {"...": "arguments the tool refuses"}, "code": "error_code"}],
            "noop": {"policy": "preserves", "documented": "Not stated; the rule applies."},
        }
    }
    return (
        f'  Add an entry for {tool!r} under "tools" in ci-scripts/mcp_revision_matrix.json, '
        'and a fixture under "fixtures" if none fits. Use "no_noop": "reason" instead of '
        '"noop" when a repeat has no meaning. The check fails until the entry runs.\n'
        f"  Minimal example:\n  {json.dumps(example)}"
    )


def writing_tools(census: dict[str, Any]) -> list[str]:
    """List the worker tools the census marks as writing."""
    return sorted(
        name
        for name, entry in census["tools"].items()
        if entry["origin"] == "worker" and entry["writes"]
    )


def _fixture_problems(matrix: dict[str, Any], census: dict[str, Any]) -> list[str]:
    problems = []
    for name, spec in matrix["fixtures"].items():
        if "extends" in spec and spec["extends"] not in matrix["fixtures"]:
            problems.append(f"fixture {name}: extends unknown fixture {spec['extends']}")
        for step in spec["steps"]:
            tool = step.get("tool")
            if tool is not None and tool not in census["tools"]:
                problems.append(f"fixture {name}: step calls unknown tool {tool}")
    return problems


def _shape_problems(key: str, entry: dict[str, Any], matrix: dict[str, Any]) -> list[str]:
    problems = [f"entry {key}: missing {field}" for field in ENTRY_FIELDS if field not in entry]
    if entry.get("kind") not in KINDS:
        problems.append(f"entry {key}: unknown kind {entry.get('kind')!r}")
    if entry.get("fixture") not in matrix["fixtures"]:
        problems.append(f"entry {key}: unknown fixture {entry.get('fixture')!r}")
    if not entry.get("reject"):
        problems.append(f"entry {key}: needs at least one rejected call")
    for item in entry.get("reject", []):
        if not item.get("code"):
            problems.append(f"entry {key}: a rejected call does not pin its error code")
    return problems


def _path_problems(key: str, entry: dict[str, Any]) -> list[str]:
    kind = entry.get("kind")
    problems = []
    if not entry.get("success") and not (entry.get("unexercised") or "/" in key):
        problems.append(f"entry {key}: has no successful call and gives no reason")
    if entry.get("success") and kind == "document" and not entry.get("reads"):
        problems.append(f"entry {key}: a document change needs a read that observes it")
    if entry.get("success") and kind in FILE_KINDS and not entry.get("effect"):
        problems.append(
            f"entry {key}: a file, job or worker-state write needs an effect to observe"
        )
    if entry.get("success") and kind == "document" and "/" not in key:
        has_noop = bool(entry.get("noop")) != bool(entry.get("no_noop"))
        if not has_noop:
            problems.append(f"entry {key}: needs exactly one of noop and no_noop")
    noop = entry.get("noop")
    if noop is not None and noop.get("policy") not in NOOP_POLICIES:
        problems.append(f"entry {key}: unknown no-op policy")
    if noop is not None and not noop.get("documented"):
        problems.append(f"entry {key}: a no-op must say whether the tool documents it")
    return problems


def check_entries(matrix: dict[str, Any], census: dict[str, Any]) -> list[str]:
    """Check each entry's shape and that every writing tool has a primary entry."""
    problems = _fixture_problems(matrix, census)
    writing = set(writing_tools(census))
    for key, entry in matrix["tools"].items():
        tool = entry_tool(key, entry)
        if tool not in writing:
            problems.append(f"entry {key}: {tool} is not a writing worker tool in the census")
        problems.extend(_shape_problems(key, entry, matrix))
        problems.extend(_path_problems(key, entry))
        problems.extend(_read_problems(key, entry, census))
    problems.extend(
        f"writing tool {tool}: has no revision table entry.\n{entry_help(tool)}"
        for tool in sorted(writing - set(matrix["tools"]))
    )
    return problems


def _read_problems(key: str, entry: dict[str, Any], census: dict[str, Any]) -> list[str]:
    reads = list(entry.get("reads", []))
    if "effect" in entry and "read" in entry["effect"]:
        reads.append(entry["effect"]["read"])
    return [
        f"entry {key}: reads unknown tool {read['tool']}"
        for read in reads
        if read["tool"] not in census["tools"]
    ]


def check_violations(matrix: dict[str, Any]) -> list[str]:
    """Require every known violation to name an entry, a phase and the defect."""
    problems = []
    for item in matrix["known_violations"]:
        label = f"known violation {item.get('entry')}"
        if item.get("entry") not in matrix["tools"]:
            problems.append(f"{label}: names no table entry")
        for field in ("tool", "call", "phase", "expected", "observed", "defect"):
            if not str(item.get(field, "")).strip():
                problems.append(f"{label}: missing {field}")
    return problems


def check_groups(matrix: dict[str, Any], acceptance: str) -> list[str]:
    """Require an acceptance method for every group the table names."""
    groups = {entry["group"] for entry in matrix["tools"].values()}
    return [
        f"group {group}: acceptance has no test_revision_matrix_{group}"
        for group in sorted(groups)
        if not re.search(rf"def test_revision_matrix_{group}\(", acceptance)
    ]


def check_live(matrix: dict[str, Any], capabilities: dict[str, Any]) -> list[str]:
    """Compare the table with the catalog a worker reports, writes and declared kinds."""
    tools = {tool["name"]: tool for tool in capabilities["tools"]}
    writing = {name for name, tool in tools.items() if not tool["annotations"]["readOnlyHint"]}
    primary = {key for key in matrix["tools"] if "/" not in key}
    problems = [
        f"writing tool {name}: has no table entry.\n{entry_help(name)}"
        for name in sorted(writing - primary)
    ]
    problems += [f"entry {name}: is not a writing tool" for name in sorted(primary - writing)]
    for name in sorted(writing & primary):
        outside = tools[name]["effects"]["document"] not in ADVANCING_DOCUMENT_EFFECTS
        kind = matrix["tools"][name]["kind"]
        if outside != (kind in OUTSIDE_KINDS):
            problems.append(
                f"entry {name}: kind {kind} contradicts effects.document in the catalog"
            )
    return problems


def static_problems(matrix: dict[str, Any], census: dict[str, Any], acceptance: str) -> list[str]:
    """Run every check that needs no worker."""
    problems = check_entries(matrix, census) + check_violations(matrix)
    problems += check_groups(matrix, acceptance)
    return problems


def _tests(census: dict[str, Any], tool: str, group: str) -> str:
    own = f"{mcp_census.ACCEPTANCE}::test_revision_matrix_{group}"
    others = [t for t in census["tools"][tool]["tests"] if "revision_matrix" not in t][:2]
    return "<br>".join(f"`{test}`" for test in [own, *others])


def _reads(entry: dict[str, Any]) -> str:
    names = {read["tool"] for read in entry["reads"]}
    if "effect" in entry and "read" in entry["effect"]:
        names.add(entry["effect"]["read"]["tool"])
    if "effect" in entry and "file" in entry["effect"]:
        names.add("file system")
    return ", ".join(f"`{name}`" if "." in name else name for name in sorted(names)) or "none"


def _noop_text(entry: dict[str, Any]) -> str:
    noop = entry.get("noop")
    if noop is None:
        return (
            entry.get("no_noop", "Not applicable.") if entry.get("success") else "Not applicable."
        )
    if noop["policy"] == "rejects":
        return f"A repeat is rejected ({noop['code']}) and changes nothing."
    return "A repeat leaves the revision and the reads unchanged."


def _row(key: str, entry: dict[str, Any], census: dict[str, Any]) -> list[str]:
    tool = entry_tool(key, entry)
    return [
        f"`{key}`",
        entry["changes"],
        entry["class"],
        entry["bumps"],
        _reads(entry),
        _noop_text(entry),
        _tests(census, tool, entry["group"]),
    ]


def _table(header: list[str], rows: list[list[str]]) -> list[str]:
    lines = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    lines.extend("| " + " | ".join(row) + " |" for row in rows)
    return lines + [""]


def _domain_sections(matrix: dict[str, Any], census: dict[str, Any]) -> list[str]:
    lines: list[str] = []
    header = ["Tool", "Changes", "Class", "Revision", "Observed by", "No-op", "Tests"]
    for domain in DOMAIN_ORDER:
        rows = [
            _row(key, entry, census)
            for key, entry in sorted(matrix["tools"].items())
            if entry["domain"] == domain and "/" not in key
        ]
        lines += [f"### {domain}", ""] + _table(header, rows)
    return lines


def _scenario_section(matrix: dict[str, Any]) -> list[str]:
    rows = []
    for key, entry in sorted(matrix["tools"].items()):
        if "/" in key:
            rows.append([f"`{key}`", entry["changes"], entry["reject"][0]["code"]])
    return ["## Extra rejection scenarios", ""] + _table(["Scenario", "Call", "Error"], rows)


def _violation_section(matrix: dict[str, Any]) -> list[str]:
    lines = ["## Known violations", ""]
    if not matrix["known_violations"]:
        return lines + ["None.", ""]
    lines.append(
        "The check stays green for these entries only while the defect reproduces exactly. A fix "
        "makes the entry fail until it is removed."
    )
    lines.append("")
    rows = [
        [
            f"`{item['entry']}`",
            item["call"],
            item["phase"],
            item["expected"],
            item["observed"],
            item["defect"],
        ]
        for item in matrix["known_violations"]
    ]
    header = ["Entry", "Call", "Phase", "Expected", "Observed", "Defect"]
    return lines + _table(header, rows)


def _class_section(matrix: dict[str, Any]) -> list[str]:
    rows = [
        [item["class"], item["examples"], item["bumps"], item["reference"]]
        for item in matrix["notes"]["classes"]
    ]
    header = ["Class", "Examples", "Revision", "Where"]
    return ["## Changes that are not content", ""] + _table(header, rows)


def _undo_section(matrix: dict[str, Any]) -> list[str]:
    undo = matrix["notes"]["live_undo"]
    lines = [
        "## Live undo and stale edits",
        "",
        f"Question: {undo['question']}",
        "",
        "Current behavior:",
        "",
    ]
    lines += [f"- {item}" for item in undo["current"]]
    lines += ["", "Options:", ""]
    lines += [f"- {o['name']}: {o['effect']} Cost: {o['cost']}" for o in undo["options"]]
    return lines + ["", f"Decision: {undo['decision']}", ""]


def _summary_section(matrix: dict[str, Any], census: dict[str, Any]) -> list[str]:
    entries = matrix["tools"]
    primary = [key for key in entries if "/" not in key]
    unexercised = sorted(key for key in primary if entries[key].get("unexercised"))
    rows = [
        ["Writing tools in the catalog", str(len(writing_tools(census)))],
        ["Writing tools with a table entry", str(len(primary))],
        ["Entries, including extra rejection scenarios", str(len(entries))],
        ["Known violations", str(len(matrix["known_violations"]))],
        ["Success paths not exercised", ", ".join(f"`{k}`" for k in unexercised) or "none"],
    ]
    return ["## Summary", ""] + _table(["Measure", "Value"], rows)


def render_document(matrix: dict[str, Any], census: dict[str, Any]) -> str:
    """Render the revision coverage document with a byte-stable layout."""
    lines = [
        "# MCP revision coverage",
        "",
        "Generated by `ci-scripts/mcp_revision_matrix.py --document` from",
        "`ci-scripts/mcp_revision_matrix.json`. Edit the table, never this document.",
        "",
        "## The rule",
        "",
    ]
    lines += [f"{paragraph}" for item in matrix["notes"]["rule"] for paragraph in (item, "")]
    lines += _summary_section(matrix, census)
    lines += _class_section(matrix)
    lines += ["## Writing tools by domain", ""] + _domain_sections(matrix, census)
    lines += _scenario_section(matrix)
    lines += _violation_section(matrix)
    lines += _undo_section(matrix)
    return "\n".join(lines).rstrip("\n") + "\n"


def main() -> int:
    """Check the revision table and optionally write its document."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--update", action="store_true", help="Accepted; nothing is stored")
    parser.add_argument("--document", type=Path, help="Write the coverage document to this file")
    args = parser.parse_args()
    matrix = load_matrix()
    census = mcp_census.load_census()
    acceptance = ACCEPTANCE_PATH.read_text(encoding="utf-8")
    problems = static_problems(matrix, census, acceptance)
    if args.document and not problems:
        args.document.write_text(render_document(matrix, census), encoding="utf-8")
    for problem in problems:
        print(f"revision matrix: {problem}")
    print(f"revision matrix: {len(matrix['tools'])} entries, {len(problems)} problems")
    return int(bool(problems))


if __name__ == "__main__":
    sys.exit(main())
