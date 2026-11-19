"""Every read tool and every tool without input refuses what it must refuse, with its code.

Each call below names a tool and arguments it cannot serve (an unknown document or handle, a
value outside its enumeration, an unknown field) and requires a typed error. The calls exist so
that no released tool is without a negative test (`mcp_fx_matrix.py` fails when one is).
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

UNKNOWN_DOCUMENT = "document_00000000_unknown"
ZERO = {"expected_revision": "0"}
DRAWING = {"number": 1, "suffix": ""}


def _refused(case: NativeAcceptance, tool: str, params: dict[str, Any], code: str) -> None:
    error = case.failure(tool, params)
    case.assertEqual(error["code"], code, f"{tool}: {error}")


def _schema_refused(case: NativeAcceptance, tool: str, params: dict[str, Any]) -> None:
    error = case.failure(tool, params)
    case.assertEqual(error["code"], case.schema_code(), f"{tool}: {error}")


def _refuse_unknown_documents(case: NativeAcceptance) -> None:
    for tool in (
        "camera.list",
        "cast.list",
        "cast.usage",
        "motion_path.list",
        "rig.inspect",
        "scene.dependencies",
        "scene.properties.inspect",
        "scene.validate",
        "storyboard.panels",
    ):
        _refused(case, tool, {"document_id": UNKNOWN_DOCUMENT, **ZERO}, "document_not_found")
    rig = {"document_id": UNKNOWN_DOCUMENT, **ZERO, "object_ids": ["object_unknown"], "frames": [0]}
    _refused(case, "rig.evaluate", rig, "document_not_found")


def _refuse_unknown_handles(case: NativeAcceptance, document: str, revision: str) -> None:
    level = {"document_id": document, "level_id": "level_unknown"}
    drawing = {**level, "drawing_id": DRAWING}
    _refused(case, "level.inspect", level, "invalid_handle")
    _refused(case, "palette.inspect", level, "invalid_handle")
    _refused(case, "palette.pages", level, "invalid_handle")
    _refused(case, "hook.inspect", {**level, **{"expected_revision": revision}}, "invalid_handle")
    _refused(case, "vector.structure", drawing, "invalid_handle")
    raster = {"coordinate_space": "raster_pixels_lower_left", "x": 0, "y": 0}
    _refused(
        case, "raster.read_pixels", {**drawing, **raster, "width": 1, "height": 1}, "invalid_handle"
    )
    opacity = {**drawing, "expected_revision": revision, "stroke_index": 0, "offset": 0, "limit": 8}
    _refused(case, "vector.opacity.inspect", opacity, "invalid_handle")
    _refused(
        case,
        "lipsync.inspect",
        {"analysis_id": "analysis_unknown", "offset": 0, "limit": 8},
        "invalid_handle",
    )


def _refuse_bad_values(case: NativeAcceptance) -> None:
    _refused(
        case, "scene.package_info", {"path": str(case.inputs / "missing")}, "unregistered_package"
    )
    _refused(case, "studio_palette.find", {"palette_id": "palette_unknown"}, "root_unavailable")
    _refused(case, "tool.describe", {"names": ["no.such.tool"]}, "unknown_tool")
    _schema_refused(case, "text.fonts", {"family_prefix": "x" * 400, "offset": 0, "limit": 8})
    _schema_refused(case, "operation.list", {"state": "no_such_state"})
    _schema_refused(case, "runtime.inspect", {"probes": ["no_such_probe"]})
    _schema_refused(case, "automation.capabilities", {"limit": 0})
    _schema_refused(case, "fx.recipe.list", {"category": "no_such_category"})


def _refuse_unknown_fields(case: NativeAcceptance) -> None:
    for tool in (
        "automation.list_documents",
        "column.kinds",
        "fx.catalog",
        "fx.recipe.list",
        "journal.status",
        "render.capabilities",
        "shared.roots",
    ):
        _schema_refused(case, tool, {"unrecognized_negative_argument": 1})


def _refuse_toolset_changes(case: NativeAcceptance) -> None:
    """The adapter's own toolset tools: only a client in front of the worker has them."""
    if not case.through_sdk:
        return
    for tool in ("toolsets.enable", "toolsets.disable"):
        _refused(case, tool, {"names": ["no-such-toolset"]}, "UNKNOWN_TOOLSET")
    _schema_refused(case, "toolsets.list", {"unrecognized_negative_argument": 1})


@acceptance_case
def read_tools_refuse_what_they_cannot_serve(case: NativeAcceptance) -> None:
    """Refuse unknown documents and handles, out-of-range values and unknown fields by code.

    Through the SDK client the adapter's toolset tools are refused too.
    """
    document = case.create("Negative reads")
    _refuse_unknown_documents(case)
    _refuse_unknown_handles(case, document, case.revisions[document])
    _refuse_bad_values(case)
    _refuse_unknown_fields(case)
    _refuse_toolset_changes(case)
