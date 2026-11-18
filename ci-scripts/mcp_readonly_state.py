"""Read a document's whole native state through the read tools, and count what it holds."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

PAGE_KEYS = ("next_offset", "next_index")
PAGE_PARAMS = {"next_offset": "offset", "next_index": "start_index"}
MAX_PAGES = 64
PINNED_TOOLS = (
    "cast.list",
    "cast.usage",
    "scene.dependencies",
    "scene.validate",
    "motion_path.list",
    "audio.inspect",
    "storyboard.panels",
    "rig.inspect",
)
READ_ONLY_TOOLS = ("cast.list", "cast.usage", "scene.dependencies", "scene.validate")
COUNTED = ("stage objects", "effects", "levels", "columns", "palette styles", "palette pages")


def _read(case: NativeAcceptance, tool: str, params: dict[str, Any]) -> Any:
    response = case.worker.call(tool, params)
    if "error" in response:
        return {"error": response["error"]["code"]}
    return response["result"]


def _next_page(page: Any) -> tuple[str, Any] | None:
    if not isinstance(page, dict):
        return None
    for key in PAGE_KEYS:
        position = page.get(key)
        if position and position < page.get("total", position + 1):
            return PAGE_PARAMS[key], position
    return None


def _paged(case: NativeAcceptance, tool: str, params: dict[str, Any]) -> list[Any]:
    pages: list[Any] = []
    for _ in range(MAX_PAGES):
        pages.append(_read(case, tool, params))
        following = _next_page(pages[-1])
        if following is None:
            return pages
        params = {**params, following[0]: following[1]}
    raise AssertionError(f"{tool} returned more than {MAX_PAGES} pages")


def _drawing(case: NativeAcceptance, base: dict[str, Any], level: str, number: Any) -> dict:
    target = {**base, "level_id": level, "drawing_id": number}
    inspected = _read(case, "drawing.inspect", target)
    state: dict[str, Any] = {"inspect": inspected}
    if inspected.get("format") == "vector":
        state["structure"] = _read(case, "vector.structure", target)
        state["strokes"] = _paged(case, "vector.inspect", {**target, "limit": 50})
    elif "width" in inspected:
        rectangle = {"coordinate_space": inspected["coordinate_space"], "x": 0, "y": 0}
        rectangle.update(width=inspected["width"], height=inspected["height"])
        state["pixels"] = _read(case, "raster.read_pixels", {**target, **rectangle})
    return state


def _level(case: NativeAcceptance, base: dict[str, Any], pinned: dict, level: dict) -> dict:
    target = {**base, "level_id": level["level_id"]}
    return {
        "inspect": _read(case, "level.inspect", target),
        "palette": _paged(case, "palette.inspect", {**target, "limit": 50}),
        "pages": _read(case, "palette.pages", target),
        "hooks": _read(case, "hook.inspect", {**pinned, "level_id": level["level_id"]}),
        "drawings": [
            _drawing(case, base, level["level_id"], number) for number in level["drawings"]
        ],
    }


def _read_only_document(case: NativeAcceptance, entry: dict[str, str]) -> dict[str, Any]:
    base = {"document_id": entry["document_id"]}
    pinned = {**base, "expected_revision": entry["revision"]}
    summary = _read(case, "scene.summary", base)
    return {
        "summary": summary,
        "camera": _read(case, "camera.inspect", base),
        "graph": _read(case, "fx.graph", base),
        "pinned": {tool: _paged(case, tool, pinned) for tool in READ_ONLY_TOOLS},
        "levels": [
            {"inspect": _read(case, "level.inspect", {**base, "level_id": level["level_id"]})}
            for level in summary["levels"]
        ],
    }


def _document(case: NativeAcceptance, entry: dict[str, str]) -> dict[str, Any]:
    if entry.get("access") == "read_only":
        return _read_only_document(case, entry)
    base = {"document_id": entry["document_id"]}
    pinned = {**base, "expected_revision": entry["revision"]}
    summary = _read(case, "scene.summary", base)
    graph = _read(case, "fx.graph", base)
    length = max(1, summary["frame_count"])
    return {
        "summary": summary,
        "camera": _read(case, "camera.inspect", base),
        "graph": graph,
        "pinned": {tool: _paged(case, tool, pinned) for tool in PINNED_TOOLS},
        "levels": [_level(case, base, pinned, level) for level in summary["levels"]],
        "cells": {
            column["column_id"]: _read(
                case,
                "column.cells",
                {**base, "column_id": column["column_id"], "start_frame": 0, "length": length},
            )
            for column in summary["columns"]
        },
        "channels": {
            item["object_id"]: _read(
                case, "animation.channels", {**base, "object_id": item["object_id"]}
            )
            for item in summary["objects"]
        },
        "parameters": {
            item["fx_id"]: _read(case, "fx.parameter.get", {**base, "fx_id": item["fx_id"]})
            for item in graph["effects"]
        },
        "paths": [
            _read(case, "motion_path.inspect", {**pinned, "path_id": item["path_id"]})
            for page in _paged(case, "motion_path.list", pinned)
            for item in page["paths"]
        ],
    }


def read_state(case: NativeAcceptance) -> dict[str, Any]:
    """Read every open document through the read tools that describe native state.

    A read-only document is read through the tools its scene host answers.

    Args:
        case: The case whose worker holds the documents.
    Returns:
        The documents list and, per document id, summary, camera, FX graph and parameters,
        cast, dependencies, checks, motion paths, audio, panels, rig, and per level its
        palette, hooks and drawings, plus every column's cells and every object's channels.
    """
    documents = case.success("automation.list_documents", {})["documents"]
    state: dict[str, Any] = {"documents": documents}
    for entry in documents:
        state[entry["document_id"]] = _document(case, entry)
    return state


def counts(state: dict[str, Any]) -> dict[str, dict[str, int]]:
    """Count the objects each document holds, by the kinds a getter might create.

    Args:
        state: The result of read_state.
    Returns:
        Per document id, the count of each kind in COUNTED.
    """
    result: dict[str, dict[str, int]] = {}
    for entry in state["documents"]:
        document = state[entry["document_id"]]
        styles = pages = 0
        for level in document["levels"]:
            styles += sum(len(page.get("styles", [])) for page in level.get("palette", []))
            pages += len(level.get("pages", {}).get("pages", []))
        result[entry["document_id"]] = dict(
            zip(
                COUNTED,
                (
                    len(document["summary"]["objects"]),
                    len(document["graph"]["effects"]),
                    len(document["summary"]["levels"]),
                    len(document["summary"]["columns"]),
                    styles,
                    pages,
                ),
                strict=True,
            )
        )
    return result
