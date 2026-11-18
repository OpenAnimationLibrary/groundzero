"""Prove that every read tool leaves files, documents and native objects unchanged.

The harness is driven by the live catalog: each tool annotated read-only is called with the
representative arguments in mcp_readonly_calls.READ_CALLS, and a read tool without an entry
fails the check. Around each call it hashes every file under the workspace (which holds the
worker's private project and settings), the input root and every shared root, and compares
the documents' revisions. After each tool it also saves a copy of every document and reads the
whole native state back, so an object created on demand is seen as a change.
"""

from __future__ import annotations

import os
import shutil
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import TYPE_CHECKING, Any

import mcp_readonly_fixture
from mcp_readonly_calls import READ_CALLS, SCHEMA, Call
from mcp_readonly_state import counts, read_state
from mcp_readonly_tree import Cache, Tree, compare, snapshot

if TYPE_CHECKING:
    from collections.abc import Callable, Mapping, Sequence

    from mcp_acceptance import NativeAcceptance
    from mcp_readonly_fixture import Fixture

UNKNOWN_ARGUMENT = Call("unknown argument", {"unrecognized_acceptance_argument": True}, SCHEMA)
SETTLED = "after the tool's calls"
REPEATS = 2

PRIVATE_CACHES = (
    Cache(
        "workspace/runtime_*",
        "A read of a shared studio palette or Library file runs a parse helper that makes and "
        "removes its own scratch directory inside the worker's runtime directory, which "
        "changes only that directory's modification time; a scratch entry left behind is "
        "still reported as new",
        fields=("mtime_ns",),
    ),
)


@dataclass(frozen=True)
class KnownViolation:
    """A read that changes something, recorded so the suite is green only for this reason.

    Attributes:
        tool: The read tool.
        call: The label of the call, or SETTLED for a change seen after all its calls.
        contains: Text the reported change must contain.
        defect: The defect in the worker this entry stands for.
    """

    tool: str
    call: str
    contains: str
    defect: str


KNOWN_VIOLATIONS: tuple[KnownViolation, ...] = ()


@dataclass(frozen=True)
class Finding:
    """One observed change caused by a read."""

    tool: str
    call: str
    change: str

    def __str__(self) -> str:
        return f"{self.tool} [{self.call}]: {self.change}"


def read_tool_names(catalog: Sequence[Mapping[str, Any]]) -> list[str]:
    """Return the tools the catalog annotates as read-only, in catalog order."""
    return [tool["name"] for tool in catalog if tool["annotations"]["readOnlyHint"]]


def require_entries(names: Sequence[str], table: Mapping[str, object]) -> None:
    """Fail unless the table has an entry for every given read tool.

    Args:
        names: The read tools of the live catalog.
        table: The registered argument builders by tool name.
    """
    missing = [name for name in names if name not in table]
    if missing:
        raise AssertionError(
            f"Read tool(s) {missing} have no entry in READ_CALLS "
            "(ci-scripts/mcp_readonly_calls.py). "
            "Add a function returning a successful and a refused Call for each, then register it, "
            "so the harness proves the new tool leaves files and native state unchanged."
        )


def _digests(directory: Path) -> dict[str, tuple[str, int]]:
    tree = snapshot({"copy": directory})
    return {key: (entry.digest, entry.size) for key, entry in tree.items() if entry.kind == "file"}


def _differing(before: Mapping[str, Any], after: Mapping[str, Any]) -> list[str]:
    return [
        key for key in sorted(before.keys() | after.keys()) if before.get(key) != after.get(key)
    ]


def _listing_changes(
    before: Sequence[Mapping[str, Any]], after: Sequence[Mapping[str, Any]]
) -> list[str]:
    old = {item["document_id"]: item for item in before}
    new = {item["document_id"]: item for item in after}
    notes = []
    for key in _differing(old, new):
        if key not in old or key not in new:
            notes.append(f"document {key}: opened or closed during the call")
            continue
        notes.extend(
            f"document {key}: {field} went from {old[key].get(field)} to {new[key].get(field)}"
            for field in _differing(old[key], new[key])
        )
    return notes


@dataclass
class Harness:
    """Run read calls against a fixture and collect every change they cause."""

    fixture: Fixture
    caches: tuple[Cache, ...] = PRIVATE_CACHES
    findings: list[Finding] = field(default_factory=list)
    calls: int = 0
    tree: Tree = field(default_factory=dict)
    state: dict[str, Any] = field(default_factory=dict)
    copies: dict[str, dict[str, tuple[str, int]]] = field(default_factory=dict)
    generation: int = 0

    def start(self) -> None:
        """Take the baselines every later comparison is made against."""
        self.copies = self._save_copies(self.fixture.documents)
        self.state = read_state(self.fixture.case)
        self.tree = snapshot(self.fixture.roots)

    def _revisions(self) -> list[dict[str, str]]:
        return self.fixture.case.success("automation.list_documents", {})["documents"]

    def _save_copies(self, documents: Sequence[str]) -> dict[str, dict[str, tuple[str, int]]]:
        case = self.fixture.case
        root = case.workspace / "ground-truth"
        root.mkdir(exist_ok=True)
        self.generation += 1
        revisions = {item["document_id"]: item["revision"] for item in self._revisions()}
        copies = {}
        for index, document in enumerate(documents):
            directory = root / f"{self.generation}-{index}"
            case.operations += 1
            case.success(
                "scene.save_copy",
                {
                    "document_id": document,
                    "expected_revision": revisions[document],
                    "operation_id": f"ground-truth-{case.operations}",
                    "directory": str(directory),
                },
            )
            copies[document] = _digests(directory)
        for stale in root.glob(f"{self.generation - 1}-*"):
            shutil.rmtree(stale)
        return copies

    def _outcome(self, tool: str, call: Call, response: dict[str, Any]) -> None:
        case = self.fixture.case
        if call.refusal is None:
            case.assertNotIn("error", response, f"{tool} [{call.label}] was refused: {response}")
            return
        case.assertIn("error", response, f"{tool} [{call.label}] was not refused")
        expected = case.schema_code() if call.refusal == SCHEMA else call.refusal
        case.assertEqual(response["error"]["code"], expected, f"{tool} [{call.label}]")

    def call(self, tool: str, call: Call) -> None:
        """Call one tool, require its expected outcome, and record any change it caused."""
        before = self._revisions()
        response = self.fixture.worker.call(tool, call.params)
        self._outcome(tool, call, response)
        self.calls += 1
        after = snapshot(self.fixture.roots)
        notes = compare(self.tree, after, self.caches)
        notes.extend(_listing_changes(before, self._revisions()))
        self.tree = after
        self.findings.extend(Finding(tool, call.label, note) for note in notes)

    def check_tool(self, tool: str, calls: Sequence[Call] | None = None) -> None:
        """Call a tool with each of its calls, then compare copies and native state."""
        if calls is None:
            calls = [*READ_CALLS[tool](self.fixture), UNKNOWN_ARGUMENT]
        for repeat in range(REPEATS):
            for call in calls:
                again = replace(call, label=f"{call.label} (again)") if repeat else call
                self.call(tool, again)
        self._settle(tool)

    def _settle(self, tool: str) -> None:
        notes = self._compare_copies([self.fixture.document])
        saved = snapshot(self.fixture.roots)
        state = read_state(self.fixture.case)
        notes.extend(self._compare_state(state))
        reads = compare(saved, snapshot(self.fixture.roots), self.caches)
        notes.extend(f"the harness's own state reads changed files: {note}" for note in reads)
        self.state = state
        self.tree = snapshot(self.fixture.roots)
        self.findings.extend(Finding(tool, SETTLED, note) for note in notes)

    def _compare_copies(self, documents: Sequence[str]) -> list[str]:
        copies = self._save_copies(documents)
        notes = []
        for document, digests in copies.items():
            changed = _differing(self.copies[document], digests)
            notes.extend(f"saved copy of {document} differs at {key}" for key in changed)
        self.copies.update(copies)
        return notes

    def finish(self) -> None:
        """Compare a last copy of every document with the latest earlier copy of it."""
        notes = self._compare_copies(self.fixture.documents)
        self.findings.extend(Finding("whole run", SETTLED, note) for note in notes)

    def _compare_state(self, state: dict[str, Any]) -> list[str]:
        notes = []
        before, after = counts(self.state), counts(state)
        for document in before.keys() & after.keys():
            for kind, number in before[document].items():
                if after[document][kind] != number:
                    notes.append(
                        f"{document}: {kind} went from {number} to {after[document][kind]}"
                    )
        for key in _differing(self.state, state):
            if isinstance(self.state.get(key), dict) and isinstance(state.get(key), dict):
                for part in _differing(self.state[key], state[key]):
                    notes.append(f"native state of {key}: {part} changed")
            else:
                notes.append(f"native state: {key} changed")
        return notes

    def unexplained(self, known: Sequence[KnownViolation] = ()) -> list[str]:
        """Return findings that no known violation explains, and known entries that never fired."""
        remaining = list(self.findings)
        problems = []
        for entry in known:
            matched = [
                item
                for item in remaining
                if (item.tool, item.call) == (entry.tool, entry.call)
                and entry.contains in item.change
            ]
            if not matched:
                problems.append(f"known violation no longer reproduces, remove it: {entry}")
            remaining = [item for item in remaining if item not in matched]
        return [*(str(item) for item in remaining), *problems]


def _catalog(case: NativeAcceptance) -> list[dict[str, Any]]:
    return case.success("automation.capabilities", {})["tools"]


def every_read_tool_leaves_everything_unchanged(case: NativeAcceptance) -> None:
    """Call each catalog read tool with successful and refused arguments and require no change.

    Args:
        case: A fresh acceptance case; the harness replaces its worker.
    """
    names = read_tool_names(_catalog(case))
    require_entries(names, READ_CALLS)
    harness = Harness(mcp_readonly_fixture.build(case))
    harness.start()
    for name in names:
        harness.check_tool(name)
    harness.finish()
    case.assertEqual(harness.unexplained(KNOWN_VIOLATIONS), [])
    case.assertGreaterEqual(harness.calls, 4 * len(names))


def _writer(f: Fixture, **params: Any) -> dict[str, Any]:
    case = f.case
    case.operations += 1
    document = {"document_id": f.document, "expected_revision": f.revision(f.document)}
    return {**document, "operation_id": f"mislabelled-{case.operations}", **params}


def harness_catches_writing_tools_labelled_as_reads(case: NativeAcceptance) -> None:
    """Point the harness at writing tools as if they were reads and require it to object.

    Args:
        case: A fresh acceptance case; the harness replaces its worker.
    """
    fixture = mcp_readonly_fixture.build(case)
    exported = {"level_id": fixture.ids["rgba_level"], "drawing_id": {"number": 1, "suffix": ""}}
    exported["path"] = str(case.workspace / "mislabelled.png")
    writers = {
        "scene.settings": ({"width_px": 48}, "revision went from"),
        "raster.export_png": (exported, "new workspace/mislabelled.png"),
        "column.create": ({"name": "Created by a read"}, "columns went from"),
        "object.create": ({"kind": "pegbar", "name": "Created"}, "stage objects went from"),
        "fx.create": ({"type": "STD_glowFx"}, "effects went from"),
    }
    harness = Harness(fixture, PRIVATE_CACHES)
    harness.start()
    for tool, (params, expected) in writers.items():
        before = len(harness.findings)
        harness.check_tool(tool, [Call("mislabelled", _writer(fixture, **params))])
        reported = [item.change for item in harness.findings[before:]]
        case.assertTrue(any(expected in item for item in reported), (tool, expected, reported))
    _require_known_violations_to_be_exact(case, harness)


def _require_known_violations_to_be_exact(case: NativeAcceptance, harness: Harness) -> None:
    known = [
        KnownViolation(item.tool, item.call, item.change, "a writing tool labelled as a read")
        for item in harness.findings
    ]
    case.assertEqual(harness.unexplained(known), [])
    stale = KnownViolation("scene.summary", "main", "no such change", "a defect that was fixed")
    case.assertEqual(len(harness.unexplained([*known, stale])), 1)
    case.assertEqual(len(harness.unexplained(known[1:])), 1)


def requires_an_entry_for_each_read_tool(case: NativeAcceptance) -> None:
    """Fail a catalog read tool that has no entry, naming what to add, and ignore writers.

    Args:
        case: A fresh acceptance case.
    """
    names = read_tool_names(_catalog(case))
    case.assertIn("scene.summary", names)
    case.assertNotIn("scene.create", names)
    require_entries(names, READ_CALLS)
    with case.assertRaises(AssertionError) as raised:
        require_entries([*names, "invented.read"], READ_CALLS)
    case.assertIn("invented.read", str(raised.exception))
    case.assertIn("Add a function returning", str(raised.exception))
    case.assertEqual(read_tool_names([{"name": "x", "annotations": {"readOnlyHint": False}}]), [])


def _write(path: Path, data: bytes = b"data") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def tree_hashing_sees_every_kind_of_change(case: NativeAcceptance) -> None:
    """Require snapshots to report each way a file can change, and caches to be private.

    Args:
        case: A fresh acceptance case.
    """
    root = case.directory / "hashed"
    target = _write(root / "folder" / "file.bin")
    changes: dict[str, tuple[Callable[[], object], str]] = {
        "content": (
            lambda: target.write_bytes(b"DATA"),
            "changed workspace/folder/file.bin: content",
        ),
        "size": (lambda: target.write_bytes(b"longer"), "changed workspace/folder/file.bin: size"),
        "mode": (lambda: target.chmod(0o600), "changed workspace/folder/file.bin: mode"),
        "mtime": (lambda: os.utime(target, (1, 1)), "changed workspace/folder/file.bin: mtime_ns"),
        "new": (lambda: _write(root / "added"), "new workspace/added"),
        "removed": (target.unlink, "removed workspace/folder/file.bin"),
    }
    for name, (change, expected) in changes.items():
        before = snapshot({"workspace": root})
        case.assertEqual(compare(before, snapshot({"workspace": root})), [], name)
        change()
        case.assertIn(expected, compare(before, snapshot({"workspace": root})), name)
        shutil.rmtree(root)
        target = _write(root / "folder" / "file.bin")


def declared_caches_are_private_and_exact(case: NativeAcceptance) -> None:
    """Require caches to lie in private storage and to excuse only what they declare.

    Args:
        case: A fresh acceptance case.
    """
    for pattern in ("inputs/cache*", "library/templates/*", "workspace/runtime_x/*", "workspace/*"):
        with case.assertRaises(ValueError, msg=pattern):
            Cache(pattern, "a reason")
    with case.assertRaises(ValueError):
        Cache("workspace/runtime_*/cache*", "  ")
    with case.assertRaises(ValueError):
        Cache("workspace/runtime_*/cache*", "a reason", fields=("owner",))
    root = case.directory / "caches"
    runtime = root / "runtime_1"
    (runtime / "cache").mkdir(parents=True)
    roots = {"workspace": root, "inputs": case.inputs}
    before = snapshot(roots)
    _write(runtime / "cache" / "entry")
    _write(root / "elsewhere")
    _write(case.inputs / "cache" / "entry")
    notes = compare(before, snapshot(roots), [Cache("workspace/runtime_*/cache*", "a cache")])
    notes = [note for note in notes if not note.endswith("mtime_ns")]
    case.assertEqual(
        sorted(notes), ["new inputs/cache", "new inputs/cache/entry", "new workspace/elsewhere"]
    )
    scratch = [Cache("workspace/runtime_*", "scratch", fields=("mtime_ns",))]
    before = snapshot(roots)
    os.utime(runtime, (1, 1))
    case.assertEqual(compare(before, snapshot(roots), scratch), [])
    _write(runtime / "left behind")
    left = compare(before, snapshot(roots), scratch)
    case.assertIn("new workspace/runtime_1/left behind", left)
