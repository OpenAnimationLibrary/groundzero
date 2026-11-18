"""Check the worker's output schemas against the result keys its handlers write.

Every property an output schema names must be a key some automation source writes into a
JSON object; a name no handler writes is a typo or a removed field. `--report` also lists,
per tool, the keys its handler and the functions it calls in the same file write that the
tool's schema does not name, for a reviewer to place or dismiss.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "toonz" / "sources" / "toonz"
SCHEMAS = SOURCES / "automation_output_schemas.json"
KEY = r'"([a-z_][a-z0-9_]*)"'
WRITTEN = re.compile(rf"\{{\s*{KEY}\s*,|insert\(\s*{KEY}\s*,|\[\s*{KEY}\s*\]\s*=|{KEY}\s*:(?!:)")
HANDLER = re.compile(r'\{\s*"([a-z_]+(?:\.[a-z_]+)+)"\s*,\s*([A-Za-z_][A-Za-z0-9_:]*)\s*\}')
DEFINITION = r"\b{name}\s*\([^;{{]*\)\s*(?:const\s*)?\{{"
CALL = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def schema_keys(schema: Any) -> set[str]:
    """Return every property name a schema names, at any depth."""
    if isinstance(schema, list):
        return set().union(*(schema_keys(item) for item in schema))
    if not isinstance(schema, dict):
        return set()
    keys = set(schema.get("properties", {}))
    for nested in schema.get("properties", {}).values():
        keys |= schema_keys(nested)
    for word in ("items", "oneOf"):
        keys |= schema_keys(schema.get(word))
    return keys


def written_keys(text: str) -> set[str]:
    """Return the keys a source text writes into JSON objects, in code or embedded JSON."""
    return {next(group for group in match.groups() if group) for match in WRITTEN.finditer(text)}


def sources() -> dict[Path, str]:
    """Read every automation source file."""
    return {path: path.read_text(encoding="utf-8") for path in SOURCES.glob("automation_*.cpp")}


def unemitted(schemas: dict[str, Any], texts: dict[Path, str]) -> list[str]:
    """Name every schema property that no automation source writes."""
    written = set().union(*(written_keys(text) for text in texts.values()))
    return [
        f"{tool}: the output schema names {key}, which no automation source writes; "
        "remove it from automation_output_schemas.json or fix its name"
        for tool, schema in sorted(schemas.items())
        for key in sorted(schema_keys(schema) - written)
    ]


def body(text: str, name: str) -> str:
    """Return the body of the first function definition of name in text, or nothing."""
    match = re.search(DEFINITION.format(name=re.escape(name.split("::")[-1])), text)
    if match is None:
        return ""
    depth, index = 1, match.end()
    while depth and index < len(text):
        depth += {"{": 1, "}": -1}.get(text[index], 0)
        index += 1
    return text[match.end() : index]


def handler_keys(text: str, name: str) -> set[str]:
    """Return the keys a handler and the functions it calls in its own file write."""
    own = body(text, name)
    called = {call for call in CALL.findall(own) if call != name}
    return written_keys(own).union(*(written_keys(body(text, call)) for call in called))


def unlisted(schemas: dict[str, Any], texts: dict[Path, str]) -> dict[str, list[str]]:
    """Map each tool to the keys its handler writes that its schema does not name."""
    report: dict[str, list[str]] = {}
    for text in texts.values():
        for tool, handler in HANDLER.findall(text):
            if tool in schemas:
                missing = handler_keys(text, handler) - schema_keys(schemas[tool])
                if missing:
                    report[tool] = sorted(missing)
    return report


def main() -> int:
    """Check, or with --report also list, schema keys against handler keys."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--report", action="store_true")
    args = parser.parse_args()
    schemas = json.loads(SCHEMAS.read_text(encoding="utf-8"))
    texts = sources()
    problems = unemitted(schemas, texts)
    for problem in problems:
        print(f"schema keys: {problem}")
    if args.report:
        print(json.dumps(unlisted(schemas, texts), indent=1))
    print(f"schema keys: {len(schemas)} schemas, {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
