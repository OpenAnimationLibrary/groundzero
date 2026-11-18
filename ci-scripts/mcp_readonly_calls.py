"""Representative successful and refused arguments for every read tool.

Add an entry to READ_CALLS when the catalog gains a read tool: a function from the fixture
to its calls, with at least one successful call, and one refused call unless the tool takes
no arguments. Every read tool is also called once with an unknown argument, which the
harness adds itself.
"""

from __future__ import annotations

import re
from collections.abc import Callable
from dataclasses import dataclass, replace
from typing import Any

from mcp_readonly_fixture import VECTOR_DRAWING, Fixture

SCHEMA = "schema"
EITHER = "either"
ZERO = "00000000-0000-0000-0000-000000000000"
MISSING_DRAWING = {"number": 99, "suffix": ""}
SECTIONS = ("sheets", "columns", "stage_objects")
DRAWING_TOOLS = frozenset(
    {
        "drawing.inspect",
        "vector.inspect",
        "vector.structure",
        "vector.opacity.inspect",
        "raster.read_pixels",
    }
)
RECTANGLE = {
    "coordinate_space": "raster_pixels_lower_left",
    "x": 0,
    "y": 0,
    "width": 2,
    "height": 1,
}


@dataclass(frozen=True)
class Call:
    """One call of a read tool.

    Attributes:
        label: Short name used in reports and known-violation entries.
        params: The arguments.
        refusal: The error code the call must be refused with, SCHEMA for a violation of
            the advertised input schema, EITHER for a call whose outcome depends on the
            scene's content (it must succeed or be refused with a structured error), or
            None for a call that must succeed.
        reason: The `details.reason` a refusal must carry, when it names one.
        hint: What to do when a call that should be refused is not.
    """

    label: str
    params: dict[str, Any]
    refusal: str | None = None
    reason: str | None = None
    hint: str = ""


def _string(schema: dict[str, Any]) -> str:
    pattern = schema.get("pattern", ".*")
    for candidate in ("x", "0", ""):
        if len(candidate) >= schema.get("minLength", 0) and re.fullmatch(pattern, candidate):
            return candidate
    raise ValueError(f"No sample string for {schema}")


def _number(schema: dict[str, Any]) -> float:
    if "exclusiveMinimum" in schema:
        return schema.get("maximum", 1)
    return schema.get("minimum", 0)


def sample(schema: dict[str, Any]) -> Any:
    """Return the smallest simple value that satisfies a tool's input schema."""
    if "const" in schema:
        return schema["const"]
    if "enum" in schema:
        return schema["enum"][0]
    if "oneOf" in schema:
        return sample(schema["oneOf"][0])
    kind = schema.get("type")
    if kind == "object":
        return {name: sample(schema["properties"][name]) for name in schema.get("required", [])}
    if kind == "array":
        return [sample(schema["items"]) for _ in range(schema.get("minItems", 0))]
    if kind == "string":
        return _string(schema)
    if kind in {"integer", "number"}:
        return _number(schema)
    return False if kind == "boolean" else 0


def minimal_arguments(tool: dict[str, Any], document: str) -> dict[str, Any]:
    """Return arguments that satisfy a tool's schema, aimed at one document."""
    arguments = sample(tool["inputSchema"])
    arguments.update(document_id=document)
    properties = tool["inputSchema"]["properties"]
    if "expected_revision" in properties:
        arguments["expected_revision"] = "0"
    if "operation_id" in properties:
        arguments["operation_id"] = f"refused-{tool['name']}"
    return arguments


def _missing(kind: str) -> str:
    return f"{kind}_{ZERO}"


def _doc(f: Fixture, **extra: Any) -> dict[str, Any]:
    return {"document_id": f.document, **extra}


def _pinned(f: Fixture, document: str | None = None, **extra: Any) -> dict[str, Any]:
    document = document or f.document
    return {"document_id": document, "expected_revision": f.revision(document), **extra}


def _stale(f: Fixture, **extra: Any) -> dict[str, Any]:
    return {"document_id": f.document, "expected_revision": "0", **extra}


def _drawing(f: Fixture, level: str, **extra: Any) -> dict[str, Any]:
    return _doc(f, level_id=f.ids[level], drawing_id=VECTOR_DRAWING, **extra)


def _project_inspect(f: Fixture) -> list[Call]:
    return [
        Call("no arguments", {}),
        Call("project", {"project_id": f.ids["project_a"]}),
        Call("without a scenes folder", {"project_id": f.ids["project_shot"]}),
        Call("unknown project", {"project_id": "nonsense"}, "not_found"),
        Call("standalone", {"project_id": "standalone"}, "invalid_params"),
    ]


def _project_scenes(f: Fixture) -> list[Call]:
    project = {"project_id": f.ids["project_a"]}
    return [
        Call("project", project),
        Call("one at a time", {**project, "limit": 1}),
        Call("without a scenes folder", {"project_id": f.ids["project_shot"]}),
        Call("unknown project", {"project_id": "nonsense"}, "not_found"),
        Call("standalone", {"project_id": "standalone"}, "invalid_params"),
    ]


def _editable_inventory(f: Fixture) -> list[Call]:
    calls = []
    for name, document in (("main", f.document), ("reopened", f.reopened), ("bare", f.bare)):
        calls.extend(
            replace(call, label=f"{name}: {call.label}") for call in _inventory(f, document)
        )
    return calls


def _editable_source_status(f: Fixture) -> list[Call]:
    return [Call("editable", _pinned(f), "unsupported_target")]


def _no_arguments(_f: Fixture) -> list[Call]:
    return [Call("no arguments", {})]


def _projects(_f: Fixture) -> list[Call]:
    return [
        Call("roots", {}),
        Call("two levels", {"depth": 2, "limit": 1}),
        Call("outside every root", {"folder": "/etc"}, "path_denied"),
        Call("stale listing", {"expected_listing_revision": "listing:stale"}, "revision_conflict"),
    ]


def _runtime(_f: Fixture) -> list[Call]:
    return [Call("bounded", {}), Call("no probes", {"probes": []}, SCHEMA)]


def _preflight(f: Fixture) -> list[Call]:
    return [
        Call("preview", _pinned(f, kind="preview", frame=0, width=16, height=16), EITHER),
        Call("stale", _stale(f, kind="preview", frame=0, width=16, height=16), "stale_revision"),
    ]


def _describe(_f: Fixture) -> list[Call]:
    return [
        Call("two tools", {"names": ["scene.summary", "tool.describe"]}),
        Call("unknown tool", {"names": ["no.such.tool"]}, "unknown_tool"),
    ]


def _per_document(f: Fixture) -> list[Call]:
    return [
        Call("main", {"document_id": f.document}),
        Call("reopened", {"document_id": f.reopened}),
        Call("without content", {"document_id": f.bare}),
        Call("unknown", {"document_id": _missing("document")}, "document_not_found"),
    ]


def _level(f: Fixture) -> list[Call]:
    calls = [Call(name, _doc(f, level_id=f.ids[name])) for name in LEVELS]
    return [*calls, Call("unknown", _doc(f, level_id=_missing("level")), "invalid_handle")]


LEVELS = ("arm_level", "rgba_level", "toonz_level")


def _cells(f: Fixture) -> list[Call]:
    window = {"start_frame": 0, "length": 8}
    return [
        Call("level column", _doc(f, column_id=f.ids["arm_column"], **window)),
        Call("unknown", _doc(f, column_id=_missing("column"), **window), "invalid_handle"),
    ]


def _channels(f: Fixture) -> list[Call]:
    return [
        Call("pegbar", _doc(f, object_id=f.ids["peg"])),
        Call("column", _doc(f, object_id=f.ids["arm_object"])),
        Call("camera", _doc(f, object_id=f.ids["camera"])),
        Call("unknown", _doc(f, object_id=_missing("object")), "invalid_handle"),
    ]


def _channel(f: Fixture, **extra: Any) -> dict[str, Any]:
    return _doc(f, object_id=f.ids["peg"], channel="x", unit="inch", **extra)


def _keys(f: Fixture) -> list[Call]:
    missing = {**_channel(f), "object_id": _missing("object")}
    return [Call("pegbar x", _channel(f)), Call("unknown", missing, "invalid_handle")]


def _sample(f: Fixture) -> list[Call]:
    missing = {**_channel(f, frames=[0]), "object_id": _missing("object")}
    return [
        Call("fractional frames", _channel(f, frames=[0, 1.5, 9])),
        Call("unknown", missing, "invalid_handle"),
    ]


def _parameter(f: Fixture) -> list[Call]:
    return [
        Call("keyed", _doc(f, fx_id=f.ids["fx"], frame=2)),
        Call("unknown", _doc(f, fx_id=_missing("fx")), "invalid_handle"),
    ]


def _recipes(_f: Fixture) -> list[Call]:
    return [
        Call("all", {}),
        Call("light", {"category": "light"}),
        Call("unknown category", {"category": "no_such_category"}, SCHEMA),
    ]


def _drawings(f: Fixture) -> list[Call]:
    calls = [Call(name, _drawing(f, name)) for name in LEVELS]
    missing = _doc(f, level_id=f.ids["arm_level"], drawing_id=MISSING_DRAWING)
    return [*calls, Call("missing drawing", missing, "not_found")]


def _vector(f: Fixture) -> list[Call]:
    return [
        Call("vector", _drawing(f, "arm_level", limit=8)),
        Call("raster", _drawing(f, "rgba_level"), "unsupported_type"),
    ]


def _select(f: Fixture) -> list[Call]:
    query = {"mode": "nearest_centerline", "x": 8, "y": -5, "max_distance": 100}
    params = _drawing(f, "arm_level", coordinate_space="drawing_native", query=query)
    return [
        Call("nearest", {**params, "expected_revision": f.revision(f.document)}),
        Call("stale", {**params, "expected_revision": "0"}, "revision_conflict"),
    ]


def _opacity(f: Fixture) -> list[Call]:
    page = {"stroke_index": 0, "offset": 0, "limit": 8}
    params = _drawing(f, "arm_level", expected_revision=f.revision(f.document), **page)
    return [
        Call("first stroke", params),
        Call("stale", {**params, "expected_revision": "0"}, "revision_conflict"),
    ]


def _structure(f: Fixture) -> list[Call]:
    return [
        Call("vector", _drawing(f, "arm_level")),
        Call("raster", _drawing(f, "toonz_level"), "unsupported_type"),
    ]


def _palette_pages(f: Fixture, **page: Any) -> list[Call]:
    return [
        Call("vector", _doc(f, level_id=f.ids["arm_level"], **page)),
        Call("toonz raster", _doc(f, level_id=f.ids["toonz_level"])),
        Call("rgba", _doc(f, level_id=f.ids["rgba_level"]), "unsupported_type"),
        Call("unknown", _doc(f, level_id=_missing("level")), "invalid_handle"),
    ]


def _palette(f: Fixture) -> list[Call]:
    return _palette_pages(f, limit=2)


def _pages(f: Fixture) -> list[Call]:
    return _palette_pages(f)


def _pixels(f: Fixture) -> list[Call]:
    return [
        Call("toonz raster", _drawing(f, "toonz_level", **RECTANGLE)),
        Call("rgba", _drawing(f, "rgba_level", **RECTANGLE)),
        Call("vector", _drawing(f, "arm_level", **RECTANGLE), "invalid_params"),
    ]


def _job(f: Fixture) -> list[Call]:
    return [
        Call("completed", {"job_id": f.ids["job"]}),
        Call("of a read-only document", {"job_id": f.ids["read_only_job"]}),
        Call("unknown", {"job_id": _missing("job")}, "unknown_job"),
    ]


def _artifact(f: Fixture) -> list[Call]:
    return [
        Call("preview", {"artifact_id": f.ids["artifact"]}),
        Call("of a read-only document", {"artifact_id": f.ids["read_only_artifact"]}),
        Call("unknown", {"artifact_id": _missing("artifact")}, "unknown_artifact"),
    ]


def _package(f: Fixture) -> list[Call]:
    return [
        Call("saved package", {"path": str(f.package)}),
        Call("copy under an input root", {"path": str(f.input_package)}, "unregistered_package"),
        Call("outside every root", {"path": "/etc"}, "path_denied"),
    ]


def _pinned_with(stale_code: str) -> Callable[[Fixture], list[Call]]:
    def calls(f: Fixture) -> list[Call]:
        return [
            Call("main", _pinned(f)),
            Call("reopened", _pinned(f, f.reopened)),
            Call("without content", _pinned(f, f.bare)),
            Call("stale", _stale(f), stale_code),
        ]

    return calls


def _fonts(_f: Fixture) -> list[Call]:
    return [
        Call("prefix", {"family_prefix": "Arial", "offset": 0, "limit": 4}),
        Call(
            "beyond the end",
            {"family_prefix": "Arial", "offset": 4096, "limit": 4},
            "invalid_params",
        ),
        Call("empty page", {"family_prefix": "Arial", "offset": 0, "limit": 0}, SCHEMA),
    ]


def _path(f: Fixture) -> list[Call]:
    missing = _pinned(f, path_id=_missing("path"))
    return [
        Call("attached", _pinned(f, path_id=f.ids["motion_path"])),
        Call("unknown", missing, "invalid_handle"),
    ]


def _evaluate(f: Fixture) -> list[Call]:
    objects = [f.ids["arm_object"], f.ids["peg"]]
    missing = _pinned(f, object_ids=[_missing("object")], frames=[0])
    return [
        Call("column and pegbar", _pinned(f, object_ids=objects, frames=[0, 2])),
        Call("unknown", missing, "invalid_handle"),
    ]


def _hooks(f: Fixture) -> list[Call]:
    return [
        Call("level with hooks", _pinned(f, level_id=f.ids["arm_level"])),
        Call("level without hooks", _pinned(f, level_id=f.ids["rgba_level"])),
        Call("unknown", _pinned(f, level_id=_missing("level")), "invalid_handle"),
    ]


def _roots(_f: Fixture) -> list[Call]:
    return [Call("no arguments", {})]


def _studio_list(_f: Fixture) -> list[Call]:
    folder = {"root": "studio_palettes", "folder": "Global Palettes"}
    return [
        Call("studio", folder),
        Call("nested", {**folder, "folder": "Global Palettes/Warm"}),
        Call("project", {"root": "project_palettes", "folder": ""}),
        Call("missing", {**folder, "folder": "Absent"}, "not_found"),
    ]


def _studio_inspect(_f: Fixture) -> list[Call]:
    warm = {"root": "studio_palettes", "path": "Global Palettes/Warm.tpl"}
    return [
        Call("warm", {**warm, "limit": 2}),
        Call("read-only file", {**warm, "path": "Global Palettes/Warm/Deep/Frozen.tpl"}),
        Call("missing", {**warm, "path": "Global Palettes/Absent.tpl"}, "not_found"),
    ]


def _studio_find(_f: Fixture) -> list[Call]:
    return [
        Call("two roots", {"palette_id": "1700000000_1234567890123456789"}),
        Call("no match", {"palette_id": "nothing_1"}),
        Call("malformed id", {"palette_id": ""}, SCHEMA),
    ]


def _library_list(_f: Fixture) -> list[Call]:
    return [
        Call("templates", {"kind": "template"}),
        Call("poses", {"kind": "pose_set", "name_contains": "al"}),
        Call("stale listing", {"kind": "template", "expected_listing_revision": "x"}, SCHEMA),
    ]


def _library_inspect(_f: Fixture) -> list[Call]:
    return [
        Call("template", {"kind": "template", "name": "Rig"}),
        Call("template without level", {"kind": "template", "name": "Legacy"}),
        Call("pose set", {"kind": "pose_set", "name": "Walk"}),
        Call("missing", {"kind": "template", "name": "Absent"}, "not_found"),
    ]


def _library_thumbnail(_f: Fixture) -> list[Call]:
    return [
        Call("template", {"kind": "template", "name": "Rig"}),
        Call("pose", {"kind": "pose_set", "name": "Walk", "pose_index": 0}),
        Call("no pose", {"kind": "pose_set", "name": "Walk", "pose_index": 9}, "not_found"),
        Call("no icon", {"kind": "template", "name": "Legacy"}, "asset_unavailable"),
    ]


def _analysis(f: Fixture) -> list[Call]:
    page = {"offset": 0, "limit": 3}
    return [
        Call("first page", {"analysis_id": f.ids["analysis"], **page}),
        Call("unknown", {"analysis_id": _missing("analysis"), **page}, "invalid_handle"),
    ]


def _assistance(_f: Fixture) -> list[Call]:
    return [
        Call("all", {}),
        Call("cadmium", {"component": "cadmium"}),
        Call("unknown component", {"component": "other"}, SCHEMA),
    ]


def _ro_document(_f: Fixture, document: str) -> list[Call]:
    return [Call("document", {"document_id": document})]


def _ro_summary(f: Fixture, document: str) -> list[Call]:
    return [
        *_ro_document(f, document),
        Call("without drawings", {"document_id": document, "include_drawings": False}),
    ]


def _ro_levels(f: Fixture, document: str) -> list[Call]:
    levels = f.case.success("scene.summary", {"document_id": document})["levels"]
    calls = [
        Call(row["name"], {"document_id": document, "level_id": row["level_id"]}) for row in levels
    ]
    missing = {"document_id": document, "level_id": _missing("level")}
    return [*calls, Call("unknown", missing, "invalid_handle")]


def _ro_pinned(_f: Fixture, document: str) -> list[Call]:
    pinned = {"document_id": document, "expected_revision": "0"}
    return [
        Call("document", pinned),
        Call("stale", {**pinned, "expected_revision": "1"}, "stale_revision"),
    ]


def _ro_summary_of(f: Fixture, document: str) -> dict[str, Any]:
    return f.case.success("scene.summary", {"document_id": document})


def _ro_per_level(tool: str, f: Fixture, document: str, **extra: Any) -> list[Call]:
    calls = []
    for level in _ro_summary_of(f, document)["levels"]:
        drawing = level["drawings"][0] if level["drawings"] else VECTOR_DRAWING
        params = {"document_id": document, "level_id": level["level_id"], **extra}
        if tool in DRAWING_TOOLS:
            params["drawing_id"] = drawing
        calls.append(Call(level["name"], params, EITHER))
    return calls


def _ro_levels_with_drawings(f: Fixture, document: str) -> list[Call]:
    return _ro_per_level("drawing.inspect", f, document)


def _ro_palettes(f: Fixture, document: str) -> list[Call]:
    return _ro_per_level("palette.inspect", f, document)


def _ro_vectors(f: Fixture, document: str) -> list[Call]:
    return _ro_per_level("vector.inspect", f, document)


def _ro_opacity(f: Fixture, document: str) -> list[Call]:
    extra = {"expected_revision": "0", "stroke_index": 0, "offset": 0, "limit": 8}
    return _ro_per_level("vector.opacity.inspect", f, document, **extra)


def _ro_pixels(f: Fixture, document: str) -> list[Call]:
    return _ro_per_level("raster.read_pixels", f, document, **RECTANGLE)


def _ro_cells(f: Fixture, document: str) -> list[Call]:
    columns = _ro_summary_of(f, document)["columns"]
    window = {"document_id": document, "start_frame": 0, "length": 2}
    return [
        Call("first column", {**window, "column_id": columns[0]["column_id"]}),
        Call("unknown", {**window, "column_id": _missing("column")}, "invalid_handle"),
    ]


def _ro_object(f: Fixture, document: str) -> dict[str, str]:
    """The first column's stage object, or nothing where the scene file has no entry for it."""
    object_id = _ro_summary_of(f, document)["columns"][0]["object_id"]
    return {"document_id": document, "object_id": object_id} if object_id else {}


def _ro_channels(f: Fixture, document: str) -> list[Call]:
    given = _ro_object(f, document)
    missing = {"document_id": document, "object_id": _missing("object")}
    first = [Call("first column", given)] if given else []
    return [*first, Call("unknown", missing, "invalid_handle")]


def _ro_keys(f: Fixture, document: str) -> list[Call]:
    given = _ro_object(f, document)
    return [Call("x", {**given, "channel": "x", "unit": "inch"})] if given else []


def _ro_samples(f: Fixture, document: str) -> list[Call]:
    given = _ro_object(f, document)
    sample = {**given, "channel": "x", "unit": "inch", "frames": [0, 1.5]}
    return [Call("x", sample)] if given else []


def _ro_parameters(f: Fixture, document: str) -> list[Call]:
    effects = f.case.success("fx.graph", {"document_id": document})["effects"]
    calls = [
        Call(f"{row['type']} {index}", {"document_id": document, "fx_id": row["fx_id"]}, EITHER)
        for index, row in enumerate(effects)
    ]
    missing = {"document_id": document, "fx_id": _missing("fx")}
    return [*calls, Call("unknown", missing, "invalid_handle")]


def _inventory(f: Fixture, document: str) -> list[Call]:
    revision = f.revision(document)
    pinned = {"document_id": document, "expected_revision": revision}
    calls = [Call(section, {**pinned, "section": section}) for section in SECTIONS]
    stale = {**pinned, "expected_revision": "stale", "section": "sheets"}
    return [*calls, Call("stale", stale, "stale_revision")]


def _ro_source_status(_f: Fixture, document: str) -> list[Call]:
    pinned = {"document_id": document, "expected_revision": "0"}
    return [
        Call("unchanged", pinned),
        Call("stale", {**pinned, "expected_revision": "1"}, "stale_revision"),
    ]


READ_ONLY_CALLS: dict[str, Callable[[Fixture, str], list[Call]]] = {
    "scene.summary": _ro_summary,
    "camera.inspect": _ro_document,
    "level.inspect": _ro_levels,
    "cast.list": _ro_pinned,
    "cast.usage": _ro_pinned,
    "scene.dependencies": _ro_pinned,
    "scene.validate": _ro_pinned,
    "fx.graph": _ro_document,
    "scene.inventory": _inventory,
    "scene.source_status": _ro_source_status,
    "column.cells": _ro_cells,
    "drawing.inspect": _ro_levels_with_drawings,
    "palette.inspect": _ro_palettes,
    "palette.pages": _ro_palettes,
    "fx.parameter.get": _ro_parameters,
    "animation.channels": _ro_channels,
    "animation.keys": _ro_keys,
    "animation.sample": _ro_samples,
    "vector.inspect": _ro_vectors,
    "vector.structure": _ro_vectors,
    "vector.opacity.inspect": _ro_opacity,
    "raster.read_pixels": _ro_pixels,
}
ROUTED_WRITES = frozenset({"render.preview", "render.submit", "scene.discard"})


READ_CALLS: dict[str, Callable[[Fixture], list[Call]]] = {
    "automation.capabilities": _no_arguments,
    "automation.list_documents": _no_arguments,
    "project.inspect": _project_inspect,
    "project.scenes": _project_scenes,
    "scene.inventory": _editable_inventory,
    "scene.source_status": _editable_source_status,
    "project.list": _projects,
    "scene.summary": _per_document,
    "camera.inspect": _per_document,
    "level.inspect": _level,
    "column.cells": _cells,
    "animation.channels": _channels,
    "animation.keys": _keys,
    "animation.sample": _sample,
    "fx.catalog": _no_arguments,
    "fx.graph": _per_document,
    "fx.parameter.get": _parameter,
    "fx.recipe.list": _recipes,
    "drawing.inspect": _drawings,
    "vector.inspect": _vector,
    "vector.select": _select,
    "vector.opacity.inspect": _opacity,
    "vector.structure": _structure,
    "palette.inspect": _palette,
    "palette.pages": _pages,
    "raster.read_pixels": _pixels,
    "render.capabilities": _no_arguments,
    "render.preflight": _preflight,
    "runtime.inspect": _runtime,
    "tool.describe": _describe,
    "render.status": _job,
    "render.artifacts": _job,
    "artifact.read": _artifact,
    "scene.package_info": _package,
    "cast.list": _pinned_with("stale_revision"),
    "cast.usage": _pinned_with("stale_revision"),
    "scene.dependencies": _pinned_with("stale_revision"),
    "scene.validate": _pinned_with("stale_revision"),
    "text.fonts": _fonts,
    "motion_path.list": _pinned_with("revision_conflict"),
    "motion_path.inspect": _path,
    "audio.inspect": _pinned_with("revision_conflict"),
    "rig.inspect": _pinned_with("revision_conflict"),
    "rig.evaluate": _evaluate,
    "hook.inspect": _hooks,
    "shared.roots": _roots,
    "studio_palette.list": _studio_list,
    "studio_palette.inspect": _studio_inspect,
    "studio_palette.find": _studio_find,
    "library.list": _library_list,
    "library.inspect": _library_inspect,
    "library.thumbnail": _library_thumbnail,
    "storyboard.panels": _pinned_with("revision_conflict"),
    "lipsync.inspect": _analysis,
    "assistance.inspect": _assistance,
}
