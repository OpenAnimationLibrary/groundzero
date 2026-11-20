"""Check that every required output-schema field is written on every success path.

A field may be required only when its tool's handler, or a function it calls without a
condition in any automation source, writes the key outside any if, loop, switch, lambda or
conditional expression; or when a reviewer read the handler and lists the key under `read`
in automation_output_schema_proofs.json. Every tool with an output schema needs a review
entry, so a new tool cannot arrive with fields required by observation alone. `--relax`
drops every required field that is neither proven nor read, and records the entries.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

from mcp_schema_keys import CALL, DEFINITION, HANDLER, SCHEMAS, SOURCES, WRITTEN, sources

REVIEW = SOURCES / "automation_output_schema_proofs.json"
WRITE_KEYS = {"document_id", "revision", "operation_id"}
CONTROL = re.compile(r"(\bif|\bfor|\bwhile|\bswitch|\bcatch|\])\s*$")
GUARD = re.compile(r"\s*(if|else|for|while|case|default)\b")
STRING = re.compile(r'R"\((?:.|\n)*?\)"|"(?:\\.|[^"\\\n])*"|//[^\n]*')
METHOD = re.compile(
    r'method\s*==\s*"([a-z_]+(?:\.[a-z_]+)+)"\s*\)\s*return\s+'
    r"(?:[A-Za-z_][A-Za-z0-9_]*::)*(?:[A-Za-z_][A-Za-z0-9_]*(?:->|\.))?([A-Za-z_][A-Za-z0-9_]*)\("
)
DEPTH = 3


def masked(text: str) -> str:
    """Blank string literals and comments, keeping offsets, so braces are code only."""
    return STRING.sub(lambda match: " " * len(match.group(0)), text)


def _opening(code: str, close: int) -> int:
    depth = 0
    for index in range(close, -1, -1):
        depth += {")": 1, "(": -1}.get(code[index], 0)
        if depth == 0:
            return index
    return 0


def _control(code: str, brace: int) -> bool:
    before = code[:brace].rstrip()
    if before.endswith(("else", "do", "try", "mutable")):
        return True
    if not before.endswith(")"):
        return before.endswith("]")
    return bool(CONTROL.search(code[: _opening(code, len(before) - 1)].rstrip()))


def control_spans(code: str, start: int, end: int) -> list[tuple[int, int]]:
    """Return the spans of the control and lambda blocks between start and end."""
    spans, stack = [], []
    for index in range(start, end):
        if code[index] == "{":
            stack.append((index, _control(code, index)))
        elif code[index] == "}" and stack:
            opened, control = stack.pop()
            if control:
                spans.append((opened, index))
    return spans


def guarded(code: str, start: int, position: int) -> bool:
    """Whether the statement holding position starts with a condition or is a ternary."""
    statement = max(start, *(code.rfind(mark, start, position) + 1 for mark in ";{}"))
    segment = code[statement:position]
    return bool(GUARD.match(segment)) or "?" in segment


def _span(text: str, name: str) -> tuple[int, int] | None:
    match = re.search(DEFINITION.format(name=re.escape(name.split("::")[-1])), text)
    if match is None:
        return None
    depth, index = 1, match.end()
    while depth and index < len(text):
        depth += {"{": 1, "}": -1}.get(text[index], 0)
        index += 1
    return match.end(), index


Sources = list[tuple[str, str]]
_CACHE: dict[tuple[str, int, int], set[str]] = {}


def _definitions(files: Sources, name: str, home: int) -> list[tuple[int, int, int]]:
    """Return (file, start, end) of every definition a call from the home file may reach.

    A definition in the home file is the one called. Otherwise every other file that defines
    the name is a candidate, so the answer does not depend on the order the files were read.
    """
    span = _span(files[home][1], name)
    if span is not None:
        return [(home, *span)]
    found = []
    for index, (_text, code) in enumerate(files):
        span = _span(code, name)
        if span is not None:
            found.append((index, *span))
    return found


def unconditional(files: Sources, name: str, home: int, depth: int = DEPTH) -> set[str]:
    """Return the keys a function writes on every path, following unconditional calls.

    When the home file does not define the name and several other files do, only the keys
    that every one of those definitions writes count.

    Args:
        files: Every source text with its masked twin.
        name: The function.
        home: The index of the file whose definitions are preferred.
        depth: How many call levels to follow.
    """
    cache_key = (name, home, depth)
    if cache_key not in _CACHE:
        found = [_written(files, name, place, depth) for place in _definitions(files, name, home)]
        _CACHE[cache_key] = set.intersection(*found) if found else set()
    return _CACHE[cache_key]


def _written(files: Sources, name: str, place: tuple[int, int, int], depth: int) -> set[str]:
    """Return the keys one definition writes on every path, following unconditional calls."""
    home, start, end = place
    text, code = files[home]
    spans = control_spans(code, start, end)

    def plain(position: int) -> bool:
        inside = any(opened < position < closed for opened, closed in spans)
        return not inside and not guarded(code, start, position)

    keys = {
        next(group for group in match.groups() if group)
        for match in WRITTEN.finditer(text, start, end)
        if plain(match.start())
    }
    if depth > 0:
        for call in CALL.finditer(code, start, end):
            if call.group(1) != name and plain(call.start()):
                keys |= unconditional(files, call.group(1), home, depth - 1)
    return keys


def required_keys(schema: Any) -> set[str]:
    """Return every field name a schema requires, at any depth."""
    if isinstance(schema, list):
        return set().union(*(required_keys(item) for item in schema))
    if not isinstance(schema, dict):
        return set()
    keys = set(schema.get("required", []))
    for nested in schema.get("properties", {}).values():
        keys |= required_keys(nested)
    for word in ("items", "oneOf"):
        keys |= required_keys(schema.get(word))
    return keys


def proven(texts: dict[Path, str]) -> dict[str, tuple[str, set[str]]]:
    """Map each tool with a handler entry to its handler and its unconditional keys.

    A tool registered in more than one place (a table entry and a dispatcher that forwards)
    keeps only the keys every registration proves; its handler is the first by file name.
    """
    ordered = sorted(texts.items())
    files = [(text, masked(text)) for _path, text in ordered]
    result: dict[str, tuple[str, set[str]]] = {}
    for home, (text, _code) in enumerate(files):
        for tool, handler in [*HANDLER.findall(text), *METHOD.findall(text)]:
            keys = unconditional(files, handler, home)
            if tool in result:
                result[tool] = (result[tool][0], result[tool][1] & keys)
            else:
                result[tool] = (handler, keys)
    return result


def unproven(tool: str, schema: Any, found: tuple[str, set[str]] | None, entry: Any) -> set[str]:
    """Return the required fields of one tool that neither the tooling nor a reader proved."""
    keys = found[1] if found else set()
    read = set(entry.get("read", [])) if isinstance(entry, dict) else set()
    return required_keys(schema) - keys - read - WRITE_KEYS


def problems(schemas: dict[str, Any], review: dict[str, Any], texts: dict[Path, str]) -> list[str]:
    """Name every tool without a review entry and every unproven required field."""
    found = proven(texts)
    messages = []
    for tool, schema in sorted(schemas.items()):
        if tool not in review:
            messages.append(
                f"{tool}: no entry in automation_output_schema_proofs.json; read its handler "
                "and add one, listing under read the required fields you confirmed"
            )
            continue
        for key in sorted(unproven(tool, schema, found.get(tool), review[tool])):
            messages.append(
                f"{tool}: {key} is required but not written on every success path; make it "
                "optional or list it under read after checking the handler"
            )
    return messages


def relax(schema: Any, keep: set[str]) -> Any:
    """Return the schema with every required field outside keep made optional."""
    if isinstance(schema, list):
        return [relax(item, keep) for item in schema]
    if not isinstance(schema, dict):
        return schema
    result = dict(schema)
    if "required" in result:
        result["required"] = [key for key in result["required"] if key in keep]
    if "properties" in result:
        result["properties"] = {
            key: relax(value, keep) for key, value in result["properties"].items()
        }
    for word in ("items", "oneOf"):
        if word in result:
            result[word] = relax(result[word], keep)
    return result


def apply_relax(schemas: dict[str, Any], review: dict[str, Any], texts: dict[Path, str]) -> int:
    """Make unproven required fields optional, write both files, return the change count."""
    found = proven(texts)
    changed = 0
    for tool, schema in sorted(schemas.items()):
        entry = review.setdefault(tool, {"read": []})
        drop = unproven(tool, schema, found.get(tool), entry)
        changed += len(drop)
        schemas[tool] = relax(schema, required_keys(schema) - drop)
        entry["handler"] = found[tool][0] if tool in found else None
    SCHEMAS.write_text(json.dumps(schemas, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    REVIEW.write_text(json.dumps(review, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return changed


def main() -> int:
    """Check required fields, or with --relax apply the rule to the schema file."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--relax", action="store_true")
    args = parser.parse_args()
    schemas = json.loads(SCHEMAS.read_text(encoding="utf-8"))
    review = json.loads(REVIEW.read_text(encoding="utf-8")) if REVIEW.exists() else {}
    texts = sources()
    if args.relax:
        print(f"schema required: {apply_relax(schemas, review, texts)} required flags dropped")
        return 0
    messages = problems(schemas, review, texts)
    for message in messages:
        print(f"schema required: {message}")
    print(f"schema required: {len(schemas)} schemas, {len(messages)} problems")
    return 1 if messages else 0


if __name__ == "__main__":
    sys.exit(main())
