"""Cases on tools that already exist: exposure rows, numeric limits and curve units.

Each case closes a gap that no earlier case named: the exact row-to-drawing mapping through a
save, shared against cloned pixels, refusals at the edges of a range or a number, curves under
decoy launch settings, and a copy between incompatible units. A defect that a case finds is
recorded in `mcp_known_violations.py` instead of weakening the case.
"""

from __future__ import annotations

import contextlib
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import zlib
from collections.abc import Generator
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_animation_cases import _inspect, _peg, _render, _reopen, _rich_keys, _sample, _simple
from mcp_audio_cases import _column, _import, _place
from mcp_audio_model import Clip, tone, wav_file
from mcp_case import acceptance_case
from mcp_failure_cases import _fill_disk, _free_bytes
from mcp_file_publish_cases import staging_left
from mcp_known_violations import settle
from mcp_png import SIGNATURE, fixture_pixel
from mcp_scene_access_cases import _cells, _level, base_package, discard, open_scene, preview
from mcp_studio_palette_cases import call
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

Drawing = tuple[int, str]
ONE: Drawing = (1, "")
ONE_A: Drawing = (1, "a")
TWO: Drawing = (2, "")
MAX_FRAME = 999999
TERMINAL = {"completed", "failed", "canceled", "deadline_exceeded"}


def _drawing(drawing: Drawing) -> dict[str, Any]:
    return {"number": drawing[0], "suffix": drawing[1]}


def _raw(case: NativeAcceptance, method: str, params: dict[str, Any]) -> dict[str, Any]:
    """Call a tool without the fixture's declared-error check and return the whole answer."""
    case.worker.sequence += 1
    payload = {"id": f"raw-{case.worker.sequence}", "method": method, "params": params}
    return case.worker.exchange(json.dumps(payload).encode())


def _declared(case: NativeAcceptance, tool: str) -> set[str]:
    """The error codes a tool declares in its published descriptor."""
    listed = {item["name"]: item for item in case.capabilities()["tools"]}
    return set(listed[tool]["errors"])


def _summary(case: NativeAcceptance, document: str) -> dict[str, Any]:
    return case.success("scene.summary", {"document_id": document})


def _rows(
    case: NativeAcceptance, document: str, column: str, length: int
) -> list[tuple[str, Drawing] | None]:
    """Read `length` rows of a column as (level name, drawing) or None for a blank."""
    names = {item["level_id"]: item["name"] for item in _summary(case, document)["levels"]}
    cells = case.success(
        "column.cells",
        {"document_id": document, "column_id": column, "start_frame": 0, "length": length},
    )["cells"]
    case.assertEqual([cell["frame"] for cell in cells], list(range(length)))
    rows: list[tuple[str, Drawing] | None] = []
    for cell in cells:
        if cell["empty"]:
            rows.append(None)
        else:
            drawing = cell["drawing_id"]
            rows.append((names[cell["level_id"]], (drawing["number"], drawing["suffix"])))
    return rows


def _expand(layout: list[tuple[int, int, Drawing]], length: int) -> list[Any]:
    """Expected rows of a layout of (first frame, held frames, drawing) on one level, by hand."""
    rows: list[Any] = [None] * length
    for first, held, drawing in layout:
        for frame in range(first, first + held):
            rows[frame] = ("Ink", drawing)
    return rows


def _vector_column(case: NativeAcceptance, drawings: list[Drawing]) -> tuple[str, str, str]:
    document = case.create()
    level = case.mutate("level.create", document, name="Ink", format="vector")["level_id"]
    for drawing in drawings:
        case.mutate(
            "drawing.create",
            document,
            level_id=level,
            drawing_id=_drawing(drawing),
            format="vector",
        )
    column = case.mutate("column.create", document, name="Rows")["column_id"]
    return document, level, column


def _expose(
    case: NativeAcceptance, scene: tuple[str, str, str], first: int, held: int, drawing: Drawing
) -> None:
    document, level, column = scene
    case.mutate(
        "column.expose",
        document,
        column_id=column,
        level_id=level,
        drawing_id=_drawing(drawing),
        start_frame=first,
        length=held,
    )


@acceptance_case
def exposure_on_twos_with_blanks_keeps_every_row_through_save_and_reopen(
    case: NativeAcceptance,
) -> None:
    """Expose 1, 1a and 2 on twos with blanks and compare each row before and after reopening."""
    scene = _vector_column(case, [ONE, ONE_A, TWO])
    document, _level, column = scene
    layout = [(0, 2, ONE), (2, 2, ONE_A), (6, 2, TWO), (9, 1, ONE)]
    for first, held, drawing in layout:
        _expose(case, scene, first, held, drawing)
    expected = _expand(layout, 12)
    case.assertEqual(
        expected[:10],
        [("Ink", ONE)] * 2
        + [("Ink", ONE_A)] * 2
        + [None] * 2
        + [("Ink", TWO)] * 2
        + [None]
        + [("Ink", ONE)],
    )
    case.assertEqual(_rows(case, document, column, 12), expected)
    case.assertEqual(_summary(case, document)["frame_count"], 10)
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "twos"))
    reopened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    summary = _summary(case, reopened)
    case.assertEqual(summary["frame_count"], 10)
    case.assertEqual(len(summary["columns"]), 1)
    case.assertEqual(_rows(case, reopened, summary["columns"][0]["column_id"], 12), expected)
    again = case.mutate("scene.save_copy", reopened, directory=str(case.workspace / "twos-2"))
    second = case.mutate("scene.open", None, path=again["package_directory"])["document_id"]
    column_id = _summary(case, second)["columns"][0]["column_id"]
    case.assertEqual(_rows(case, second, column_id, 12), expected)


def _paint(case: NativeAcceptance, document: str, target: dict[str, Any]) -> None:
    magenta = {"r": 255, "g": 0, "b": 255, "a": 255}
    case.mutate(
        "raster.patch",
        document,
        **target,
        coordinate_space="raster_pixels_lower_left",
        x=0,
        y=0,
        width=8,
        height=8,
        sharing="all_exposures",
        pixels=[magenta] * 64,
    )


def _corner(pixels: list[Any]) -> list[Any]:
    """The 8 by 8 block at the lower left of a 32 by 32 image, whose first row is its top."""
    return [pixels[row * 32 + column] for row in range(24, 32) for column in range(8)]


@acceptance_case
def repeated_exposure_shares_pixels_and_a_clone_does_not(case: NativeAcceptance) -> None:
    """Edit a drawing exposed twice: both exposures change; its clone keeps the old pixels."""
    document, level, column = case.populated_scene()
    target = {"level_id": level["level_id"], "drawing_id": _drawing(ONE)}
    clone = case.mutate(
        "drawing.clone", document, **target, destination_drawing_id={"number": 3, "suffix": ""}
    )
    case.assertEqual(clone["drawing_id"], {"number": 3, "suffix": ""})
    for first, drawing in ((30, ONE), (60, (3, ""))):
        case.mutate(
            "column.expose",
            document,
            column_id=column["column_id"],
            level_id=level["level_id"],
            drawing_id=_drawing(drawing),
            start_frame=first,
            length=5,
        )
    original = [fixture_pixel(x, y, 32) for y in range(24, 32) for x in range(8)]
    frames = {"first": 0, "again": 30, "clone": 60}
    before = {name: _corner(_render(case, document, frame)) for name, frame in frames.items()}
    case.assertEqual(before, {"first": original, "again": original, "clone": original})
    _paint(case, document, target)
    after = {name: _corner(_render(case, document, frame)) for name, frame in frames.items()}
    magenta = [(255, 0, 255, 255)] * 64
    case.assertEqual(after, {"first": magenta, "again": magenta, "clone": original})
    rectangle = {"coordinate_space": "raster_pixels_lower_left", "x": 0, "y": 0}
    rectangle.update(width=1, height=1)
    read = {"document_id": document, **rectangle}
    edited = case.success("raster.read_pixels", {**read, **target})["pixels"]
    cloned = case.success(
        "raster.read_pixels", {**read, **target, "drawing_id": clone["drawing_id"]}
    )
    case.assertNotEqual(edited, cloned["pixels"])
    _paint(case, document, {**target, "drawing_id": clone["drawing_id"]})
    case.assertEqual(_corner(_render(case, document, frames["clone"])), magenta)
    case.assertEqual(_corner(_render(case, document, frames["again"])), magenta)


def _refuse_numbers(
    case: NativeAcceptance,
    method: str,
    document: str,
    base: dict[str, Any],
    name: str,
    bad: list[Any],
) -> None:
    for value in bad:
        with case.subTest(tool=method, field=name, value=value):
            params = case.write_params(document, {**base, name: value})
            case.schema_failure(method, params)


@acceptance_case
def expose_and_cells_refuse_negative_overflowing_and_oversized_ranges(
    case: NativeAcceptance,
) -> None:
    """Refuse rows and lengths outside the schema, accept the last frame, keep the scene."""
    scene = _vector_column(case, [ONE])
    document, level, column = scene
    target = {"column_id": column, "level_id": level, "drawing_id": _drawing(ONE)}
    expose = {**target, "start_frame": 0, "length": 1}
    starts = [-1, -MAX_FRAME, MAX_FRAME + 1, 2**31, 2**32, 1.5, "0", None]
    lengths = [0, -1, 10001, 2**31, 2**32, 0.5, "1", None]
    _refuse_numbers(case, "column.expose", document, expose, "start_frame", starts)
    _refuse_numbers(case, "column.expose", document, expose, "length", lengths)
    read = {"document_id": document, "column_id": column, "start_frame": 0, "length": 1}
    _refuse_numbers(case, "column.cells", document, read, "start_frame", starts)
    _refuse_numbers(case, "column.cells", document, read, "length", lengths)
    case.assertEqual(_summary(case, document)["frame_count"], 0)
    _expose(case, scene, MAX_FRAME - 1, 2, ONE)
    case.assertEqual(_summary(case, document)["frame_count"], MAX_FRAME + 1)
    cells = case.success("column.cells", {**read, "start_frame": MAX_FRAME - 1, "length": 2})[
        "cells"
    ]
    case.assertEqual([cell["frame"] for cell in cells], [MAX_FRAME - 1, MAX_FRAME])
    case.assertEqual([cell["empty"] for cell in cells], [False, False])
    _past_the_last_frame(case, document, target, read)


def _past_the_last_frame(
    case: NativeAcceptance, document: str, target: dict[str, Any], read: dict[str, Any]
) -> None:
    """An interval that starts in range and runs past the last row is refused whole."""
    before = _summary(case, document)
    expose = _raw(
        case,
        "column.expose",
        case.write_params(document, {**target, "start_frame": MAX_FRAME - 1, "length": 3}),
    )
    case.assertIn("error", expose)
    case.assertIn(expose["error"]["code"], _declared(case, "column.expose"))
    case.assertEqual(_summary(case, document), before)
    cells = _raw(case, "column.cells", {**read, "start_frame": MAX_FRAME, "length": 2})
    case.assertIn("error", cells)
    case.assertIn(cells["error"]["code"], _declared(case, "column.cells"))
    case.assertEqual(_summary(case, document), before)


@acceptance_case
def a_ten_thousand_row_exposure_reads_back_whole(case: NativeAcceptance) -> None:
    """Hold one drawing for the largest allowed length and read every row back."""
    scene = _vector_column(case, [ONE])
    document, _level, column = scene
    _expose(case, scene, 0, 10000, ONE)
    cells = case.success(
        "column.cells",
        {"document_id": document, "column_id": column, "start_frame": 0, "length": 10000},
    )["cells"]
    case.assertEqual(len(cells), 10000)
    case.assertTrue(all(not cell["empty"] for cell in cells))
    case.assertEqual({(c["drawing_id"]["number"], c["drawing_id"]["suffix"]) for c in cells}, {ONE})
    case.assertEqual(_summary(case, document)["frame_count"], 10000)


SIZE_TOOLS = ("scene.create", "scene.settings", "camera.update")
SETTING_VALUES: dict[str, tuple[list[Any], list[Any]]] = {
    "frame_rate": ([1, 24, 23.976, 240], [0, -24, -0.0, 0.999, 240.001, 241, 1e6, 2**31]),
    "width_px": ([1, 32, 4096], [0, -1, 8193, 2**31, 2**63, 1.5]),
    "height_px": ([1, 32, 4096], [0, -1, 8193, 2**31, 2**63, 1.5]),
    "width_in": ([0.001, 0.2667, 10000], [0, -1, -0.001, 0.0009, 10000.5, 10001, 1e9]),
    "height_in": ([0.001, 0.2667, 10000], [0, -1, -0.001, 0.0009, 10000.5, 10001, 1e9]),
}


def _document_count(case: NativeAcceptance) -> int:
    return len(case.success("automation.list_documents", {})["documents"])


def _setting_call(case: NativeAcceptance, tool: str, document: str, extra: dict[str, Any]) -> Any:
    """Build the call of one of the three tools that take the same settings."""
    if tool == "scene.create":
        return case.write_params(None, {"name": "Limits", **extra})
    return case.write_params(document, extra)


def _refuse_setting(
    case: NativeAcceptance, tool: str, document: str, extra: dict[str, Any]
) -> None:
    params = _setting_call(case, tool, document, extra)
    before = _document_count(case)
    case.schema_failure(tool, params)
    case.assertEqual(_document_count(case), before)


def _accept_setting(
    case: NativeAcceptance, tool: str, document: str, extra: dict[str, Any]
) -> None:
    result = case.mutate(tool, None if tool == "scene.create" else document, **_named(tool, extra))
    if tool == "scene.create":
        case.mutate("scene.discard", result["document_id"])


def _named(tool: str, extra: dict[str, Any]) -> dict[str, Any]:
    return {"name": "Limits", **extra} if tool == "scene.create" else extra


@acceptance_case
def frame_rate_resolution_and_size_limits_are_refused_by_every_setting_tool(
    case: NativeAcceptance,
) -> None:
    """Refuse zero, negative, fractional and over-limit numbers; accept each limit itself."""
    document = case.create()
    for tool in SIZE_TOOLS:
        for name, (good, bad) in SETTING_VALUES.items():
            if name == "frame_rate" and tool == "camera.update":
                continue
            for value in bad:
                with case.subTest(tool=tool, field=name, value=value):
                    _refuse_setting(case, tool, document, {name: value})
            for value in good:
                with case.subTest(tool=tool, field=name, accepted=value):
                    _accept_setting(case, tool, document, {name: value})
    case.assertEqual(_document_count(case), 1)
    summary = _summary(case, document)
    case.assertEqual(summary["frame_rate"], 240)
    case.assertEqual((summary["camera"]["width_px"], summary["camera"]["height_px"]), (4096, 4096))


@acceptance_case
def sixteen_megapixel_cameras_are_the_limit_for_every_setting_tool(case: NativeAcceptance) -> None:
    """Refuse a camera above 16 megapixels with a typed limit and keep the previous one."""
    document = case.create()
    camera = _summary(case, document)["camera"]
    for width, height in ((8192, 8192), (4096, 4097), (8192, 2049), (4097, 4096)):
        for tool in SIZE_TOOLS:
            with case.subTest(tool=tool, width=width, height=height):
                before = _document_count(case)
                params = _setting_call(
                    case, tool, document, {"width_px": width, "height_px": height}
                )
                error = case.failure(tool, params)
                case.assertEqual(error["code"], "resource_limit", error)
                case.assertEqual(_document_count(case), before)
                case.assertEqual(_summary(case, document)["camera"], camera)
    for width, height in ((4096, 4096), (8192, 2048), (2048, 8192), (1, 8192)):
        with case.subTest(accepted=(width, height)):
            result = case.mutate("camera.update", document, width_px=width, height_px=height)
            case.assertEqual((result["width_px"], result["height_px"]), (width, height))
            observed = case.success("camera.inspect", {"document_id": document})
            case.assertEqual((observed["width_px"], observed["height_px"]), (width, height))


def _literal_call(tool: str, document: str | None, revision: str | None, member: str) -> bytes:
    params: dict[str, Any] = {"operation_id": "literal"}
    if document is None:
        params["name"] = "Literal"
    else:
        params.update(document_id=document, expected_revision=revision)
    text = json.dumps({"id": "literal", "method": tool, "params": params})
    return (text[:-2] + f", {member}" + text[-2:]).encode()


@acceptance_case(
    worker_only="writes NaN, infinity and overflowing number literals, which no SDK client encodes"
)
def non_finite_settings_are_refused_before_any_setting_tool_runs(case: NativeAcceptance) -> None:
    """Send NaN, infinities and overflowing literals as every setting; nothing changes."""
    document = case.create()
    before = _summary(case, document)
    members = [
        f'"{name}": {literal}'
        for name in ("frame_rate", "width_px", "height_px", "width_in", "height_in")
        for literal in ("NaN", "Infinity", "-Infinity", "1e999", "-1e999")
    ]
    for tool in SIZE_TOOLS:
        for member in members:
            with case.subTest(tool=tool, member=member):
                revision = None if tool == "scene.create" else case.revisions[document]
                target = None if tool == "scene.create" else document
                response = case.worker.exchange(_literal_call(tool, target, revision, member))
                case.assertEqual(response["error"]["code"], "parse_error", response)
    case.assertEqual(_document_count(case), 1)
    case.assertEqual(_summary(case, document), before)


CHANNEL_UNITS = {
    "x": "inch",
    "y": "inch",
    "z": "native_z",
    "rotation": "degree",
    "scale_x": "scalar",
    "scale_y": "scalar",
    "scale": "scalar",
}
UNITS = ("inch", "degree", "scalar", "native_z")
COPY_FAMILIES = (("x", "y"), ("z",), ("rotation",), ("scale_x", "scale_y", "scale"))


def _family(channel: str) -> tuple[str, ...]:
    return next(family for family in COPY_FAMILIES if channel in family)


def _copy_request(source: dict[str, str], destination: dict[str, str]) -> dict[str, Any]:
    return {"source": source, "destination": destination, "destination_policy": "replace_all"}


def _curve_state(case: NativeAcceptance, document: str, objects: list[str]) -> list[Any]:
    state: list[Any] = [case.revisions[document]]
    for object_id in objects:
        for channel, unit in CHANNEL_UNITS.items():
            keys = case.success(
                "animation.keys",
                {"document_id": document, "object_id": object_id, "channel": channel, "unit": unit},
            )
            state.append(keys["keys"])
    return state


def _refused_copy(
    case: NativeAcceptance, document: str, pair: tuple[dict[str, str], dict[str, str]]
) -> str:
    """Refuse one copy, leaving every curve and the revision as they were; return the code."""
    objects = [pair[0]["object_id"], pair[1]["object_id"]]
    before = _curve_state(case, document, objects)
    request = case.write_params(document, _copy_request(*pair))
    response = _raw(case, "animation.copy_curve", request)
    case.assertIn("error", response, f"{pair} was copied")
    case.assertEqual(_curve_state(case, document, objects), before)
    return response["error"]["code"]


@acceptance_case
def copy_between_incompatible_units_is_refused_for_every_pair(case: NativeAcceptance) -> None:
    """Copy every channel to every other, with each unit: only the same kind of channel copies."""
    document = case.create()
    source = _peg(case, document)["object_id"]
    destination = _peg(case, document)["object_id"]
    for channel, unit in CHANNEL_UNITS.items():
        keys = [_simple(0, 1), _simple(10, 2)]
        case.mutate(
            "animation.set_keys",
            document,
            object_id=source,
            channel=channel,
            unit=unit,
            keys=keys,
        )
    for source_channel, source_unit in CHANNEL_UNITS.items():
        for target_channel, target_native in CHANNEL_UNITS.items():
            for unit in UNITS:
                pair = (
                    {"object_id": source, "channel": source_channel, "unit": source_unit},
                    {"object_id": destination, "channel": target_channel, "unit": unit},
                )
                with case.subTest(source=source_channel, destination=target_channel, unit=unit):
                    compatible = target_channel in _family(source_channel) and unit == target_native
                    if compatible:
                        _copied(case, document, pair)
                    else:
                        _incompatible(case, document, pair, unit == source_unit)


def _copied(
    case: NativeAcceptance, document: str, pair: tuple[dict[str, str], dict[str, str]]
) -> None:
    copied = case.mutate("animation.copy_curve", document, **_copy_request(*pair))
    case.assertEqual(copied["keys"], _inspect(case, document, pair[0])["keys"])
    case.assertEqual(_inspect(case, document, pair[1])["keys"], copied["keys"])


def _incompatible(
    case: NativeAcceptance,
    document: str,
    pair: tuple[dict[str, str], dict[str, str]],
    same_unit: bool,
) -> None:
    code = _refused_copy(case, document, pair)
    if same_unit:
        case.assertIn(code, _declared(case, "animation.copy_curve"))
    else:
        case.assertEqual(code, "invalid_params")


DECOY_PREFERENCES = (
    "[General]\npixelsOnly=true\nlinearUnits=pixel\ncameraUnits=pixel\nkeyframeType=7\n"
    "animationStep=9\ndefLevelWidth=7\ndefLevelHeight=7\nCurrentRoomChoice=Decoy\n"
)
SAMPLE_FRAMES = [index / 4 for index in range(0, 169)]


@contextlib.contextmanager
def _environment(changes: dict[str, str]) -> Generator[None]:
    saved = {name: os.environ.get(name) for name in changes}
    os.environ.update(changes)
    try:
        yield
    finally:
        for name, value in saved.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value


def _decoy_settings(base: Path) -> dict[str, str]:
    """Create user settings that disagree with every default and return the variables for them."""
    home = base / "decoy home"
    user = os.environ.get("USER", "user")
    root = base / "decoy root"
    for folder in (
        home / ".config" / "OpenToonz",
        home / "Library" / "Preferences",
        home / "Library" / "Application Support" / "OpenToonz",
        root / "profiles" / "layouts" / f"settings.{user}",
        root / "profiles" / "layouts" / "settings.decoy",
    ):
        folder.mkdir(parents=True)
        (folder / "preferences.ini").write_text(DECOY_PREFERENCES, encoding="utf-8")
        (folder / "OpenToonz.ini").write_text(DECOY_PREFERENCES, encoding="utf-8")
    return {
        "HOME": str(home),
        "XDG_CONFIG_HOME": str(home / ".config"),
        "TOONZROOT": str(root),
        "TOONZPROFILES": str(root / "profiles"),
        "LC_ALL": "de_DE.UTF-8",
        "LANG": "de_DE.UTF-8",
        "TZ": "Pacific/Kiritimati",
    }


def _curve_scenes(case: NativeAcceptance) -> dict[str, Any]:
    """Author one set of curves and read back everything the tools report about them."""
    document = case.create()
    objects = {
        "rich": _peg(case, document),
        "stepped": {**_peg(case, document), "channel": "y"},
        "plain": {**_peg(case, document), "channel": "rotation", "unit": "degree"},
    }
    stepped = [{**_simple(0, 0), "step": 2}, {**_simple(10, 5), "step": 3}, _simple(20, 5)]
    case.mutate("animation.set_keys", document, **objects["rich"], keys=_rich_keys())
    case.mutate("animation.set_keys", document, **objects["stepped"], keys=stepped)
    constant = [{**_simple(0, 90), "interpolation": "constant"}, _simple(20, -90)]
    case.mutate("animation.set_keys", document, **objects["plain"], keys=constant)
    report: dict[str, Any] = {}
    reopened, _target = _reopen(case, document, "decoy-copy")
    for name, target in objects.items():
        report[name] = {
            "keys": _inspect(case, document, target)["keys"],
            "samples": _sample(case, document, target, SAMPLE_FRAMES),
        }
        again = _sample(case, reopened, _retarget(case, reopened, target), SAMPLE_FRAMES)
        case.assertEqual(again, report[name]["samples"], name)
    case.mutate("scene.discard", document)
    case.mutate("scene.discard", reopened)
    return report


def _retarget(case: NativeAcceptance, document: str, target: dict[str, str]) -> dict[str, str]:
    objects = [item for item in _summary(case, document)["objects"] if item["kind"] == "pegbar"]
    order = ["Animation peg"] * len(objects)
    case.assertEqual([item["name"] for item in objects], order)
    index = {"x": 0, "y": 1, "rotation": 2}[target["channel"]]
    return {**target, "object_id": objects[index]["object_id"]}


def _run_under(case: NativeAcceptance, name: str, changes: dict[str, str], folder: Path) -> Any:
    """Run the curve scenario on a worker launched with the given variables and folders."""
    workspace, inputs = folder / "workspace", folder / "inputs"
    workspace.mkdir(parents=True)
    inputs.mkdir()
    previous = (case.worker, case.workspace, case.inputs)
    with _environment(changes):
        worker = Worker(case.executable, workspace, inputs, case.directory / f"{name}.stderr.log")
    case.addCleanup(worker.close)
    case.worker, case.workspace, case.inputs = worker, workspace, inputs
    try:
        return _curve_scenes(case)
    finally:
        case.worker, case.workspace, case.inputs = previous


@acceptance_case
def curves_sample_identically_under_decoy_launch_settings(case: NativeAcceptance) -> None:
    """Author linear, constant, ease and speed curves with steps under two launch settings.

    The settings are plain, and user settings that disagree with every default plus another
    locale and time zone and folders with spaces. The keys and every sample, also after save
    and reopen, are identical, and a few are known by hand.
    """
    plain = _run_under(case, "plain", {}, case.directory / "plain")
    decoy = _decoy_settings(case.directory)
    decoyed = _run_under(case, "decoy", decoy, case.directory / "decoy folder (two)")
    case.assertEqual(decoyed, plain)
    values = dict(zip(SAMPLE_FRAMES, plain["plain"]["samples"], strict=True))
    case.assertEqual([values[0], values[19.75], values[20]], [90, 90, -90])
    stepped = dict(zip(SAMPLE_FRAMES, plain["stepped"]["samples"], strict=True))
    case.assertEqual([stepped[0], stepped[1.75], stepped[2], stepped[20]], [0, 0, 1, 5])
    rich = dict(zip(SAMPLE_FRAMES, plain["rich"]["samples"], strict=True))
    case.assertEqual([rich[0], rich[12], rich[24], rich[36], rich[42]], [0, 120, 240, 360, 360])


def _stroke(offset: float = 0) -> dict[str, Any]:
    points = [{"x": x + offset, "y": 0, "thickness": 1} for x in (-10, 0, 10)]
    return {"action": "add", "style_id": 1, "closed": False, "points": points}


def _drawn_scene(case: NativeAcceptance) -> dict[str, Any]:
    """A scene with a PNG level, a vector level with a stroke, a sound and animated pegs."""
    document, image, column = case.populated_scene()
    vector = case.mutate("level.create", document, name="Ink", format="vector")["level_id"]
    target = {"level_id": vector, "drawing_id": _drawing(ONE)}
    case.mutate("drawing.create", document, **target, format="vector")
    edits = {"coordinate_space": "drawing_native", "sharing": "all_exposures"}
    case.mutate("vector.edit", document, **target, **edits, edits=[_stroke()])
    inked = case.mutate("column.create", document, name="Lines")["column_id"]
    case.mutate(
        "column.expose",
        document,
        column_id=inked,
        level_id=vector,
        drawing_id=_drawing(ONE),
        start_frame=0,
        length=4,
    )
    source = tone(24000, "pcm_s16", 24000)
    voice = _import(case, document, "Voice", wav_file(source))["level_id"]
    _place(case, document, _column(case, document), voice, Clip(source, 0, 0, 10))
    peg = _peg(case, document)
    case.mutate("animation.set_keys", document, **peg, keys=_rich_keys(1 / 2400))
    return {"document": document, "image": image, "column": column, "vector": target, "peg": peg}


def _package_files(package: Path) -> dict[str, bytes]:
    return {
        str(path.relative_to(package)): path.read_bytes()
        for path in sorted(package.rglob("*"))
        if path.is_file()
    }


QUOTED = re.compile(r'"([^"]*)"')
DRIVE = re.compile(r"[A-Za-z]:[\\/]")


def _strings(value: Any) -> list[str]:
    if isinstance(value, str):
        return [value]
    items = value.values() if isinstance(value, dict) else value if isinstance(value, list) else []
    return [text for item in items for text in _strings(item)]


def _absolute(texts: list[str]) -> list[str]:
    return [text for text in texts if text.startswith("/") or DRIVE.match(text)]


@acceptance_case
def saved_package_names_no_workspace_or_input_path(case: NativeAcceptance) -> None:
    """Parse the saved scene: every path is relative to the package, none names this machine."""
    scene = _drawn_scene(case)
    saved = case.mutate("scene.save_copy", scene["document"], directory=str(case.workspace / "out"))
    package = Path(saved["package_directory"])
    files = _package_files(package)
    case.assertIn("scene.tnz", files)
    case.assertGreaterEqual(len(files), 5)
    private = {str(case.directory), str(case.directory.resolve()), str(Path.home())}
    private |= {str(case.workspace), str(case.inputs), "/private/tmp", "/var/folders"}
    for name, data in files.items():
        for root in private:
            case.assertNotIn(root.encode(), data, f"{name} names {root}")
        for runtime in case.workspace.glob("runtime_*"):
            case.assertNotIn(runtime.name.encode(), data, f"{name} names {runtime.name}")
    tnz = files["scene.tnz"].decode("utf-8")
    case.assertEqual(_absolute(QUOTED.findall(tnz)), [], "scene.tnz holds an absolute path")
    case.assertIn("$scenefolder/assets/", tnz)
    manifest = json.loads(files["manifest.json"])
    case.assertEqual(_absolute(_strings(manifest)), [], "manifest.json holds an absolute path")


@acceptance_case
def a_discarded_draft_keeps_its_job_alive_and_loses_its_private_copies(
    case: NativeAcceptance,
) -> None:
    """Discard a draft while a render job runs: the job finishes, its files are reclaimed.

    The draft's own copies of its levels go at the discard. The snapshot the job copied must
    exist while the job runs; when the job ends it is not needed (KV-3 records that it stays).
    """
    scene = _drawn_scene(case)
    document = scene["document"]
    case.assertTrue(list(case.workspace.glob("runtime_*/levels/*")))
    job = case.mutate(
        "render.submit", document, start_frame=0, end_frame=119, step=1, width=256, height=256
    )
    folder = next(case.workspace.glob(f"renders/{job['job_id']}"))
    case.assertTrue((folder / "snapshot" / "scene.tnz").is_file())
    case.mutate("scene.discard", document)
    case.assertEqual(
        case.failure("scene.summary", {"document_id": document})["code"], "document_not_found"
    )
    case.assertEqual(list(case.workspace.glob("runtime_*/levels/*")), [])
    present = (folder / "snapshot" / "scene.tnz").is_file()
    status = case.success("job.status", {"job_id": job["job_id"]})["status"]
    if status not in TERMINAL:
        case.assertTrue(present, "the snapshot went while the job was still running")
    ended = case.wait_job(job["job_id"])
    case.assertEqual(ended["status"], "completed", ended)
    artifacts = case.success("job.result", {"job_id": job["job_id"]})["artifacts"]
    case.assertEqual(len(artifacts), 120)
    case.assertEqual(list(case.workspace.glob("runtime_*/levels/*")), [])
    case.assertEqual(list(case.workspace.glob("runtime_*/sounds/*")), [])
    settle(case, "KV-3", (folder / "snapshot").exists(), False)
    case.assertEqual(case.success("automation.list_documents", {})["documents"], [])


def _content(case: NativeAcceptance, document: str) -> dict[str, Any]:
    """Everything these tools report about a scene's drawings, curves and exposures."""
    summary = _summary(case, document)
    levels = {item["name"]: item["level_id"] for item in summary["levels"]}
    rectangle = {"coordinate_space": "raster_pixels_lower_left", "x": 0, "y": 0}
    rectangle.update(width=32, height=32)
    raster = {"document_id": document, "level_id": levels["Quadrants"], **rectangle}
    raster["drawing_id"] = _drawing(ONE)
    vector = {"document_id": document, "level_id": levels["Ink"], "drawing_id": _drawing(ONE)}
    pegs = [item for item in summary["objects"] if item["kind"] == "pegbar"]
    curve = {"document_id": document, "object_id": pegs[0]["object_id"], "unit": "inch"}
    return {
        "scene": {key: value for key, value in summary.items() if key != "document_id"},
        "pixels": case.success("raster.read_pixels", raster)["pixels"],
        "strokes": [
            {key: value for key, value in stroke.items() if key != "native_id"}
            for stroke in case.success("vector.inspect", vector)["strokes"]
        ],
        "keys": [
            case.success("animation.keys", {**curve, "channel": channel})["keys"]
            for channel in ("x", "y")
        ],
        "rows": [
            _rows(case, document, column["column_id"], 12) for column in summary["columns"][:2]
        ],
    }


def _edit_duplicate(case: NativeAcceptance, document: str) -> None:
    """Change drawings, curves, exposures and names, so any shared state would show."""
    summary = _summary(case, document)
    levels = {item["name"]: item["level_id"] for item in summary["levels"]}
    case.mutate(
        "raster.patch",
        document,
        level_id=levels["Quadrants"],
        drawing_id=_drawing(ONE),
        coordinate_space="raster_pixels_lower_left",
        x=0,
        y=0,
        width=2,
        height=1,
        sharing="all_exposures",
        pixels=[{"r": 9, "g": 8, "b": 7, "a": 255}] * 2,
    )
    ink = {"level_id": levels["Ink"], "drawing_id": _drawing(ONE)}
    edits = {"coordinate_space": "drawing_native", "sharing": "all_exposures"}
    case.mutate("vector.edit", document, **ink, **edits, edits=[_stroke(3)])
    peg = next(item for item in summary["objects"] if item["kind"] == "pegbar")
    case.mutate(
        "animation.set_keys",
        document,
        object_id=peg["object_id"],
        channel="y",
        unit="inch",
        keys=[_simple(0, 1), _simple(8, 2)],
    )
    column = summary["columns"][0]["column_id"]
    case.mutate("column.update", document, column_id=column, name="Renamed", opacity=0.5)
    case.mutate(
        "column.expose",
        document,
        column_id=column,
        level_id=levels["Quadrants"],
        drawing_id=_drawing(ONE),
        start_frame=40,
        length=2,
    )


@acceptance_case
def editing_a_saved_duplicate_leaves_the_original_and_the_package_unchanged(
    case: NativeAcceptance,
) -> None:
    """Open a package twice and edit one copy: the original, the package, the other copy hold."""
    scene = _drawn_scene(case)
    original = scene["document"]
    saved = case.mutate("scene.save_copy", original, directory=str(case.workspace / "first"))
    package = Path(saved["package_directory"])
    files = _package_files(package)
    first = case.mutate("scene.open", None, path=str(package))["document_id"]
    second = case.mutate("scene.open", None, path=str(package))["document_id"]
    pristine = _content(case, original)
    for duplicate in (first, second):
        case.assertEqual(_content(case, duplicate)["pixels"], pristine["pixels"])
        case.assertEqual(_content(case, duplicate)["strokes"], pristine["strokes"])
    _edit_duplicate(case, first)
    edited = _content(case, first)
    for field in ("pixels", "strokes", "keys"):
        case.assertNotEqual(edited[field], pristine[field], field)
    case.assertEqual(_content(case, original), pristine)
    case.assertEqual(_content(case, second)["pixels"], pristine["pixels"])
    case.assertEqual(_content(case, second)["strokes"], pristine["strokes"])
    case.assertEqual(_content(case, second)["keys"], pristine["keys"])
    case.assertEqual(_package_files(package), files)
    _edit_duplicate(case, original)
    case.assertEqual(_content(case, first), edited)
    case.assertEqual(_package_files(package), files)
    again = case.mutate("scene.save_copy", first, directory=str(case.workspace / "second"))
    reopened = case.mutate("scene.open", None, path=again["package_directory"])["document_id"]
    case.assertEqual(_content(case, reopened)["pixels"], edited["pixels"])
    case.assertEqual(_content(case, reopened)["strokes"], edited["strokes"])
    case.assertEqual(_package_files(package), files)


def _noise_png(path: Path, size: int, seed: int) -> None:
    """Write a PNG of seeded random pixels, stored uncompressed so that it keeps its size."""
    generator = random.Random(seed)
    rows = b"".join(b"\x00" + generator.randbytes(size * 4) for _ in range(size))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        )

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    path.write_bytes(
        SIGNATURE
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(rows, 0))
        + chunk(b"IEND", b"")
    )


def _small_volume(case: NativeAcceptance) -> Path:
    """Mount a 64 MB volume for the case and return its mount point."""
    image = case.directory / "small.dmg"
    mount = case.directory / "small-volume"
    mount.mkdir()
    create = ["hdiutil", "create", "-size", "64m", "-fs", "APFS", "-volname", "FULLDISK"]
    subprocess.run([*create, str(image)], check=True, capture_output=True)
    attach = ["hdiutil", "attach", str(image), "-mountpoint", str(mount), "-nobrowse"]
    subprocess.run(attach, check=True, capture_output=True)
    case.addCleanup(
        subprocess.run, ["hdiutil", "detach", "-force", str(mount)], capture_output=True
    )
    return mount


def _noisy_scene(case: NativeAcceptance, levels: int) -> str:
    document = case.create("Heavy")
    column = case.mutate("column.create", document, name="Noise")["column_id"]
    for index in range(levels):
        source = case.inputs / f"noise-{index}.png"
        _noise_png(source, 384, index + 1)
        level = case.mutate("level.import", document, path=str(source), name=f"N{index}", dpi=120)
        case.mutate(
            "column.expose",
            document,
            column_id=column,
            level_id=level["level_id"],
            drawing_id=_drawing(ONE),
            start_frame=index,
            length=1,
        )
    return document


def _fill_to(mount: Path, leave: int) -> Path:
    """Fill a volume until `leave` bytes are free; `_fill_disk` can only fill it to the brim.

    It truncates the file that filled the volume, and APFS keeps the truncated blocks
    allocated, so the room it leaves does not depend on its argument.
    """
    filler = mount / "filler.bin"
    remaining = _free_bytes(mount) - leave
    block = bytes(1 << 20)
    with filler.open("wb") as handle:
        try:
            while remaining > 0:
                handle.write(block[:remaining])
                remaining -= len(block)
            handle.flush()
            os.fsync(handle.fileno())
        except OSError:
            # The volume keeps a reserve that statvfs counts as free, so a small `leave` cannot
            # be reached; the disk is then as full as it gets.
            pass
    return filler


def _save_on_full_disk(case: NativeAcceptance, document: str, target: Path) -> str:
    """Save with the disk nearly full; a refusal is typed, claims no rollback and leaves nothing."""
    revision = case.revisions[document]
    response = _raw(
        case, "scene.save_copy", case.write_params(document, {"directory": str(target)})
    )
    if "result" in response:
        case.assertTrue(Path(response["result"]["scene_path"]).is_file())
        case.revisions[document] = response["result"]["revision"]
        return "saved"
    error = response["error"]
    case.assertIn(error["code"], FULL_DISK_CODES, error)
    case.assertTrue(error["message"], error)
    case.assertNotIn("roll", (error["message"] + str(error["details"])).lower())
    case.assertFalse(target.exists(), "a refused save left its destination")
    case.assertEqual(staging_left(case.workspace), [])
    case.assertEqual(_summary(case, document)["revision"], revision)
    return "refused"


LEAVE_KB = (0, 32 * 1024, 3000)
FULL_DISK_CODES = {"package_write_failed", "io_error", "native_error", "resource_limit"}


@acceptance_case
def a_package_save_on_a_nearly_full_disk_fails_cleanly_and_then_succeeds(
    case: NativeAcceptance,
) -> None:
    """Save a scene of about 2 MB on a full disk, then with room, then with barely enough."""
    if sys.platform != "darwin":
        case.skipTest("Builds its small disk with hdiutil, which only macOS provides")
    mount = _small_volume(case)
    workspace = mount / "workspace"
    workspace.mkdir()
    case.worker.close()
    case.workspace = workspace
    case.worker = Worker(
        case.executable, workspace, case.inputs, case.directory / "disk-worker.stderr.log"
    )
    case.addCleanup(case.worker.close)
    document = _noisy_scene(case, 3)
    outcomes = []
    for index, leave in enumerate(LEAVE_KB):
        filler = _fill_to(mount, leave * 1024) if leave else _fill_disk(mount, 0)
        target = workspace / f"save-{index}"
        outcomes.append(_save_on_full_disk(case, document, target))
        filler.unlink()
        shutil.rmtree(target, ignore_errors=True)
    case.assertEqual(outcomes[:2], ["refused", "saved"], outcomes)
    case.assertGreater(_free_bytes(mount), 1024 * 1024)
    final = case.mutate("scene.save_copy", document, directory=str(workspace / "final"))
    reopened = case.mutate("scene.open", None, path=final["package_directory"])["document_id"]
    names = [item["name"] for item in _summary(case, reopened)["levels"]]
    case.assertEqual(names, ["N0", "N1", "N2"])
    case.assertEqual(_content_rows(case, reopened), _content_rows(case, document))


def _content_rows(case: NativeAcceptance, document: str) -> list[Any]:
    column = _summary(case, document)["columns"][0]["column_id"]
    names = {item["level_id"]: item["name"] for item in _summary(case, document)["levels"]}
    cells = case.success(
        "column.cells",
        {"document_id": document, "column_id": column, "start_frame": 0, "length": 3},
    )["cells"]
    return [names[cell["level_id"]] for cell in cells]


Level = tuple[int, str, str]
INK = "level-0.pli"
# Each scene adds levels to the base scene; "exposed" says whether its first fills a column.
SCENES: dict[str, tuple[bool, list[Level]]] = {
    "old": (True, [(2400, "Ink", INK)]),
    "new": (True, [(2400, "Zed", INK), (2401, "Zed", INK), (2402, "Zed", INK)]),
    "unexposed": (False, [(2400, "Zed", INK), (2401, "Zed", INK), (2402, "Zed", INK)]),
}


def _repeated_names_scene(case: NativeAcceptance, base: Path, name: str) -> Path:
    """Copy the base scene and add levels, exposing the first of them in its first column."""
    folder = case.inputs / name
    shutil.copytree(base / "assets", folder / "assets")
    exposed, levels = SCENES[name]
    text = (base / "scene.tnz").read_text()
    if exposed:
        text = _cells(text, {0: f"<level id='{levels[0][0]}'/>0001 0"})
    extra = "".join(_level(*level) for level in levels)
    anchor = "</levels>\n    <folder"
    (folder / "shot.tnz").write_text(text.replace(anchor, extra + anchor, 1))
    return folder / "shot.tnz"


def _read(case: NativeAcceptance, tool: str, document: str) -> list[dict[str, Any]]:
    read = {"document_id": document, "expected_revision": "0", "limit": 128}
    return call(case, case.worker, tool, read)["items"]


def _repeats(values: list[str]) -> int:
    return len(values) - len(set(values))


def _check_repeated_names(case: NativeAcceptance, base: Path, name: str, reference: bytes) -> None:
    opened = open_scene(case, case.worker, _repeated_names_scene(case, base, name), "standalone")
    document = opened["document_id"]
    cast = _read(case, "cast.list", document)
    dependencies = _read(case, "scene.dependencies", document)
    primary = [row["owner_id"] for row in dependencies if row["role"] == "primary"]
    repeated = (_repeats([row["level_id"] for row in cast]), _repeats(primary))
    exposed, levels = SCENES[name]
    if exposed:
        settle(case, "KV-4", repeated, (0, 0))
    else:
        case.assertEqual(repeated, (0, 0))
        case.assertEqual(len(cast), 3 + len(levels))
    listed = cast[:-1] if exposed else cast
    case.assertEqual(_repeats([row["native_cast_id"] for row in listed]), 0)
    runs = {run["column_index"]: run["level_id"] for run in _read(case, "cast.usage", document)}
    case.assertEqual(runs[0], cast[3 if exposed else 0]["level_id"])
    case.assertEqual(preview(case, case.worker, document, 0), reference)
    discard(case, case.worker, document)


@acceptance_case
def repeated_level_names_in_a_read_only_scene_stay_separate_levels(case: NativeAcceptance) -> None:
    """Open scenes whose levels repeat names: every level is listed once, with its exposure."""
    base = base_package(case)
    plain = open_scene(case, case.worker, base / "scene.tnz", "standalone")["document_id"]
    reference = preview(case, case.worker, plain, 0)
    case.assertEqual(
        [row["name"] for row in _read(case, "cast.list", plain)], ["Ink", "Cel", "Paint"]
    )
    for name in SCENES:
        with case.subTest(scene=name):
            _check_repeated_names(case, base, name, reference)
    discard(case, case.worker, plain)
