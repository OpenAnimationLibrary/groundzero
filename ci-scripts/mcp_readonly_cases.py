"""Prove that every read tool leaves files, documents and native objects unchanged.

The harness is driven by the live catalog: each tool annotated read-only is called with the
representative arguments in mcp_readonly_calls.READ_CALLS, and a read tool without an entry
fails the check. Around each call it hashes every file under the workspace (which holds the
worker's private project and settings), the input root and every shared root, and compares
the documents' revisions. After each tool it also saves a copy of the main document and reads
every open document's whole native state back, and a copy of every document is compared at the
start and the end, so an object created on demand is seen as a change.
"""

from __future__ import annotations

import json
import os
import shutil
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import TYPE_CHECKING, Any

import mcp_readonly_fixture
from mcp_case import acceptance_case
from mcp_readonly_calls import (
    EITHER,
    READ_CALLS,
    READ_ONLY_CALLS,
    ROUTED_WRITES,
    SCHEMA,
    Call,
    minimal_arguments,
)
from mcp_readonly_fixture import USER_LEVEL_NAMES
from mcp_readonly_state import counts, read_state
from mcp_readonly_tree import Cache, Tree, compare, snapshot, snapshot_named

if TYPE_CHECKING:
    from collections.abc import Callable, Mapping, Sequence

    from mcp_acceptance import NativeAcceptance
    from mcp_readonly_fixture import Fixture

UNKNOWN_ARGUMENT = Call("unknown argument", {"unrecognized_acceptance_argument": True}, SCHEMA)
SETTLED = "after the tool's calls"
LAST_PAGE = "last page"
READ_ONLY_SUFFIX = " on a read-only document"
PAGE_KEYS = ("next_offset", "next_index")
PAGE_ARGUMENTS = ("limit", "offset", "start_index")
REPEATS = 2

PRIVATE_CACHES = (
    Cache(
        "workspace/runtime_*",
        "A read of a shared studio palette or Library file, and a graphics probe of "
        "runtime.inspect, render.capabilities or render.preflight, runs a parse helper that "
        "makes and removes its own scratch directory inside the worker's runtime directory, which "
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
# Tools whose result does not hold every item at the largest limit. job.result pages analysis
# rows and a render job's artifacts come whole, without a paging key (jobs contract);
# automation.capabilities describes more tools than its largest page holds.
PAGED_BY_KIND = frozenset({"job.result", "automation.capabilities"})


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
        self.tree = self.fixture.watched()

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
        name = f"{tool} [{call.label}]"
        if call.refusal is None:
            case.assertNotIn("error", response, f"{name} was refused: {response}")
            return
        if call.refusal == EITHER:
            return
        case.assertIn("error", response, f"{name} was not refused. {call.hint}")
        error = response["error"]
        expected = case.schema_code() if call.refusal == SCHEMA else call.refusal
        case.assertEqual(error["code"], expected, name)
        if call.reason is not None:
            case.assertEqual(error["details"].get("reason"), call.reason, name)

    def call(self, tool: str, call: Call, suffix: str = "") -> dict[str, Any]:
        """Call one tool, require its expected outcome, and record any change it caused."""
        before = self._revisions()
        response = self.fixture.worker.call(tool, call.params)
        self._outcome(tool, call, response)
        self.calls += 1
        after = self.fixture.watched()
        notes = compare(self.tree, after, self.caches)
        notes.extend(_listing_changes(before, self._revisions()))
        self.tree = after
        self.findings.extend(Finding(tool + suffix, call.label, note) for note in notes)
        return response

    def check_tool(self, tool: str, calls: Sequence[Call] | None = None, suffix: str = "") -> None:
        """Call a tool with each of its calls, then compare copies and native state."""
        if calls is None:
            calls = [*READ_CALLS[tool](self.fixture), UNKNOWN_ARGUMENT]
        self.check_group(tool + suffix, [(tool, call) for call in calls], suffix)

    def check_group(self, name: str, pairs: Sequence[tuple[str, Call]], suffix: str = "") -> None:
        """Make each (tool, call) twice, then compare copies and native state once."""
        for repeat in range(REPEATS):
            for tool, call in pairs:
                again = replace(call, label=f"{call.label} (again)") if repeat else call
                self.call(tool, again, suffix)
        self._settle(name)

    def check_last_page(
        self, tool: str, calls: Sequence[Call], limit: int, suffix: str = ""
    ) -> None:
        """Require a response that holds every item to end its paging the way the others do."""
        for call in calls:
            if call.refusal is not None:
                continue
            response = self.call(
                tool, replace(call, params={**call.params, "limit": limit}), suffix
            )
            result = response["result"]
            keys = [key for key in PAGE_KEYS if key in result]
            if len(keys) != 1:
                note = f"the last page carries {keys or 'no paging key'}, not one of {PAGE_KEYS}"
            elif result[keys[0]] is not None:
                note = f"{keys[0]} is {result[keys[0]]} on the last page, not null"
            else:
                continue
            self.findings.append(Finding(tool + suffix, LAST_PAGE, f"{note} ({call.label})"))

    def _settle(self, tool: str) -> None:
        notes = self._compare_copies([self.fixture.document])
        saved = self.fixture.watched()
        state = read_state(self.fixture.case)
        notes.extend(self._compare_state(state))
        reads = compare(saved, self.fixture.watched(), self.caches)
        notes.extend(f"the harness's own state reads changed files: {note}" for note in reads)
        self.state = state
        self.tree = self.fixture.watched()
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


# Report-only: Claude Code saves a result past 50,000 characters to a file and refuses one past
# 25,000 tokens, so a read whose default result passes 64 KiB is listed for the paging work.
RESULT_REPORT_LIMIT = 64 * 1024
# automation.capabilities pages its descriptors, so its default page is held to a tighter bound.
CAPABILITIES_LIMIT = 32 * 1024
SCALED_COLUMNS = 300
SCALED_READS = ("scene.summary", "fx.graph")


def default_arguments(tool: Mapping[str, Any], params: Mapping[str, Any]) -> dict[str, Any]:
    """Drop the optional paging arguments from a call so the tool's own page size applies."""
    required = set(tool["inputSchema"].get("required", []))
    return {
        key: value for key, value in params.items() if key in required or key not in PAGE_ARGUMENTS
    }


def default_result_sizes(case: NativeAcceptance) -> dict[str, int]:
    """Return the serialized size in bytes of each read tool's largest default-paged result.

    Args:
        case: A fresh acceptance case; the fixture replaces its worker.
    """
    catalog = _catalog(case)
    by_name = {tool["name"]: tool for tool in catalog}
    fixture = mcp_readonly_fixture.build(case)
    sizes: dict[str, int] = {}
    for name in read_tool_names(catalog):
        for call in READ_CALLS[name](fixture):
            if call.refusal is not None:
                continue
            response = fixture.worker.call(name, default_arguments(by_name[name], call.params))
            case.assertNotIn("error", response, f"{name} [{call.label}] with defaults: {response}")
            text = json.dumps(response["result"], separators=(",", ":"), ensure_ascii=False)
            sizes[name] = max(sizes.get(name, 0), len(text.encode()))
    return sizes


def scaled_scene_sizes(case: NativeAcceptance) -> dict[str, int]:
    """Return the size of the whole-document reads of a scene with many columns."""
    document = case.create("Scaled")
    for index in range(SCALED_COLUMNS):
        case.mutate("column.create", document, name=f"Column {index}")
    sizes = {}
    for name in SCALED_READS:
        result = case.success(name, {"document_id": document})
        sizes[f"{name} ({SCALED_COLUMNS} columns)"] = len(
            json.dumps(result, separators=(",", ":"), ensure_ascii=False).encode()
        )
    return sizes


@acceptance_case
def report_default_read_result_sizes(case: NativeAcceptance) -> None:
    """List the reads whose default result passes 64 KiB, and bound automation.capabilities.

    The list is for information: it prints to the test output and fails nothing, except that
    the catalog's default page must stay within CAPABILITIES_LIMIT.

    Args:
        case: A fresh acceptance case; the fixture replaces its worker.
    """
    sizes = default_result_sizes(case)
    case.assertLessEqual(sizes["automation.capabilities"], CAPABILITIES_LIMIT)
    sizes.update(scaled_scene_sizes(case))
    oversized = {name: size for name, size in sizes.items() if size > RESULT_REPORT_LIMIT}
    largest = sorted(sizes.items(), key=lambda item: -item[1])[:8]
    print(f"\nREAD RESULTS OVER {RESULT_REPORT_LIMIT} BYTES: {oversized}; largest: {largest}")


def _catalog(case: NativeAcceptance) -> list[dict[str, Any]]:
    return case.capabilities()["tools"]


def _paged_limits(catalog: Sequence[Mapping[str, Any]]) -> dict[str, int]:
    limits = {}
    for tool in catalog:
        properties = tool["inputSchema"]["properties"]
        if tool["name"] in PAGED_BY_KIND:
            continue
        if tool["annotations"]["readOnlyHint"] and "limit" in properties:
            if "offset" in properties or "start_index" in properties:
                limits[tool["name"]] = properties["limit"]["maximum"]
    return limits


def _mutates(tool: Mapping[str, Any]) -> bool:
    """Whether a tool changes documents: it writes and is not declared a job start."""
    return not tool["annotations"]["readOnlyHint"] and tool["effects"]["job"] != "starts"


def _refusals_on_read_only(catalog: Sequence[dict[str, Any]], document: str) -> list:
    pairs = []
    for tool in catalog:
        name, properties = tool["name"], tool["inputSchema"]["properties"]
        if "document_id" not in properties or name in READ_ONLY_CALLS or name in ROUTED_WRITES:
            continue
        write = not tool["annotations"]["readOnlyHint"]
        hint = (
            f"{name} writes: add it to ROUTED_WRITES in mcp_readonly_calls.py if scene hosts "
            "now answer it, and cover its effects in the scene access cases"
            if write
            else f"{name} reads: add it to READ_ONLY_CALLS in mcp_readonly_calls.py"
        )
        reason = "mutation" if _mutates(tool) else "not_qualified"
        call = Call(
            "refused", minimal_arguments(tool, document), "read_only_document", reason, hint
        )
        pairs.append((name, call))
    return pairs


def check_read_only_documents(harness: Harness, catalog: Sequence[dict[str, Any]]) -> None:
    """Call the reads that scene hosts answer on every read-only document, and refuse the rest.

    Args:
        harness: A started harness whose fixture holds read-only documents.
        catalog: The live catalog.
    """
    fixture = harness.fixture
    limits = _paged_limits(catalog)
    for tool in read_tool_names(catalog):
        if tool not in READ_ONLY_CALLS:
            continue
        calls = [
            replace(call, label=f"{kind}: {call.label}")
            for kind, document in fixture.read_only.items()
            for call in READ_ONLY_CALLS[tool](fixture, document)
        ]
        harness.check_tool(tool, calls, READ_ONLY_SUFFIX)
        if tool in limits:
            harness.check_last_page(tool, calls, limits[tool], READ_ONLY_SUFFIX)
    pairs = _refusals_on_read_only(catalog, fixture.read_only["sheets"])
    harness.check_group("every other tool" + READ_ONLY_SUFFIX, pairs, READ_ONLY_SUFFIX)


@acceptance_case(name="every_read_tool_leaves_files_and_native_state_unchanged")
def every_read_tool_leaves_everything_unchanged(case: NativeAcceptance) -> None:
    """Call each catalog read tool with successful and refused arguments and require no change.

    Args:
        case: A fresh acceptance case; the harness replaces its worker.
    """
    catalog = _catalog(case)
    names = read_tool_names(catalog)
    require_entries(names, READ_CALLS)
    limits = _paged_limits(catalog)
    harness = Harness(mcp_readonly_fixture.build(case))
    harness.start()
    for name in names:
        harness.check_tool(name)
        if name in limits:
            harness.check_last_page(name, READ_CALLS[name](harness.fixture), limits[name])
    check_read_only_documents(harness, catalog)
    harness.finish()
    case.assertEqual(harness.unexplained(KNOWN_VIOLATIONS), [])
    case.assertGreaterEqual(harness.calls, 4 * len(names))


def _writer(f: Fixture, **params: Any) -> dict[str, Any]:
    case = f.case
    case.operations += 1
    document = {"document_id": f.document, "expected_revision": f.revision(f.document)}
    return {**document, "operation_id": f"mislabelled-{case.operations}", **params}


@acceptance_case(name="readonly_harness_catches_writing_tools_labelled_as_reads")
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
        "render.preview": (
            {
                "document_id": fixture.read_only["project"],
                "expected_revision": "0",
                "frame": 1,
                "width": 32,
                "height": 32,
            },
            "new workspace/",
        ),
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
    changes = {(item.tool, item.call, item.change) for item in harness.findings}
    known = [
        KnownViolation(tool, call, change, "a writing tool labelled as a read")
        for tool, call, change in sorted(changes, key=lambda item: -len(item[2]))
    ]
    case.assertEqual(harness.unexplained(known), [])
    stale = KnownViolation("scene.summary", "main", "no such change", "a defect that was fixed")
    case.assertEqual(len(harness.unexplained([*known, stale])), 1)
    revision = [entry for entry in known if "revision went from" in entry.contains]
    unexplained = harness.unexplained([entry for entry in known if entry not in revision])
    case.assertTrue(unexplained)
    case.assertTrue(all("revision went from" in line for line in unexplained), unexplained)


@acceptance_case(name="readonly_harness_requires_an_entry_for_each_read_tool")
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


@acceptance_case(name="readonly_tree_hashing_sees_every_kind_of_change")
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


@acceptance_case(name="readonly_user_level_files_are_watched_by_name")
def user_level_files_are_watched_by_name(case: NativeAcceptance) -> None:
    """Require entries of a shared user-level directory to be seen only when they match.

    Args:
        case: A fresh acceptance case.
    """
    shared = case.directory / "user-level"
    _write(shared / "GroundzeroAutomation" / "settings.ini")
    _write(shared / "Unrelated" / "file")
    watched = {"support": shared}
    before = snapshot_named(watched, USER_LEVEL_NAMES)
    case.assertEqual(
        sorted(before),
        ["support/GroundzeroAutomation/", "support/GroundzeroAutomation/settings.ini"],
    )
    _write(shared / "Unrelated" / "later")
    case.assertEqual(compare(before, snapshot_named(watched, USER_LEVEL_NAMES)), [])
    _write(shared / "GroundzeroAutomation" / "cache.bin")
    _write(shared / "com.GroundzeroAutomation.plist")
    notes = compare(before, snapshot_named(watched, USER_LEVEL_NAMES))
    case.assertIn("new support/GroundzeroAutomation/cache.bin", notes)
    case.assertIn("new support/com.GroundzeroAutomation.plist", notes)
    case.assertEqual(snapshot_named({"absent": shared / "none"}, USER_LEVEL_NAMES), {})


@acceptance_case(name="readonly_cache_declarations_are_private_and_exact")
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
