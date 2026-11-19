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
    "KV-1": Violation(
        name=(
            "An exposure or read interval that passes the schema but runs past frame 1000000 "
            "is refused with `invalid_argument`, a code the tool does not declare"
        ),
        tool="column.expose, column.cells",
        call=(
            "column.expose {start_frame: 999999, length: 3} (the same for column.cells); "
            "start_frame and length are each inside their schema bounds"
        ),
        expected=(
            "an error code the tool declares in its effect declaration (`invalid_params` or "
            "`resource_limit`), so a client that checks declared codes can handle it"
        ),
        observed=(
            'error code "invalid_argument", message "The exposure interval exceeds the '
            'supported frame range."; the tool declares neither'
        ),
        answers=frozenset({"invalid_argument"}),
        row="SCN-07 F1",
    ),
    "KV-2": Violation(
        name=(
            "animation.copy_curve refuses a channel given with a unit the channel does not "
            "have using `invalid_unit`, a code the tool does not declare"
        ),
        tool="animation.copy_curve",
        call=(
            "destination {channel: z, unit: inch} (or any channel with a unit that is not its "
            "own) with a matching source"
        ),
        expected=(
            "an error code animation.copy_curve declares (`invalid_params`), as for every "
            "other incompatible pair; animation.set_keys declares `invalid_unit`, copy_curve "
            "does not"
        ),
        observed='error code "invalid_unit"',
        answers=frozenset({"invalid_unit"}),
        row="SCN-13 F4",
    ),
    "KV-3": Violation(
        name=(
            "The snapshot a render job copied from a discarded draft stays on disk after the "
            "job has ended"
        ),
        tool="render.submit, job.status",
        call=(
            "render.submit on a draft, scene.discard of that draft while the job runs, wait for "
            "the job to complete, list renders/<job>/snapshot"
        ),
        expected=(
            "the snapshot (scene.tnz, manifest.json and assets) is reclaimed once the job is "
            "terminal, since the draft that owned it is gone and the frames are published "
            "separately (SCN-03 #3: remains alive until the worker snapshot is complete, then "
            "is reclaimed)"
        ),
        observed=(
            "renders/<job>/snapshot/ remains until a later worker's start sweeps the job "
            "folder (mcp_failure_cases.finished_render_folder_is_swept_once_its_worker_is_gone "
            "shows the folder lives as long as its worker); the private level copies of the "
            "draft are reclaimed at the discard"
        ),
        answers=frozenset({True}),
        row="SCN-03 #3",
    ),
    "KV-4": Violation(
        name=(
            "A read-only scene that exposes a level whose name another level also has is listed "
            "with a level a second time"
        ),
        tool="cast.list, scene.dependencies, scene.open_read_only",
        call=(
            "scene.open_read_only on the base scene (levels Ink, Cel, Paint) plus a fourth level "
            "named Ink with its own level and cast id, exposed in the first column; then "
            "cast.list and scene.dependencies on the document (the same with three new levels "
            "named Zed, the first exposed)"
        ),
        expected=(
            "one row per defined level (4), each level_id once, and one primary dependency row "
            "per level; the counts of scene.open_read_only equal the defined levels"
        ),
        observed=(
            "5 cast rows: the first level of that name (the base Ink; the first Zed) appears twice "
            "with the same level_id and native_cast_id, scene.dependencies repeats its owner, and "
            "counts.levels is 5; the same levels left unexposed are listed correctly"
        ),
        answers=frozenset({(1, 1)}),
        row="SCN-05 F3",
    ),
    "KV-5": Violation(
        name=(
            "A name longer than 128 characters, or with a NUL character, is refused with "
            "`invalid_argument`, a code the tool does not declare, and its schema states no limit"
        ),
        tool="level.create, column.create, object.create",
        call=(
            "column.create {name: 129 x 'm'}; the same with name 'a' NUL 'b'; and with "
            "level.create and object.create"
        ),
        expected=(
            "a code the tool declares, or a schema bound that the adapter reports (`maxLength` "
            "128 on name), so a client can see the limit before it calls"
        ),
        observed='error code "invalid_argument"; the name property is {minLength: 1, type: string}',
        answers=frozenset({"invalid_argument"}),
        row="QLT-04 #2 (identifier serialization)",
    ),
    "KV-6": Violation(
        name="The camera size in inches is stored with six digits, so a save changes it",
        tool="scene.save_copy, scene.open, camera.inspect",
        call=(
            "scene.create {width_px: 32, height_px: 32, width_in: 32/120, height_in: 32/120}, "
            "expose a cell, scene.save_copy, scene.open, camera.inspect"
        ),
        expected="width_in 0.26666666666666666 (equal within 1e-9), as before the save",
        observed=(
            "width_in 0.266667; the pixel size is kept, so the saved scene's pixels per inch change"
        ),
        answers=frozenset({"0.266667"}),
        row="QLT-04 #2 (unit serialization)",
    ),
    "KV-7": Violation(
        name="scene.summary keeps the old frame count after the only column with cells is deleted",
        tool="column.delete, scene.summary",
        call=(
            "column.create, column.expose {start_frame: 0, length: 22}, column.delete, "
            "scene.summary"
        ),
        expected="frame_count 0 (no column holds a cell); it was 22 before the deletion",
        observed=(
            "frame_count stays 22 until a later edit recomputes it (generated sequences found "
            "it; they no longer delete a column that holds cells)"
        ),
        answers=frozenset({22}),
        row="QLT-04 #2 (frame serialization)",
    ),
    "KV-8": Violation(
        name=(
            "A scene with exposures at row 0 and at row 1000000, both admitted by column.expose, "
            "cannot be saved"
        ),
        tool="column.expose, scene.save_copy",
        call=(
            "column.expose at start_frame 0 and at start_frame 1000000 (length 1) in one "
            "column, then scene.save_copy"
        ),
        expected=(
            "the save succeeds, or column.expose refuses row 1000000; the tools agree on the "
            "last row (a column's span may be 1000001 rows)"
        ),
        observed=(
            'error code "scene_limit", "The scene exceeds the supported exposure limit."; the '
            "save counts last row minus first row plus one against 1000000, while column.expose "
            "admits rows up to 1000000"
        ),
        answers=frozenset({"scene_limit"}),
        row="SCN-07 F1",
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
