"""Representative successful and refused arguments for every read tool.

Add an entry to READ_CALLS when the catalog gains a read tool: a function from the fixture
to its calls, with at least one successful call, and one refused call unless the tool takes
no arguments. Every read tool is also called once with an unknown argument, which the
harness adds itself.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from mcp_readonly_fixture import VECTOR_DRAWING, Fixture

SCHEMA = "schema"
ZERO = "00000000-0000-0000-0000-000000000000"
MISSING_DRAWING = {"number": 99, "suffix": ""}
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
            the advertised input schema, or None for a call that must succeed.
    """

    label: str
    params: dict[str, Any]
    refusal: str | None = None


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


def _no_arguments(_f: Fixture) -> list[Call]:
    return [Call("no arguments", {})]


def _projects(_f: Fixture) -> list[Call]:
    return [
        Call("roots", {}),
        Call("two levels", {"depth": 2, "limit": 1}),
        Call("outside every root", {"folder": "/etc"}, "path_denied"),
        Call("stale listing", {"expected_listing_revision": "listing:stale"}, "revision_conflict"),
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
        Call("unknown", {"job_id": _missing("job")}, "unknown_job"),
    ]


def _artifact(f: Fixture) -> list[Call]:
    return [
        Call("preview", {"artifact_id": f.ids["artifact"]}),
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
        Call("prefix", {"family_prefix": "Ar", "offset": 0, "limit": 4}),
        Call(
            "beyond the end", {"family_prefix": "Ar", "offset": 4096, "limit": 4}, "invalid_params"
        ),
        Call("empty page", {"family_prefix": "Ar", "offset": 0, "limit": 0}, SCHEMA),
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


READ_CALLS: dict[str, Callable[[Fixture], list[Call]]] = {
    "automation.capabilities": _no_arguments,
    "automation.list_documents": _no_arguments,
    "project.inspect": _no_arguments,
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
