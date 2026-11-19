"""Check that every writing tool changes the document revision exactly when native state changes.

The declarative table in `mcp_revision_matrix.json` lists each writing tool with a fixture, a
successful call, rejected calls, an optional no-op call and the reads that observe the change.
`run_tool` plays one entry against a worker and returns what differs from the rule. A defect that
is recorded in the table's known violations is the only difference the check accepts.

Table kinds:
    document: changes a document; the revision advances by one and a read changes.
    new_document: adds a document to the worker.
    read_only_document: adds a read-only document opened from a scene file; no revision moves.
    discard: removes a document from the worker.
    file: writes a file or a job result and leaves every document revision alone.
    worker_state: changes worker state that no document holds; documents stay as they were.
"""

from __future__ import annotations

import itertools
import json
import re
import time
import wave
from collections.abc import Iterator
from pathlib import Path
from typing import TYPE_CHECKING, Any, Protocol

import mcp_revision_matrix
from mcp_case import acceptance_case

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

MATRIX_PATH = Path(__file__).with_name("mcp_revision_matrix.json")
IGNORED_KEYS = frozenset({"revision", "source_revision", "expected_revision", "operation_id"})
VARIABLE = re.compile(r"\$\{([a-z_0-9]+)\}")
FONT_FAMILIES = ("Arial", "DejaVu Sans", "Liberation Sans")
DOCUMENT_KINDS = ("document", "file", "worker_state")
STALE_KINDS = ("document", "file", "discard")
TERMINAL_STATUSES = ("completed", "failed", "canceled", "deadline_exceeded")
SETTLE_SECONDS = 90
SCHEMA_CODE = "INVALID_ARGUMENTS"
Violation = dict[str, str]


class Caller(Protocol):
    """The worker calls the check needs, whether direct or through the adapter."""

    def call(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        """Send one request and return its response object."""
        ...


def load_matrix() -> dict[str, Any]:
    """Read the declarative revision table."""
    return json.loads(MATRIX_PATH.read_text(encoding="utf-8"))


def _strip(value: Any) -> Any:
    if isinstance(value, dict):
        return {k: _strip(v) for k, v in value.items() if k not in IGNORED_KEYS}
    if isinstance(value, list):
        return [_strip(item) for item in value]
    return value


def _lookup(value: Any, path: str) -> Any:
    for part in path.split("."):
        value = value[int(part)] if isinstance(value, list) else value[part]
    return value


def _outcome(response: dict[str, Any]) -> Any:
    return response.get("result", response.get("error"))


class Session:
    """One worker, the document under test, and the identifiers a table entry refers to."""

    def __init__(
        self,
        worker: Caller,
        schemas: dict[str, Any],
        variables: dict[str, Any],
        operations: Iterator[int],
        schema_code: str = SCHEMA_CODE,
    ) -> None:
        self.schema_code = schema_code
        self.worker = worker
        self.schemas = schemas
        self.variables = dict(variables)
        self.operations = operations
        self.document: str | None = None
        self.variables["serial"] = next(operations)

    def resolve(self, value: Any) -> Any:
        """Replace `$name` values and `${name}` fragments with saved variables."""
        if isinstance(value, dict):
            return {key: self.resolve(item) for key, item in value.items()}
        if isinstance(value, list):
            return [self.resolve(item) for item in value]
        if isinstance(value, str) and value.startswith("$") and value[1:] in self.variables:
            return self.variables[value[1:]]
        if isinstance(value, str):
            return VARIABLE.sub(lambda match: str(self.variables[match.group(1)]), value)
        return value

    def prepare(
        self, tool: str, args: dict[str, Any], operation: str | None = None
    ) -> dict[str, Any]:
        """Add the document, revision and operation identifiers the tool's schema asks for."""
        params = self.resolve(args)
        properties = self.schemas[tool]["inputSchema"].get("properties", {})
        if "document_id" in properties and "document_id" not in params and self.document:
            params["document_id"] = self.document
        if "expected_revision" in properties and "expected_revision" not in params:
            if params.get("document_id"):
                params["expected_revision"] = self.revision(params["document_id"])
        if "operation_id" in properties and "operation_id" not in params:
            params["operation_id"] = f"{operation or 'matrix'}-{next(self.operations)}"
        return params

    def listed(self) -> list[dict[str, Any]]:
        """Return the documents the worker lists."""
        return self.worker.call("automation.list_documents", {})["result"]["documents"]

    def revision(self, document: str | None = None) -> str:
        """Return a document's revision as the worker lists it."""
        document = document or self.document
        return next(e["revision"] for e in self.listed() if e["document_id"] == document)

    def send(self, tool: str, params: dict[str, Any]) -> dict[str, Any]:
        """Send prepared parameters and return the raw response."""
        return self.worker.call(tool, params)

    def run_step(self, step: dict[str, Any]) -> None:
        """Execute one fixture step, which must succeed, `repeat` times when it says so."""
        for _count in range(step.get("repeat", 1)):
            self._run_once(step)

    def _run_once(self, step: dict[str, Any]) -> None:
        if "file" in step:
            self._write_file(step)
        elif "font" in step:
            self.variables[step["font"]] = self._pick_font()
        else:
            self._call_step(step)

    def _call_step(self, step: dict[str, Any]) -> None:
        response = self.send(step["tool"], self.prepare(step["tool"], step.get("args", {})))
        if "result" not in response:
            raise AssertionError(f"fixture step {step['tool']} failed: {response}")
        result = response["result"]
        for variable, path in step.get("save", {}).items():
            self.variables[variable] = _lookup(result, path)
        if step["tool"] == "scene.create":
            self.document = result["document_id"]
        if step.get("select_document"):
            self.document = self.variables[step["select_document"]]

    def _write_file(self, step: dict[str, Any]) -> None:
        path = Path(self.variables["inputs"]) / step["file"]
        if step.get("kind") != "wav":
            path.write_text(step["text"], encoding="utf-8")
            return
        with wave.open(str(path), "wb") as sound:
            sound.setnchannels(1)
            sound.setsampwidth(2)
            sound.setframerate(48000)
            sound.writeframes(b"\x10\x00\xf0\xff" * 48000)

    def _pick_font(self) -> dict[str, Any]:
        for family in FONT_FAMILIES:
            page = self.send("text.fonts", {"family_prefix": family, "offset": 0, "limit": 64})
            for item in page["result"]["families"]:
                for style in ("Regular", "Book"):
                    if item["family"] == family and style in item["styles"]:
                        return {"family": family, "style": style, "size_points": 32}
        raise AssertionError("the text fixture needs Arial, DejaVu Sans or Liberation Sans")


def _violation(phase: str, expected: str, observed: str) -> Violation:
    return {"phase": phase, "expected": expected, "observed": observed}


def _error_code(response: dict[str, Any]) -> str:
    return f"error {response['error']['code']}"


def _bump(before: Any, after: Any) -> str:
    delta = int(after) - int(before)
    return "revision unchanged" if delta == 0 else f"revision advanced by {delta}"


def _reads(session: Session, entry: dict[str, Any], key: str = "reads") -> list[Any]:
    observed = []
    for read in entry.get(key, []):
        try:
            params = session.prepare(read["tool"], read.get("args", {}))
        except KeyError:
            observed.append(None)
            continue
        value = _outcome(session.send(read["tool"], params))
        if "pick" in read and isinstance(value, dict):
            value = value.get(read["pick"])
        observed.append(_strip(value))
    return observed


def _effect(session: Session, entry: dict[str, Any]) -> Any:
    effect = entry.get("effect")
    if effect is None:
        return None
    if "file" in effect:
        try:
            return Path(session.resolve(effect["file"])).exists()
        except KeyError:
            return False
    return _reads(session, {"effects": [effect["read"]]}, "effects")[0]


def _snapshot(session: Session, entry: dict[str, Any]) -> dict[str, Any]:
    kind = entry["kind"]
    return {
        "revision": session.revision() if kind in DOCUMENT_KINDS and session.document else None,
        "documents": len(session.listed()),
        "reads": _reads(session, entry),
        "effect": _effect(session, entry),
    }


def _difference(before: dict[str, Any], after: dict[str, Any]) -> list[str]:
    found = []
    if before["revision"] != after["revision"]:
        found.append(_bump(before["revision"], after["revision"]))
    if before["documents"] != after["documents"]:
        found.append("document count changed")
    if before["reads"] != after["reads"]:
        found.append("read changed")
    if before["effect"] != after["effect"]:
        found.append("effect changed")
    return found


def _reject(session: Session, entry: dict[str, Any]) -> list[Violation]:
    calls = [(item["args"], False, item.get("code")) for item in entry.get("reject", [])]
    if entry["kind"] in STALE_KINDS and entry.get("success"):
        calls.append((entry["success"]["args"], True, "revision_conflict"))
    found: list[Violation] = []
    for args, stale, code in calls:
        before = _snapshot(session, entry)
        params = session.prepare(entry["tool"], args)
        if stale and "expected_revision" in params:
            params["expected_revision"] = str(int(params["expected_revision"]) + 7)
        elif stale:
            continue
        response = session.send(entry["tool"], params)
        after = _snapshot(session, entry)
        found.extend(_reject_violations(response, code, session.schema_code, before, after))
    return found


def _reject_violations(
    response: dict[str, Any],
    code: str | list[str] | None,
    schema_code: str,
    before: dict[str, Any],
    after: dict[str, Any],
) -> list[Violation]:
    codes = [code] if isinstance(code, str) else list(code or [])
    expected = f"error {' or '.join(codes)} and no change".replace("error  ", "error ")
    if "result" in response:
        return [_violation("reject", expected, "call succeeded")]
    found = []
    allowed = {*codes, schema_code} if "invalid_params" in codes else set(codes)
    if codes and response["error"]["code"] not in allowed:
        found.append(_violation("reject", expected, _error_code(response)))
    found.extend(_violation("reject", expected, text) for text in _difference(before, after))
    return found


def _settle(session: Session, entry: dict[str, Any]) -> None:
    spec = entry.get("settle")
    if spec is None:
        return
    deadline = time.monotonic() + SETTLE_SECONDS
    while time.monotonic() < deadline:
        read = session.send(spec["tool"], session.prepare(spec["tool"], spec["args"]))
        if _outcome(read).get("status") in TERMINAL_STATUSES:
            return
        time.sleep(0.05)
    raise AssertionError(f"{entry['tool']}: the job did not finish within {SETTLE_SECONDS} s")


def _success(
    session: Session, entry: dict[str, Any]
) -> tuple[list[Violation], dict[str, Any], dict[str, Any]]:
    before = _snapshot(session, entry)
    params = session.prepare(entry["tool"], entry["success"]["args"], "matrix-success")
    response = session.send(entry["tool"], params)
    if "result" not in response:
        return [_violation("success", "call succeeds", _error_code(response))], params, response
    for variable, path in entry["success"].get("save", {}).items():
        session.variables[variable] = _lookup(response["result"], path)
    _settle(session, entry)
    after = _snapshot(session, entry)
    changes = _difference(before, after)
    wanted = _wanted(entry["kind"], entry)
    found = _report_problems("success", response, before, after)
    if changes != wanted:
        found.append(_violation("success", ", ".join(wanted), ", ".join(changes)))
    return found, params, response


def _report_problems(
    phase: str, response: dict[str, Any], before: dict[str, Any], after: dict[str, Any]
) -> list[Violation]:
    result = response.get("result", {})
    found = []
    if "revision" in result and after["revision"] is not None:
        if result["revision"] != after["revision"]:
            found.append(_violation(phase, "result revision equals the listed revision", "differs"))
    if isinstance(result.get("changed"), bool) and after["revision"] is not None:
        advanced = after["revision"] != before["revision"]
        if result["changed"] != advanced:
            found.append(
                _violation(phase, f"changed is {advanced}", f"changed is {result['changed']}")
            )
    return found


def _wanted(kind: str, entry: dict[str, Any]) -> list[str]:
    if kind == "document":
        return ["revision advanced by 1", "read changed"]
    if kind in {"new_document", "read_only_document", "discard"}:
        return ["document count changed"]
    return ["effect changed"] if entry.get("effect") else []


def _retry(
    session: Session, entry: dict[str, Any], params: dict[str, Any], response: dict[str, Any]
) -> list[Violation]:
    before = _snapshot(session, entry)
    again = session.send(entry["tool"], params)
    expected = "same result and no change"
    if _outcome(again) != _outcome(response):
        return [_violation("retry", expected, "result differs")]
    return [
        _violation("retry", expected, text)
        for text in _difference(before, _snapshot(session, entry))
    ]


def _late_retry(
    session: Session, entry: dict[str, Any], params: dict[str, Any], response: dict[str, Any]
) -> list[Violation]:
    if entry["kind"] != "document":
        return []
    bump = session.prepare("object.create", {"kind": "pegbar", "name": "Later"}, "matrix-later")
    if "result" not in session.send("object.create", bump):
        raise AssertionError("the late-retry write failed")
    before = _snapshot(session, entry)
    again = session.send(entry["tool"], params)
    expected = "same result and no change after a later write"
    if _outcome(again) != _outcome(response):
        return [_violation("late retry", expected, "result differs")]
    return [
        _violation("late retry", expected, text)
        for text in _difference(before, _snapshot(session, entry))
    ]


def _noop(session: Session, entry: dict[str, Any]) -> list[Violation]:
    noop = entry.get("noop")
    if noop is None:
        return []
    args = noop.get("args", entry["success"]["args"])
    before = _snapshot(session, entry)
    response = session.send(entry["tool"], session.prepare(entry["tool"], args))
    after = _snapshot(session, entry)
    changes = _difference(before, after)
    if noop["policy"] == "rejects":
        return _noop_rejected(response, noop.get("code"), changes)
    if "result" not in response:
        return [_violation("noop", "call succeeds", _error_code(response))]
    if noop["policy"] == "preserves":
        found = _report_problems("noop", response, before, after)
        return found + [_violation("noop", "no change", text) for text in changes]
    wanted = ["revision advanced by 1"]
    return [_violation("noop", ", ".join(wanted), ", ".join(changes))] if changes != wanted else []


def _noop_rejected(
    response: dict[str, Any], code: str | None, changes: list[str]
) -> list[Violation]:
    found = [_violation("noop", "no change", text) for text in changes]
    if "result" in response:
        found.append(_violation("noop", f"error {code}", "call succeeded"))
    elif code and response["error"]["code"] != code:
        found.append(_violation("noop", f"error {code}", _error_code(response)))
    return found


def discard_documents(session: Session) -> None:
    """Discard every document the worker holds so later entries stay under the document limit."""
    for listed in session.listed():
        params = session.prepare(
            "scene.discard",
            {"document_id": listed["document_id"], "expected_revision": listed["revision"]},
        )
        response = session.send("scene.discard", params)
        if "result" not in response:
            raise AssertionError(f"could not discard {listed['document_id']}: {response}")


def _open_fixture(session: Session, matrix: dict[str, Any], name: str) -> None:
    spec = matrix["fixtures"][name]
    if "extends" in spec:
        _open_fixture(session, matrix, spec["extends"])
    for step in spec["steps"]:
        session.run_step(step)


def run_tool(session: Session, matrix: dict[str, Any], tool: str) -> list[Violation]:
    """Play one table entry and return every way the tool breaks the revision rule."""
    entry = {"tool": tool, **matrix["tools"][tool]}
    _open_fixture(session, matrix, entry["fixture"])
    found = _reject(session, entry)
    if not entry.get("success"):
        return found
    problems, params, response = _success(session, entry)
    found.extend(problems)
    if "result" in response:
        found.extend(_retry(session, entry, params, response))
        found.extend(_noop(session, entry))
        found.extend(_late_retry(session, entry, params, response))
    return found


def known_violations(matrix: dict[str, Any], tool: str) -> list[Violation]:
    """Return the defects the table records for a tool."""
    return [
        _violation(item["phase"], item["expected"], item["observed"])
        for item in matrix["known_violations"]
        if item["entry"] == tool
    ]


def writing_tools(capabilities: dict[str, Any]) -> list[str]:
    """Name every worker tool the catalog does not annotate read-only."""
    return sorted(
        tool["name"] for tool in capabilities["tools"] if not tool["annotations"]["readOnlyHint"]
    )


@acceptance_case(name="revision_matrix_covers_the_catalog")
def check_catalog(case: NativeAcceptance) -> None:
    """Fail when the table and the running worker's writing tools disagree."""
    capabilities = case.capabilities()
    problems = mcp_revision_matrix.check_live(load_matrix(), capabilities)
    case.assertEqual(problems, [], "\n".join(problems))


def check_group(case: NativeAcceptance, group: str) -> None:
    """Run every entry of one group on this case's worker."""
    matrix = load_matrix()
    capabilities = case.capabilities()
    schemas = {tool["name"]: tool for tool in capabilities["tools"]}
    variables = {"workspace": str(case.workspace), "inputs": str(case.inputs)}
    variables["fixture"] = str(case.fixture)
    operations = itertools.count(1)
    for tool in sorted(t for t, e in matrix["tools"].items() if e["group"] == group):
        with case.subTest(tool=tool):
            session = Session(case.worker, schemas, variables, operations, case.schema_code())
            observed = run_tool(session, matrix, tool)
            discard_documents(session)
            case.assertEqual(observed, known_violations(matrix, tool), tool)
