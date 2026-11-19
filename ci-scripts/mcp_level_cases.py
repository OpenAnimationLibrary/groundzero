"""Level lifecycle cases: interpretation settings with their native applicability, exposures in
every sheet, rename, independent duplicates and removal with an explicit exposure plan, each
read back after a reopen through the worker and through the editor's own loader."""

from __future__ import annotations

import struct
import zlib
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_png import SIGNATURE, decode_png

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

ONE = {"number": 1, "suffix": ""}
TONE_SIZE = {"width": 16, "height": 16, "dpi": 120}
TWO = {"number": 2, "suffix": ""}
SETTINGS = (
    "dpi",
    "premultiply",
    "white_transparent",
    "antialias_softness",
    "subsampling",
    "color_space_gamma",
)


# Quadrants of the alpha fixture: opaque white, half-transparent red, opaque green and blue.
ALPHA_QUADRANTS = ((255, 255, 255, 255), (255, 0, 0, 128), (0, 255, 0, 255), (0, 0, 255, 255))
WHOLE = {"coordinate_space": "raster_pixels_lower_left", "x": 0, "y": 0, "width": 32, "height": 32}


def write_alpha_fixture(path: Path) -> None:
    """Write a 32x32 RGBA PNG whose quadrants make premultiply and white transparency visible."""

    def chunk(kind: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        )

    rows = bytearray()
    for y in range(32):
        rows.append(0)
        for x in range(32):
            rows.extend(ALPHA_QUADRANTS[int(x >= 16) + 2 * int(y >= 16)])
    header = struct.pack(">IIBBBBB", 32, 32, 8, 6, 0, 0, 0)
    path.write_bytes(
        SIGNATURE
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(rows)))
        + chunk(b"IEND", b"")
    )


def inspect(case: NativeAcceptance, document: str, level: str, **params: Any) -> dict[str, Any]:
    """Return level.inspect of one level."""
    return case.success("level.inspect", {"document_id": document, "level_id": level, **params})


def setting_values(case: NativeAcceptance, document: str) -> dict[str, dict[str, Any]]:
    """Return every simple level's setting values by level name."""
    levels = case.success("scene.summary", {"document_id": document})["levels"]
    values: dict[str, dict[str, Any]] = {}
    for level in levels:
        settings = inspect(case, document, level["level_id"]).get("settings")
        if settings is None:
            continue
        values[level["name"]] = {
            # A package stores every raster DPI as a custom DPI of the same value (D-WS-16).
            "dpi_policy": settings["dpi_policy"] if level.get("format") == "vector" else "custom",
            **{name: settings[name]["value"] for name in SETTINGS},
        }
    return values


def reopen_settings(case: NativeAcceptance, document: str, name: str) -> tuple[str, Path]:
    """Save, reopen with the worker, and require both loaders to read the same settings."""
    expected = setting_values(case, document)
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / name))
    package = Path(saved["package_directory"])
    reopened = case.mutate("scene.open", None, path=str(package))["document_id"]
    case.assertEqual(setting_values(case, reopened), expected)
    opened = case.success(
        "scene.open_read_only",
        {"path": str(package / "scene.tnz"), "project_id": "standalone", "operation_id": name},
    )
    editor = opened["document_id"]
    case.revisions[editor] = opened["revision"]
    try:
        case.assertEqual(setting_values(case, editor), expected)
    finally:
        case.mutate("scene.discard", editor)
    return reopened, package


def three_formats(case: NativeAcceptance) -> tuple[str, dict[str, str]]:
    """A scene with a vector, a Toonz raster and an imported RGBA level, each with drawing 1."""
    document = case.create("Levels")
    levels = {
        "vector": case.mutate("level.create", document, name="Ink", format="vector")["level_id"],
        "toonzRaster": case.mutate("level.create", document, name="Tone", format="toonzRaster")[
            "level_id"
        ],
        "rgba8": case.mutate(
            "level.import", document, path=str(case.fixture), name="Paint", dpi=120
        )["level_id"],
    }
    for level, extra in ((levels["vector"], {}), (levels["toonzRaster"], TONE_SIZE)):
        case.mutate(
            "drawing.create",
            document,
            level_id=level,
            drawing_id=ONE,
            format="vector" if not extra else "toonzRaster",
            **extra,
        )
    return document, levels


def expose(case: NativeAcceptance, document: str, level: str, name: str, length: int) -> str:
    """Expose drawing 1 of a level in a new column and return the column."""
    column = case.mutate("column.create", document, name=name)["column_id"]
    case.mutate(
        "column.expose",
        document,
        column_id=column,
        level_id=level,
        drawing_id=ONE,
        start_frame=0,
        length=length,
    )
    return column


def _applicable(settings: dict[str, Any]) -> set[str]:
    return {name for name in SETTINGS if settings[name]["applicable"]}


@acceptance_case
def level_settings_report_native_applicability(case: NativeAcceptance) -> None:
    """level.inspect reports each interpretation setting with the applicability the native
    Level Settings popup gives its format, and why it cannot be written (SCN-05 #6, F9)."""
    document, levels = three_formats(case)
    vector = inspect(case, document, levels["vector"])["settings"]
    case.assertEqual(_applicable(vector), set())
    tone = inspect(case, document, levels["toonzRaster"])["settings"]
    case.assertEqual(_applicable(tone), {"dpi", "antialias_softness", "subsampling"})
    paint = inspect(case, document, levels["rgba8"])["settings"]
    case.assertEqual(
        _applicable(paint),
        {"dpi", "premultiply", "white_transparent", "antialias_softness", "subsampling"},
    )
    case.assertEqual(paint["dpi"]["value"], 120)
    case.assertEqual(paint["dpi_policy"], "custom")
    case.assertEqual((paint["image_width_px"], paint["image_height_px"]), (32, 32))
    case.assertEqual(paint["color_space_gamma"]["reason"], "linear_raster_only")
    case.assertTrue(paint["premultiply"]["writable"])
    case.assertEqual(paint["subsampling"]["reason"], "unsaved_level")
    reopen_settings(case, document, "formats")


@acceptance_case
def level_usage_lists_exposures_and_locks(case: NativeAcceptance) -> None:
    """With include_usage, level.inspect lists each exposing column; a locked one makes the
    settings unwritable with locked_exposure (SCN-05 F1)."""
    document, levels = three_formats(case)
    paint = levels["rgba8"]
    case.assertEqual(
        inspect(case, document, paint, include_usage=True)["usage"],
        {
            "complete": True,
            "cells": 0,
            "columns": 0,
            "sheets": 1,
            "rows": [],
            "rows_truncated": False,
        },
    )
    first = expose(case, document, paint, "A", 5)
    second = expose(case, document, paint, "B", 3)
    usage = inspect(case, document, paint, include_usage=True)["usage"]
    case.assertEqual((usage["cells"], usage["columns"]), (8, 2))
    case.assertEqual([row["column_id"] for row in usage["rows"]], [first, second])
    case.assertEqual(
        [(row["first_frame"], row["last_frame"], row["cells"]) for row in usage["rows"]],
        [(0, 4, 5), (0, 2, 3)],
    )
    case.assertTrue(all(row["sheet_level_id"] is None for row in usage["rows"]))
    case.mutate("column.update", document, column_id=second, locked=True)
    settings = inspect(case, document, paint)["settings"]
    case.assertEqual(settings["premultiply"]["reason"], "locked_exposure")
    case.assertFalse(settings["dpi"]["writable"])
    case.assertNotIn("usage", inspect(case, document, paint))


def _alpha_scene(case: NativeAcceptance) -> tuple[str, str, str]:
    document = case.create("Interpretation")
    fixture = case.inputs / "alpha.png"
    write_alpha_fixture(fixture)
    level = case.mutate("level.import", document, path=str(fixture), name="Paint", dpi=120)
    column = expose(case, document, level["level_id"], "Paint", 1)
    return document, level["level_id"], column


def _pixels(case: NativeAcceptance, document: str, level: str) -> list:
    target = {"document_id": document, "level_id": level, "drawing_id": ONE, **WHOLE}
    return case.success("raster.read_pixels", target)["pixels"]


def _update(case: NativeAcceptance, document: str, level: str, **params: Any) -> dict[str, Any]:
    return case.mutate("level.settings.set", document, level_id=level, **params)


def _rename(case: NativeAcceptance, document: str, level: str, name: str) -> dict[str, Any]:
    return case.mutate("level.rename", document, level_id=level, name=name)


def _refused(case: NativeAcceptance, document: str, level: str, code: str, **params: Any) -> dict:
    params = case.write_params(document, {"level_id": level, **params})
    method = "level.rename" if "name" in params else "level.settings.set"
    return case.handler_failure(method, params, code)


@acceptance_case
def level_settings_change_interpretation_not_pixels(case: NativeAcceptance) -> None:
    """Premultiply, white transparency and DPI change how the level renders, never its pixels
    or the camera; the premultiply/white coupling is explicit; every value survives a reopen
    through both loaders and renders the same (SCN-05 #6, #7, #8, F8)."""
    document, level, _column = _alpha_scene(case)
    pixels = _pixels(case, document, level)
    camera = case.success("camera.inspect", {"document_id": document})
    plain = case.preview(document)
    result = _update(case, document, level, settings={"premultiply": True})
    case.assertEqual(result["changed"], ["premultiply"])
    case.assertEqual((result["pixels_resampled"], result["camera_changed"]), (False, False))
    premultiplied = case.preview(document)
    case.assertNotEqual(decode_png(premultiplied), decode_png(plain))
    _refused(case, document, level, "invalid_params", settings={"white_transparent": True})
    _refused(
        case,
        document,
        level,
        "invalid_params",
        settings={"premultiply": True, "white_transparent": True},
    )
    coupled = _update(
        case, document, level, settings={"white_transparent": True}, coupling="clear_other"
    )
    case.assertEqual(coupled["coupled_cleared"], "premultiply")
    after = coupled["settings"]
    case.assertEqual(
        (after["premultiply"]["value"], after["white_transparent"]["value"]), (False, True)
    )
    case.assertNotEqual(decode_png(case.preview(document)), decode_png(premultiplied))
    sized = _update(case, document, level, settings={"dpi": 240, "antialias_softness": 10})
    case.assertEqual(sized["stage_size_in_before"], {"width": 32 / 120, "height": 32 / 120})
    case.assertEqual(sized["stage_size_in_after"], {"width": 32 / 240, "height": 32 / 240})
    case.assertEqual(sized["exposures"], 1)
    case.assertEqual(case.success("camera.inspect", {"document_id": document}), camera)
    case.assertEqual(_pixels(case, document, level), pixels)
    rendered = case.preview(document)
    reopened, _package = reopen_settings(case, document, "interpretation")
    case.assertEqual(case.preview(reopened), rendered)


@acceptance_case
def level_settings_refuse_what_native_disables(case: NativeAcceptance) -> None:
    """Settings a format does not have, colour-space gamma outside linear raster, subsampling
    on an unsaved level and any setting under a locked exposure are refused with the reason,
    and nothing changes (SCN-05 F1, F9)."""
    document, levels = three_formats(case)
    error = _refused(case, document, levels["vector"], "unsupported_target", settings={"dpi": 200})
    case.assertEqual(
        error["details"],
        {"setting": "dpi", "format": "vector", "reason": "not_applicable_to_format"},
    )
    error = _refused(
        case, document, levels["rgba8"], "unsupported_target", settings={"color_space_gamma": 2.2}
    )
    case.assertEqual(error["details"]["reason"], "linear_raster_only")
    error = _refused(
        case, document, levels["toonzRaster"], "unsupported_target", settings={"subsampling": 2}
    )
    case.assertEqual(error["details"]["reason"], "unsaved_level")
    _refused(case, document, levels["rgba8"], "invalid_params", settings={})
    column = expose(case, document, levels["rgba8"], "Paint", 2)
    case.mutate("column.update", document, column_id=column, locked=True)
    error = _refused(case, document, levels["rgba8"], "locked", settings={"premultiply": True})
    case.assertEqual(error["details"]["column_ids"], [column])


@acceptance_case
def level_rename_keeps_identity(case: NativeAcceptance) -> None:
    """A rename keeps the level's handle, cast id and exposures; it is admitted under a locked
    exposure since no picture changes; a used name is refused; renaming to its own name keeps
    the revision; the new name survives a reopen (SCN-05 #4, rename half)."""
    document, levels = three_formats(case)
    ink = levels["vector"]
    column = expose(case, document, levels["rgba8"], "Paint", 2)
    case.mutate("column.update", document, column_id=column, locked=True)
    listed = case.success(
        "cast.list", {"document_id": document, "expected_revision": case.revisions[document]}
    )["items"]
    cast_ids = {row["level_id"]: row["native_cast_id"] for row in listed}
    _refused(case, document, ink, "name_conflict", name="Paint")
    renamed = _rename(case, document, levels["rgba8"], "Painted")
    case.assertEqual((renamed["renamed"], renamed["name_before"]), (True, "Paint"))
    case.assertEqual(renamed["level"]["level_id"], levels["rgba8"])
    revision = case.revisions[document]
    same = _rename(case, document, levels["rgba8"], "Painted")
    case.assertFalse(same["renamed"])
    case.assertEqual(case.revisions[document], revision)
    listed = case.success(
        "cast.list", {"document_id": document, "expected_revision": case.revisions[document]}
    )["items"]
    case.assertEqual({row["level_id"]: row["native_cast_id"] for row in listed}, cast_ids)
    usage = inspect(case, document, levels["rgba8"], include_usage=True)["usage"]
    case.assertEqual(usage["cells"], 2)
    reopened, _package = reopen_settings(case, document, "renamed")
    names = {
        row["name"] for row in case.success("scene.summary", {"document_id": reopened})["levels"]
    }
    case.assertEqual(names, {"Ink", "Tone", "Painted"})


def _patch(case: NativeAcceptance, document: str, level: str) -> None:
    case.mutate(
        "raster.patch",
        document,
        level_id=level,
        drawing_id=ONE,
        coordinate_space="raster_pixels_lower_left",
        x=0,
        y=0,
        width=2,
        height=2,
        sharing="all_exposures",
        pixels=[{"r": 255, "g": 0, "b": 255, "a": 255}] * 4,
    )


@acceptance_case
def level_duplicate_is_independent_and_reopens(case: NativeAcceptance) -> None:
    """A duplicate copies every drawing and setting under a new cast id, is exposed nowhere,
    and later edits of either level never reach the other; both reopen (SCN-05 #2, COV-02 F2).
    A repeated exposure keeps sharing one level."""
    document, level, column = _alpha_scene(case)
    _update(case, document, level, settings={"premultiply": True, "antialias_softness": 5})
    copy = case.mutate("level.duplicate", document, level_id=level, name="Paint copy")
    case.assertEqual(copy["level"]["drawing_count"], 1)
    case.assertEqual((copy["exposed"], copy["palette"]), (False, "none"))
    case.assertEqual(copy["cast_metadata"]["cast_id"], "new")
    duplicate = copy["level"]["level_id"]
    case.assertEqual(inspect(case, document, duplicate, include_usage=True)["usage"]["cells"], 0)
    source_settings = setting_values(case, document)
    case.assertEqual(source_settings["Paint copy"], source_settings["Paint"])
    pixels = _pixels(case, document, level)
    _patch(case, document, duplicate)
    case.assertEqual(_pixels(case, document, level), pixels)
    case.assertNotEqual(_pixels(case, document, duplicate), pixels)
    _update(case, document, level, settings={"premultiply": False})
    case.assertTrue(inspect(case, document, duplicate)["settings"]["premultiply"]["value"])
    listed = case.success(
        "cast.list", {"document_id": document, "expected_revision": case.revisions[document]}
    )["items"]
    cast_ids = {row["level_id"]: row["native_cast_id"] for row in listed}
    case.assertNotEqual(cast_ids[level], cast_ids[duplicate])
    case.assertEqual(_cell_levels(case, document, column, 1), [(level, ONE)])
    reopen_settings(case, document, "duplicate")


def _remove(case: NativeAcceptance, document: str, level: str, **params: Any) -> dict:
    return case.mutate("level.remove", document, level_id=level, **params)


def _remove_refused(case: NativeAcceptance, document: str, level: str, code: str, **params: Any):
    params = case.write_params(document, {"level_id": level, **params})
    return case.handler_failure("level.remove", params, code)


def _cell_levels(case: NativeAcceptance, document: str, column: str, length: int) -> list:
    cells = case.success(
        "column.cells",
        {"document_id": document, "column_id": column, "start_frame": 0, "length": length},
    )["cells"]
    return [None if cell["empty"] else (cell["level_id"], cell["drawing_id"]) for cell in cells]


@acceptance_case
def level_remove_needs_an_explicit_plan(case: NativeAcceptance) -> None:
    """Removing an exposed level is refused with its usage until the call states a plan: clear
    blanks the exposures without moving later cells, and no file is deleted; an unused level
    goes with refuse (SCN-05 #3, F4; COV-02 #2)."""
    document, levels = three_formats(case)
    paint = levels["rgba8"]
    column = expose(case, document, paint, "Paint", 3)
    error = _remove_refused(case, document, paint, "in_use", exposure_policy="refuse")
    case.assertEqual(error["details"]["usage"]["cells"], 3)
    case.assertEqual(error["details"]["usage"]["rows"][0]["column_id"], column)
    _remove_refused(
        case,
        document,
        paint,
        "invalid_params",
        exposure_policy="clear",
        replacement_level_id=levels["vector"],
    )
    files = sorted(path.name for path in case.workspace.rglob("*") if path.is_file())
    removed = _remove(case, document, paint, exposure_policy="clear")
    case.assertEqual((removed["cells_cleared"], removed["files_deleted"]), (3, 0))
    case.assertEqual(removed["usage_before"]["cells"], 3)
    case.assertEqual(_cell_levels(case, document, column, 3), [None, None, None])
    case.assertEqual(
        sorted(path.name for path in case.workspace.rglob("*") if path.is_file()), files
    )
    error = case.failure("level.inspect", {"document_id": document, "level_id": paint})
    case.assertEqual(error["code"], "invalid_handle")
    unused = _remove(case, document, levels["toonzRaster"], exposure_policy="refuse")
    case.assertEqual(unused["usage_before"]["cells"], 0)
    reopened, _package = reopen_settings(case, document, "removed")
    names = {
        row["name"] for row in case.success("scene.summary", {"document_id": reopened})["levels"]
    }
    case.assertEqual(names, {"Ink"})


@acceptance_case
def level_remove_replaces_exposures_by_another_level(case: NativeAcceptance) -> None:
    """replace rewrites every exposure to the same drawing of the replacement, keeping timing;
    a replacement lacking a drawing, or of another format without allow_type_change, is
    refused (SCN-05 F5)."""
    document = case.create("Replace")
    old = case.mutate("level.create", document, name="Old", format="vector")["level_id"]
    new = case.mutate("level.create", document, name="New", format="vector")["level_id"]
    for drawing in (ONE, TWO):
        case.mutate("drawing.create", document, level_id=old, drawing_id=drawing, format="vector")
    case.mutate("drawing.create", document, level_id=new, drawing_id=ONE, format="vector")
    column = case.mutate("column.create", document, name="Col")["column_id"]
    runs = [
        {"level_id": old, "drawing_id": ONE, "length": 2},
        {"empty": True, "length": 1},
        {"level_id": old, "drawing_id": TWO, "length": 1},
    ]
    case.mutate(
        "cells.set",
        document,
        columns=[{"column_id": column, "start_frame": 0, "runs": runs}],
        overwrite=False,
    )
    error = _remove_refused(
        case, document, old, "missing_drawing", exposure_policy="replace", replacement_level_id=new
    )
    case.assertEqual(error["details"]["drawings"], [TWO])
    paint = case.mutate("level.import", document, path=str(case.fixture), name="Paint", dpi=120)
    error = _remove_refused(
        case,
        document,
        old,
        "unsupported_target",
        exposure_policy="replace",
        replacement_level_id=paint["level_id"],
    )
    case.assertEqual(error["details"]["reason"], "type_change")
    case.mutate("drawing.create", document, level_id=new, drawing_id=TWO, format="vector")
    result = _remove(case, document, old, exposure_policy="replace", replacement_level_id=new)
    case.assertEqual((result["cells_replaced"], result["type_changed"]), (3, False))
    case.assertEqual(
        _cell_levels(case, document, column, 4), [(new, ONE), (new, ONE), None, (new, TWO)]
    )
    sketch = case.mutate("level.create", document, name="Sketch", format="vector")["level_id"]
    case.mutate("drawing.create", document, level_id=sketch, drawing_id=ONE, format="vector")
    other = expose(case, document, sketch, "Sketch", 1)
    typed = _remove(
        case,
        document,
        sketch,
        exposure_policy="replace",
        replacement_level_id=paint["level_id"],
        allow_type_change=True,
    )
    case.assertTrue(typed["type_changed"])
    case.assertEqual(_cell_levels(case, document, other, 1), [(paint["level_id"], ONE)])
    _width, _height, pixels = decode_png(case.preview(document))
    case.assertIn((255, 0, 0, 255), pixels)


@acceptance_case
def drawing_remove_states_what_happens_to_exposures(case: NativeAcceptance) -> None:
    """drawing.remove refuses an exposed drawing by default, and with clear or remap rewrites
    every exposure first, so none ever names a removed drawing (DRW-03 #3)."""
    document = case.create("Drawings")
    level = case.mutate("level.create", document, name="Ink", format="vector")["level_id"]
    three = {"number": 3, "suffix": ""}
    for drawing in (ONE, TWO, three):
        case.mutate("drawing.create", document, level_id=level, drawing_id=drawing, format="vector")
    column = case.mutate("column.create", document, name="Col")["column_id"]
    runs = [{"level_id": level, "drawing_id": d, "length": 1} for d in (ONE, TWO, ONE, three)]
    case.mutate(
        "cells.set",
        document,
        columns=[{"column_id": column, "start_frame": 0, "runs": runs}],
        overwrite=False,
    )
    target = {"level_id": level, "drawing_id": ONE}
    params = case.write_params(document, target)
    error = case.handler_failure("drawing.remove", params, "in_use")
    case.assertEqual(error["details"]["usage"]["cells"], 2)
    remapped = case.mutate(
        "drawing.remove",
        document,
        **target,
        exposure_policy={"kind": "remap", "drawing_id": TWO},
    )
    case.assertEqual((remapped["exposures_remapped"], remapped["usage"]["cells"]), (2, 2))
    case.assertEqual(remapped["columns"], [column])
    case.assertEqual(
        _cell_levels(case, document, column, 4),
        [(level, TWO), (level, TWO), (level, TWO), (level, three)],
    )
    cleared = case.mutate(
        "drawing.remove",
        document,
        level_id=level,
        drawing_id=three,
        exposure_policy={"kind": "clear"},
    )
    case.assertEqual(cleared["exposures_cleared"], 1)
    case.assertEqual(_cell_levels(case, document, column, 4)[3], None)
    params = case.write_params(
        document,
        {"level_id": level, "drawing_id": TWO, "exposure_policy": {"kind": "remap"}},
    )
    case.schema_failure("drawing.remove", params)
    params = case.write_params(
        document,
        {
            "level_id": level,
            "drawing_id": TWO,
            "exposure_policy": {"kind": "remap", "drawing_id": TWO},
        },
    )
    case.handler_failure("drawing.remove", params, "invalid_params")
