"""Provoke the refusals of every tool and require that no error text names a private place."""

from __future__ import annotations

import json
import os
import re
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_failure_cases import _fault_worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

PATH_WORDS = ("path", "directory", "file", "folder", "root", "location")
TOKEN = re.compile(r"[0-9a-f]{64}")
HOME_MARKER = "gzq-sentinel-home"


def _good_value(name: str, schema: dict[str, Any], outside: str) -> Any:
    if "enum" in schema:
        return schema["enum"][0]
    kind = schema.get("type")
    if kind == "string":
        return outside if any(word in name for word in PATH_WORDS) else f"unknown-{name}"
    if kind in {"integer", "number"}:
        return schema.get("minimum", 0) - 1
    return {"boolean": True, "array": [], "object": {}}.get(kind)


def _wrong_value(schema: dict[str, Any]) -> Any:
    return 12345 if schema.get("type") == "string" else "wrong-type"


def provocations(tool: dict[str, Any], outside: str) -> list[dict[str, Any]]:
    """Return the arguments that make one tool refuse: none, unknown values, wrong types.

    Args:
        tool: A catalog entry with its input schema.
        outside: A path outside every root the worker serves, as a caller would supply it.
    """
    properties: dict[str, Any] = tool["inputSchema"].get("properties", {})
    unknown = {name: _good_value(name, spec, outside) for name, spec in properties.items()}
    wrong = {name: _wrong_value(spec) for name, spec in properties.items()}
    return [{}, unknown, wrong, {**unknown, "unexpected_field": outside}]


def private_places(case: NativeAcceptance, home: Path) -> dict[str, str]:
    """Return what no error text may contain, by what it is."""
    places = {
        "the workspace": str(case.workspace),
        "the workspace as the system spells it": str(case.workspace.resolve()),
        "the user's home folder": str(Path.home()),
        "the worker's home folder": HOME_MARKER,
        "the application": str(case.native_executable.parent),
        "the worker's runtime folders": "runtime_",
    }
    if case.media_tools is not None:
        places["the media tools"] = str(case.media_tools)
    return {label: text for label, text in places.items() if text and text != "/"} | {
        "the private home": str(home)
    }


def _scan(label: str, text: str, places: dict[str, str]) -> list[str]:
    found = [f"{label} names {what}: {text[:300]}" for what, bit in places.items() if bit in text]
    found += [f"{label} carries a token-shaped string: {text[:300]}" for _ in TOKEN.findall(text)]
    return found


@acceptance_case
def refusals_never_name_private_paths_or_secrets(case: NativeAcceptance) -> None:
    """Provoke each tool's refusals; no error text or log line names a private place or token."""
    home = case.directory / HOME_MARKER
    home.mkdir()
    previous = os.environ.get("HOME")
    os.environ["HOME"] = str(home)
    case.addCleanup(
        lambda: os.environ.pop("HOME") if previous is None else os.environ.update(HOME=previous)
    )
    _fault_worker(case, "sweep")
    tools = case.capabilities()["tools"]
    outside = str(case.directory / "outside-everything" / "caller-supplied" / "item")
    places = private_places(case, home)
    problems: list[str] = []
    refused = 0
    for tool in tools:
        for index, params in enumerate(provocations(tool, outside)):
            response = case.worker.exchange(
                json.dumps({"id": "sweep", "method": tool["name"], "params": params}).encode()
            )
            if "error" not in response:
                continue
            refused += 1
            text = json.dumps(response["error"], ensure_ascii=False).replace(outside, "")
            problems += _scan(f"{tool['name']} #{index}", text, places)
    case.assertGreater(refused, 2 * len(tools), "the sweep provoked too few refusals")
    log = case.worker.log_path.read_text(encoding="utf-8", errors="replace").replace(outside, "")
    problems += _scan("the worker's stderr", log, places)
    case.assertEqual(problems, [], "\n".join(problems[:20]))
