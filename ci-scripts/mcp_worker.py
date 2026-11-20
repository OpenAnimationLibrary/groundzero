"""Bounded NDJSON client for acceptance tests of the native automation worker."""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import tempfile
from collections.abc import Callable
from pathlib import Path
from queue import Empty, Queue
from threading import Thread
from typing import Any, BinaryIO, ClassVar

MAX_RESPONSE_BYTES = 16 * 1024 * 1024
SDK_CLIENT = "native-client.mjs"
DESCRIBE_BATCH = 16
FULL_GRANTS = "inspect,draft,file,job"
FULL_AUTHORITY = ("--grants", FULL_GRANTS, "--domains", "all")
KEEPS_REVISION = {"none", "reads", "opens_read_only"}
TYPES: dict[str, tuple[type, ...]] = {
    "object": (dict,),
    "array": (list,),
    "string": (str,),
    "boolean": (bool,),
    "null": (type(None),),
    "number": (int, float),
    "integer": (int, float),
}


class SchemaViolation(AssertionError):
    """A value outside a published schema, with the path of the first offending field."""


def _has_type(value: Any, kind: str) -> bool:
    if kind in ("number", "integer") and isinstance(value, bool):
        return False
    if not isinstance(value, TYPES[kind]):
        return False
    return kind != "integer" or float(value).is_integer()


def _check_choices(value: Any, schema: dict[str, Any], path: str) -> None:
    if "type" in schema and not _has_type(value, schema["type"]):
        raise SchemaViolation(f"{path}: expected type {schema['type']}")
    if "enum" in schema and value not in schema["enum"]:
        raise SchemaViolation(f"{path}: {value!r} is not in the enumeration")
    if "const" in schema and value != schema["const"]:
        raise SchemaViolation(f"{path}: {value!r} is not the constant {schema['const']!r}")


def _check_object(value: dict[str, Any], schema: dict[str, Any], path: str) -> None:
    properties = schema.get("properties", {})
    for key in schema.get("required", []):
        if key not in value:
            raise SchemaViolation(f"{path}: missing field {key}")
    for key, item in value.items():
        if key in properties:
            check_schema(item, properties[key], f"{path}.{key}")
        elif schema.get("additionalProperties") is False:
            raise SchemaViolation(f"{path}: unknown field {key}")


def _check_array(value: list[Any], schema: dict[str, Any], path: str) -> None:
    if not schema.get("minItems", 0) <= len(value) <= schema.get("maxItems", len(value)):
        raise SchemaViolation(f"{path}: {len(value)} items is outside the bounds")
    for index, item in enumerate(value):
        if "items" in schema:
            check_schema(item, schema["items"], f"{path}[{index}]")


def _check_scalar(value: Any, schema: dict[str, Any], path: str) -> None:
    if isinstance(value, str):
        if not schema.get("minLength", 0) <= len(value) <= schema.get("maxLength", len(value)):
            raise SchemaViolation(f"{path}: string length {len(value)} is outside the bounds")
        if "pattern" in schema and not re.fullmatch(schema["pattern"], value):
            raise SchemaViolation(f"{path}: string does not match {schema['pattern']}")
    elif isinstance(value, int | float) and not isinstance(value, bool):
        if not schema.get("minimum", value) <= value <= schema.get("maximum", value):
            raise SchemaViolation(f"{path}: {value} is outside the bounds")


def check_schema(value: Any, schema: dict[str, Any], path: str = "result") -> None:
    """Require a value to satisfy a schema in the worker's dialect.

    Args:
        value: Decoded JSON.
        schema: A schema using only the keywords the worker publishes.
        path: Where the value sits, for the message.
    Raises:
        SchemaViolation: Naming the first field outside the schema.
    """
    if "oneOf" in schema:
        matches = 0
        for branch in schema["oneOf"]:
            try:
                check_schema(value, branch, path)
                matches += 1
            except SchemaViolation:
                pass
        if matches != 1:
            raise SchemaViolation(f"{path}: matches {matches} of the oneOf shapes, not exactly one")
    _check_choices(value, schema, path)
    if isinstance(value, dict):
        _check_object(value, schema, path)
    elif isinstance(value, list):
        _check_array(value, schema, path)
    else:
        _check_scalar(value, schema, path)


def check_revision_effect(
    tool: dict[str, Any], params: dict[str, Any], result: dict[str, Any]
) -> None:
    """Require a tool declared not to write its document to keep the expected revision."""
    document = tool.get("effects", {}).get("document")
    expected = params.get("expected_revision")
    if expected is None or document not in KEEPS_REVISION or "revision" not in result:
        return
    if result["revision"] != expected:
        raise AssertionError(
            f"{tool['name']} declares document {document!r} but moved the revision "
            f"from {expected} to {result['revision']}"
        )


def require_declared_error(tool: dict[str, Any], code: Any) -> None:
    """Require a raw worker's error code to be one the tool declares.

    Raises:
        AssertionError: Naming the code and where to declare it.
    """
    if code not in tool["errors"]:
        raise AssertionError(
            f"{tool['name']} returned undeclared error code {code!r}: add it to the tool's "
            "row in toolDeclarations (automation_effects.cpp)"
        )


def _request(worker: Worker, method: str, params: dict[str, Any]) -> dict[str, Any]:
    payload = json.dumps({"id": "declarations", "method": method, "params": params}).encode()
    response = worker.exchange(payload)
    if "result" not in response:
        raise AssertionError(f"{method} failed while reading declarations: {response}")
    return response["result"]


def _described(worker: Worker) -> list[dict[str, Any]]:
    catalog = joined_capabilities(
        lambda params: _request(worker, "automation.capabilities", params)
    )
    names = [tool["name"] for tool in catalog["tools"]]
    described = []
    for start in range(0, len(names), DESCRIBE_BATCH):
        batch = names[start : start + DESCRIBE_BATCH]
        described.extend(_request(worker, "tool.describe", {"names": batch})["tools"])
    return described


def joined_capabilities(fetch: Callable[[dict[str, Any]], dict[str, Any]]) -> dict[str, Any]:
    """Return automation.capabilities with every page of tool descriptors joined.

    The descriptors carry their declarations (effects, errors, output schema), which a
    default page leaves out.

    Args:
        fetch: Calls automation.capabilities with the given paging arguments and returns its
            result.
    """
    result = fetch({"limit": 64, "declarations": True})
    tools = list(result["tools"])
    page = result
    while page["next_offset"] is not None:
        page = fetch({"limit": 64, "offset": page["next_offset"], "declarations": True})
        tools.extend(page["tools"])
    return {**result, "tools": tools, "next_offset": None}


class Worker:
    """Own one native process and retain its stderr for CI failure diagnosis.

    Every response is checked against the tool's published declarations: a result or error
    must match its outputSchema, and a raw worker's error code must be one of the tool's
    declared errors (through the SDK client the fixture checks the adapter's list).
    """

    declarations: ClassVar[dict[Path, dict[str, dict[str, Any]]]] = {}

    def __init__(
        self,
        executable: Path,
        workspace: Path,
        inputs: Path,
        log: Path,
        *,
        media_tools: Path | None = None,
        shared_roots: dict[str, Path] | None = None,
        shared_write: tuple[str, ...] = (),
        test_arguments: tuple[str, ...] = (),
        grants: str = FULL_GRANTS,
        domains: str = "all",
    ) -> None:
        self.sequence = 0
        self.executable = executable
        self.log_path = log
        # The native worker ends itself within 3 s of its input ending; the adapter in front
        # of it signals after 4 s and kills 2 s later, so an SDK client gets longer.
        self.exit_deadline = 8 if executable.name == SDK_CLIENT else 5
        self.errors = log.open("wb")
        self.responses: Queue[bytes | Exception] = Queue()
        arguments = [
            str(executable),
            "--automation-worker",
            "--workspace",
            str(workspace),
            "--input-root",
            str(inputs),
            "--grants",
            grants,
            "--domains",
            domains,
        ]
        if media_tools is not None:
            arguments.extend(["--media-tools", str(media_tools)])
        for role, root in (shared_roots or {}).items():
            arguments.extend(["--shared-root", f"{role}={root}"])
        for role in shared_write:
            arguments.extend(["--shared-write", role])
        arguments.extend(test_arguments)
        self.process = subprocess.Popen(
            arguments,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.errors,
        )
        assert self.process.stdout is not None
        self.reader = Thread(target=self._read, args=(self.process.stdout,), daemon=True)
        self.reader.start()

    def _read(self, stream: BinaryIO) -> None:
        try:
            while True:
                line = stream.readline(MAX_RESPONSE_BYTES + 1)
                if not line:
                    raise EOFError("Native worker closed stdout")
                if len(line) > MAX_RESPONSE_BYTES:
                    raise ValueError("Native worker exceeded the response size bound")
                self.responses.put(line)
        except (OSError, ValueError, EOFError) as error:
            self.responses.put(error)

    def exchange(self, payload: bytes, *, timeout: float = 30) -> dict[str, Any]:
        """Send one line and require one bounded JSON object within the deadline."""
        assert self.process.stdin is not None
        self.process.stdin.write(payload + b"\n")
        self.process.stdin.flush()
        try:
            response = self.responses.get(timeout=timeout)
        except Empty as error:
            raise TimeoutError(
                f"Native worker did not respond within {timeout}s: {self._diagnostic_tail()}"
            ) from error
        if isinstance(response, Exception):
            raise RuntimeError(f"Worker transport failed: {response}; {self._diagnostic_tail()}")
        result = json.loads(response)
        if not isinstance(result, dict):
            raise AssertionError(f"Worker response must be an object: {result!r}")
        if ("result" in result) == ("error" in result):
            raise AssertionError(f"Worker response must contain exactly one outcome: {result!r}")
        return result

    def _diagnostic_tail(self) -> str:
        with self.log_path.open("rb") as log:
            log.seek(0, 2)
            log.seek(max(0, log.tell() - 4096))
            return log.read().decode("utf-8", errors="replace")

    def call(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        """Call a native operation and require its matching request identifier."""
        self.sequence += 1
        identifier = f"acceptance-{self.sequence}"
        payload = json.dumps({"id": identifier, "method": method, "params": params}).encode()
        response = self.exchange(payload)
        if response.get("id") != identifier:
            raise AssertionError(f"Worker returned an unrelated request id: {response!r}")
        tool = self._catalog().get(method)
        if tool is not None:
            self._check_declared(tool, response)
            if "result" in response:
                check_revision_effect(tool, params, response["result"])
        return response

    def _descendants(self) -> list[int]:
        """The started process and every process under it, parents first."""
        listing = subprocess.run(
            ["ps", "-A", "-o", "pid=,ppid="], capture_output=True, text=True, check=False
        ).stdout
        children: dict[int, list[int]] = {}
        for line in listing.splitlines():
            fields = line.split()
            if len(fields) == 2:
                children.setdefault(int(fields[1]), []).append(int(fields[0]))
        tree, index = [self.process.pid], 0
        while index < len(tree):
            tree.extend(children.get(tree[index], []))
            index += 1
        return tree

    def _check_declared(self, tool: dict[str, Any], response: dict[str, Any]) -> None:
        method = tool["name"]
        if "error" in response:
            content = {"error": response["error"]}
            require_declared_error(tool, response["error"].get("code"))
        else:
            content = {key: value for key, value in response["result"].items() if key != "image"}
        if "outputSchema" in tool:
            try:
                check_schema(content, tool["outputSchema"])
            except SchemaViolation as error:
                raise SchemaViolation(f"{method} broke its outputSchema: {error}") from None
        details = response["error"]["details"] if "error" in response else content
        if "rollback" in details:
            raise AssertionError(f"{method} claims a rollback, which no tool declares")

    def _catalog(self) -> dict[str, dict[str, Any]]:
        """Read every tool's published declaration once per executable from a private worker.

        The names come from the capabilities and the declarations from tool.describe, so
        through the SDK client the error lists include the adapter's own codes.
        """
        if self.executable not in Worker.declarations:
            with tempfile.TemporaryDirectory(prefix="declarations-") as directory:
                root = Path(directory).resolve(strict=True)
                (root / "workspace").mkdir()
                (root / "inputs").mkdir()
                probe = Worker(self.executable, root / "workspace", root / "inputs", root / "log")
                try:
                    tools = _described(probe)
                finally:
                    probe.close()
            Worker.declarations[self.executable] = {tool["name"]: tool for tool in tools}
        return Worker.declarations[self.executable]

    def _record_stalled_exit(self) -> None:
        """Save a stack sample of every process under a worker that did not exit.

        Through the SDK client the started process is the client, so the adapter, its
        supervisor and the native worker under them are sampled too.
        """
        sampler = shutil.which("sample")
        if sampler is None:
            # The shard runner counts these files; without a sampler the file says so.
            report = self.log_path.with_suffix(".stalled-exit.txt")
            report.write_text(f"process {self.process.pid} missed its exit deadline; no sampler\n")
            return
        for pid in self._descendants():
            suffix = "" if pid == self.process.pid else f".{pid}"
            report = self.log_path.with_suffix(f".stalled-exit{suffix}.txt")
            subprocess.run(
                [sampler, str(pid), "2", "-file", str(report)],
                capture_output=True,
                check=False,
                timeout=30,
            )

    def close(self) -> None:
        """Close the transport and reap the original native process."""
        if self.process.stdin is not None:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=self.exit_deadline)
        except subprocess.TimeoutExpired:
            self._record_stalled_exit()
            self.process.kill()
            self.process.wait(timeout=5)
        self.reader.join(timeout=5)
        if self.process.stdout is not None:
            self.process.stdout.close()
        self.errors.close()
