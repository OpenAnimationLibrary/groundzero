"""Known violations: defects a test found in the product that no test change can fix.

A case that finds a real defect while the product code is frozen does not weaken its
expectation. It names the defect here, with the call, what a correct worker answers and what
this worker answers, and `settle` lets that one observation pass. The suite is therefore green
only for written-down reasons, and it fails in both directions:

- the worker answers something neither expected nor recorded: a new defect;
- the worker answers as expected although the entry is still listed: the defect is fixed, so
  delete its entry (the message names it).

`python3 ci-scripts/mcp_known_violations.py` prints the table and fails when an entry is
incomplete, or when an entry and the `settle` calls that name it do not match one to one.
"""

from __future__ import annotations

import ast
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

SCRIPTS = Path(__file__).resolve().parent
LOG_NAME = "known-violations.ndjson"


@dataclass(frozen=True)
class Violation:
    """One recorded defect.

    Attributes:
        name: What is wrong, in one sentence.
        tool: The tool whose answer is wrong.
        call: The arguments that show it, as a short description.
        expected: What a correct worker answers.
        observed: What this worker answers, in words.
        answers: The values `settle` accepts as this worker's wrong answer.
        row: The ledger rows that need the case that found it.
    """

    name: str
    tool: str
    call: str
    expected: str
    observed: str
    answers: frozenset[Any]
    row: str


KNOWN_VIOLATIONS: dict[str, Violation] = {
    "KV-9": Violation(
        name=(
            "animation.sample does not follow the Bezier of a speed segment whose frame axis is "
            "a quadratic in the curve parameter"
        ),
        tool="animation.set_keys, animation.sample",
        call=(
            "set_keys: key 6 value -70.5 speed_in_out outgoing {frame_delta 4, value_delta "
            "32.75}; key 12 value -5.75 linear incoming {kind speed, frame_delta 0, "
            "value_delta 34.5}; sample frames 7 to 11.9. The same happens with outgoing "
            "frame_delta 0 and incoming frame_delta -4 on a span of 6"
        ),
        expected=(
            "the cubic Bezier through (6, -70.5), (10, -37.75), (12, 28.75), (12, -5.75): about "
            "-61.26 at 7 and 2.85 at 11.9, as for every incoming frame_delta from -0.001 to -6"
        ),
        observed=(
            "-69.85 at 7, then -49.24 at 8 and 9 and -20.64 from 10 to 11.9: a stepped curve. "
            "It occurs where 3 times the outgoing frame_delta minus 3 times the incoming one "
            "equals twice the span, the cubic term of the frame axis; every other handle pair "
            "matches the Bezier to 1e-5"
        ),
        answers=frozenset({"does not match the Bezier"}),
        row="SCN-13 #1",
    ),
    "KV-10": Violation(
        name=(
            "A package saved into a folder whose name is not in Unicode composed form cannot be "
            "opened by the path the save reported"
        ),
        tool="scene.save_copy, scene.open",
        call=(
            "scene.save_copy into <workspace>/a/c + U+0301 (a base letter and a combining mark), "
            "then scene.open with the package_directory it returned"
        ),
        expected=(
            "the open succeeds: the path is spelled exactly as it is stored, which is what the "
            "refusal's own message asks for"
        ),
        observed=(
            'error code "path_denied" (no normalization variant of a name), from '
            "canonicalAncestor in automation_context.cpp, which compares the given path with "
            "QFileInfo::canonicalFilePath. Likely cause, not confirmed: Qt composes file names "
            "on macOS, so a decomposed name never equals its canonical form"
        ),
        answers=frozenset({"path_denied"}),
        row="QLT-04 #2 (path serialization)",
    ),
}


def settle(case: NativeAcceptance, name: str, observed: Any, expected: Any) -> None:
    """Accept the one wrong answer that a known violation records, and nothing else.

    Call it only for the input that shows the defect.

    Args:
        case: The running case.
        name: The key of the entry in KNOWN_VIOLATIONS that the answer shows.
        observed: What the worker answered (an error code, a flag, anything comparable).
        expected: What a correct worker answers: a value, or a set of acceptable values.
    Raises:
        AssertionError: When the answer is the expected one, so the entry is stale, or is
            neither the expected nor the recorded one, so it is a new defect.
    """
    violation = KNOWN_VIOLATIONS[name]
    acceptable = expected if isinstance(expected, set | frozenset) else {expected}
    if observed in acceptable:
        case.fail(
            f"{name} no longer reproduces ({observed!r} is the expected answer): delete its "
            "entry from KNOWN_VIOLATIONS in ci-scripts/mcp_known_violations.py"
        )
    if observed not in violation.answers:
        case.fail(
            f"{name}: {violation.tool} answered {observed!r}, which is neither the expected "
            f"{sorted(map(str, acceptable))} nor the recorded {sorted(map(str, violation.answers))}"
        )
    _log(case, name, observed)


def _log(case: NativeAcceptance, name: str, observed: Any) -> None:
    violation = KNOWN_VIOLATIONS[name]
    line = {
        "violation": name,
        "case": case.id().rsplit(".", 1)[-1],
        "tool": violation.tool,
        "observed": observed,
        "row": violation.row,
    }
    with (case.directory / LOG_NAME).open("a", encoding="utf-8") as log:
        log.write(json.dumps(line) + "\n")


def _settle_names(path: Path) -> set[str]:
    """Names passed to `settle` as its second argument in one source file."""
    found: set[str] = set()
    for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
        if not isinstance(node, ast.Call):
            continue
        callee = node.func
        called = callee.id if isinstance(callee, ast.Name) else getattr(callee, "attr", "")
        if called == "settle" and len(node.args) >= 2:
            argument = node.args[1]
            if isinstance(argument, ast.Constant) and isinstance(argument.value, str):
                found.add(argument.value)
    return found


def problems() -> list[str]:
    """Return what is wrong with the table: gaps in an entry and entries without a case."""
    found: list[str] = []
    used: set[str] = set()
    for path in sorted(SCRIPTS.glob("mcp_*.py")):
        if path.name != Path(__file__).name:
            used |= _settle_names(path)
    for name, entry in KNOWN_VIOLATIONS.items():
        for field in ("name", "tool", "call", "expected", "observed", "row"):
            if not getattr(entry, field).strip():
                found.append(f"{name}: field {field} is empty")
        if name not in used:
            found.append(f"{name}: no case passes it to settle(); delete the entry")
    for name in sorted(used - set(KNOWN_VIOLATIONS)):
        found.append(f"{name}: a case settles it but KNOWN_VIOLATIONS has no entry")
    return found


def main() -> int:
    """Print every known violation and check the table."""
    for name, entry in KNOWN_VIOLATIONS.items():
        print(f"{name} [{entry.row}] {entry.tool}\n  defect:   {entry.name}")
        print(f"  call:     {entry.call}\n  expected: {entry.expected}")
        print(f"  observed: {entry.observed}")
    wrong = problems()
    for problem in wrong:
        print(f"known violations: {problem}")
    return int(bool(wrong))


if __name__ == "__main__":
    sys.exit(main())
