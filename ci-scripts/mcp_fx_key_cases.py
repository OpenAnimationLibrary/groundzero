"""Rich keys on effect parameter channels through the animation key tools.

An effect channel takes the same key edits as a stage channel (set, delete, move, duplicate,
breakdown, retime, copy). What the worker reads back and samples is compared with the
independent curve model of `mcp_property_curves`; renders at key frames match the samples; a key
or a speed handle outside the parameter's admitted bounds is refused with the document unchanged.
"""

from __future__ import annotations

import base64
import json
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_png import Pixel, decode_png
from mcp_property_curves import (
    Key,
    duplicate_keys,
    insert_breakdown,
    key_record,
    move_keys,
    ordered,
    sample,
    shape_is_defined,
)

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

BLUR = {"channel": "value", "unit": "native_fxLength"}
RED = {"channel": "color.Red", "unit": "native_colorChannel"}
BLUE = {"channel": "color.Blue", "unit": "native_colorChannel"}
SAMPLE_FRAMES = [0, 1.5, 3, 6, 9.25, 12, 14, 17.5, 20, 23, 24, 30, 36, 40]
TOLERANCE = 1e-6


def _blur_keys() -> list[Key]:
    keys = [
        Key(0, 10, "ease_in_out", outgoing=("ease", 3)),
        Key(12, 150, "speed_in_out", incoming=("ease", 2), outgoing=("speed", 4, 30)),
        Key(24, 40, "linear", step=2, incoming=("speed", -2, 20)),
        Key(36, 0, "constant"),
    ]
    for key in keys:
        key.linked = key.kind() == "none" and key.incoming[0] == "none"
    return keys


def _row(key: dict[str, Any]) -> tuple[Any, ...]:
    """A key as animation.keys reports it, reduced to what the model states."""
    incoming = key["incoming"]
    outgoing = key.get("outgoing")
    return (
        key["frame"],
        key["value"],
        key["interpolation"],
        key["step"],
        tuple(v for k, v in sorted(incoming.items())),
        None if outgoing is None else tuple(v for _, v in sorted(outgoing.items())),
    )


def _model_row(key: Key) -> tuple[Any, ...]:
    record = key_record(key)
    incoming = record.get("incoming", {"kind": "none"})
    outgoing = record.get("outgoing")
    return (
        key.frame,
        key.value,
        key.interpolation,
        key.step,
        tuple(v for _, v in sorted(incoming.items())),
        None if outgoing is None else tuple(v for _, v in sorted(outgoing.items())),
    )


def _target(document: str, effect: str, channel: dict[str, str]) -> dict[str, Any]:
    return {"document_id": document, "fx_id": effect, **channel}


def _assert_model(
    case: NativeAcceptance,
    document: str,
    effect: str,
    channel: dict[str, str],
    keys: dict[int, Key],
) -> None:
    """The worker's keys and samples of an effect channel equal the model's."""
    target = _target(document, effect, channel)
    read = case.success("animation.keys", target)["keys"]
    case.assertEqual([_row(key) for key in read], [_model_row(key) for key in ordered(keys)])
    if not keys or not shape_is_defined(keys):
        return
    values = case.success("animation.sample", {**target, "frames": SAMPLE_FRAMES})["values"]
    for item in values:
        want = sample(ordered(keys), item["frame"])
        case.assertAlmostEqual(item["value"], want, delta=TOLERANCE, msg=item)


def _blur(case: NativeAcceptance, document: str, name: str) -> str:
    return case.mutate("fx.create", document, type="STD_blurFx", name=name)["fx_id"]


def _card(case: NativeAcceptance, document: str, name: str) -> str:
    return case.mutate("fx.create", document, type="colorCardFx", name=name, frame_count=25)[
        "fx_id"
    ]


def _pixel(case: NativeAcceptance, document: str, frame: int) -> Pixel:
    job = case.mutate("render.preview", document, frame=frame, width=8, height=8)
    status = case.wait_job(job["job_id"])
    case.assertEqual(status["status"], "completed", status)
    artifacts = case.success("job.result", {"job_id": job["job_id"]})["artifacts"]
    image = case.success("artifact.read", {"artifact_id": artifacts[0]["artifact_id"]})
    _width, _height, pixels = decode_png(base64.b64decode(image["image"]["data"], validate=True))
    case.assertEqual(len(set(pixels)), 1, frame)
    return pixels[0]


def _color_keys() -> tuple[dict[int, Key], dict[int, Key]]:
    red = {0: Key(0, 0.2, "ease_in_out", outgoing=("ease", 2)), 8: Key(8, 0.9, "linear")}
    red[8].incoming = ("ease", 3)
    blue = {4: Key(4, 1.0, "constant"), 12: Key(12, 0.0, "linear")}
    for keys in (red, blue):
        for key in keys.values():
            key.linked = key.kind() == "none" and key.incoming[0] == "none"
    return red, blue


def _set(
    case: NativeAcceptance, document: str, effect: str, channel: dict[str, str], keys: list[Key]
) -> dict[str, Any]:
    records = [key_record(key) for key in keys]
    return case.mutate("animation.set_keys", document, fx_id=effect, **channel, keys=records)


def _assert_renders(
    case: NativeAcceptance, document: str, red: dict[int, Key], blue: dict[int, Key]
) -> None:
    for frame in (0, 4, 6, 8, 12):
        pixel = _pixel(case, document, frame)
        want_red = sample(ordered(red), frame) * 255
        want_blue = sample(ordered(blue), frame) * 255
        case.assertAlmostEqual(pixel[0], want_red, delta=1, msg=(frame, pixel))
        case.assertAlmostEqual(pixel[2], want_blue, delta=1, msg=(frame, pixel))


def _reopen(case: NativeAcceptance, document: str, folder: str) -> tuple[str, dict[str, str]]:
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / folder))
    case.mutate("scene.discard", document)
    opened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    graph = case.success("fx.graph", {"document_id": opened})
    return opened, {item["name"]: item["fx_id"] for item in graph["effects"]}


@acceptance_case
def fx_rich_keys_per_channel(case: NativeAcceptance) -> None:
    """Ease and speed keys on a blur value and per-channel color keys at different frames sample
    as the independent model computes, render at the key frames as sampled, and survive save and
    reopen with kind, step and controls; a speed handle outside the admitted range is
    OUT_OF_RANGE with the document unchanged."""
    document = case.create("Rich FX keys")
    blur = _blur(case, document, "Blur")
    card = _card(case, document, "Card")
    described = case.success("animation.channels", {"document_id": document, "fx_id": blur})
    value = next(item for item in described["channels"] if item["channel"] == "value")
    case.assertEqual(
        (value["edit_via"], value["writable"], value["value_bounds"], value["unit"]),
        ("animation.set_keys", True, [0, 200], BLUR["unit"]),
    )
    case.assertEqual(value["interpolations"], ["constant", "linear", "ease_in_out", "speed_in_out"])
    keys = _blur_keys()
    written = _set(case, document, blur, BLUR, keys)
    case.assertEqual((written["affected_fx_ids"], written["linked_group_id"]), ([blur], None))
    entries = written["graph_delta"]["entries"]
    case.assertEqual(
        [(e["kind"], e["fx_id"], e["cause"]) for e in entries], [("node", blur, "requested")]
    )
    case.assertIn("parameters", entries[0]["aspects"])
    _assert_model(case, document, blur, BLUR, {key.frame: key for key in keys})
    red, blue = _color_keys()
    _set(case, document, card, RED, ordered(red))
    _set(case, document, card, BLUE, ordered(blue))
    output = case.success("fx.graph", {"document_id": document})["current_output_fx_id"]
    case.mutate("fx.connect", document, fx_id=output, port="source", source_fx_id=card)
    _assert_model(case, document, card, RED, red)
    _assert_model(case, document, card, BLUE, blue)
    _assert_renders(case, document, red, blue)
    _refuse_outside_handle(case, document, blur)
    opened, names = _reopen(case, document, "rich-fx")
    _assert_model(case, opened, names["Blur"], BLUR, {key.frame: key for key in keys})
    _assert_model(case, opened, names["Card"], RED, red)
    _assert_model(case, opened, names["Card"], BLUE, blue)
    _assert_renders(case, opened, red, blue)


def _refuse_outside_handle(case: NativeAcceptance, document: str, blur: str) -> None:
    before = case.success("animation.keys", _target(document, blur, BLUR))
    revision = case.revisions[document]
    keys = _blur_keys()
    keys[1].outgoing = ("speed", 4, 60)
    request = {"fx_id": blur, **BLUR, "keys": [key_record(key) for key in keys]}
    error = case.handler_failure(
        "animation.set_keys", case.write_params(document, request), "OUT_OF_RANGE"
    )
    case.assertEqual(
        {key: error["details"].get(key) for key in ("fx_id", "parameter", "channel", "frame")},
        {"fx_id": blur, "parameter": "value", "channel": "value", "frame": 12},
    )
    case.assertEqual(case.revisions[document], revision)
    case.assertEqual(case.success("animation.keys", _target(document, blur, BLUR)), before)


def _model_edit(
    tool: str, keys: dict[int, Key], params: dict[str, Any]
) -> tuple[str | None, dict[int, Key]]:
    """What the independent model makes of a key edit: its refusal, or the edited keys."""
    if tool == "animation.insert_breakdown":
        return insert_breakdown(keys, params["frame"], params.get("favour_percent"), None)
    edit = duplicate_keys if tool == "animation.duplicate_keys" else move_keys
    return edit(keys, params["frames"], params["frame_offset"], params["value_offset"])


@acceptance_case
def fx_key_edits_follow_the_independent_model(case: NativeAcceptance) -> None:
    """Move, duplicate, breakdown, delete, retime and copy on effect channels give the keys and
    samples the independent model computes; a move that leaves the bounds is refused."""
    document = case.create("FX key edits")
    blur = _blur(case, document, "Blur")
    keys = {key.frame: key for key in _blur_keys()}
    _set(case, document, blur, BLUR, ordered(keys))
    for tool, params in (
        ("animation.insert_breakdown", {"frame": 6, "favour_percent": 25}),
        ("animation.move_keys", {"frames": [36], "frame_offset": 6, "value_offset": 5}),
        ("animation.duplicate_keys", {"frames": [42], "frame_offset": 6, "value_offset": 10}),
    ):
        with case.subTest(tool=tool):
            error, keys = _model_edit(tool, keys, params)
            case.assertIsNone(error)
            result = case.mutate(tool, document, fx_id=blur, **BLUR, **params)
            case.assertEqual(result["affected_fx_ids"], [blur])
            _assert_model(case, document, blur, BLUR, keys)
    case.assertIsNone(move_keys(keys, [12], 0, 60)[0])
    request = {"fx_id": blur, **BLUR, "frames": [12], "frame_offset": 0, "value_offset": 60}
    case.handler_failure(
        "animation.move_keys", case.write_params(document, request), "OUT_OF_RANGE"
    )
    case.mutate("animation.delete_keys", document, fx_id=blur, **BLUR, frames=[48])
    del keys[48]
    _assert_model(case, document, blur, BLUR, keys)
    retimed = case.mutate(
        "animation.retime_curve",
        document,
        fx_id=blur,
        **BLUR,
        origin_frame=0,
        shift_frames=0,
        scale={"numerator": 2, "denominator": 1},
        step_policy="scale_exact",
    )
    case.assertEqual(retimed["fx_id"], blur)
    case.assertEqual([key["frame"] for key in retimed["keys"]], [2 * f for f in sorted(keys)])
    copy = _blur(case, document, "Copy")
    copied = case.mutate(
        "animation.copy_curve",
        document,
        source={"fx_id": blur, **BLUR},
        destination={"fx_id": copy, **BLUR},
        destination_policy="replace_all",
    )
    case.assertEqual(copied["affected_fx_ids"], [copy])
    case.assertEqual(
        case.success("animation.keys", _target(document, copy, BLUR))["keys"],
        case.success("animation.keys", _target(document, blur, BLUR))["keys"],
    )
    case.mutate("animation.delete_keys", document, fx_id=blur, **BLUR, frames=[0])
    case.assertEqual(len(case.success("animation.keys", _target(document, copy, BLUR))["keys"]), 5)


@acceptance_case
def fx_key_writes_through_links_and_refusals(case: NativeAcceptance) -> None:
    """A key write to one linked effect changes and reports every member; a locked member
    column, a gradient point's linear-only curve, an unknown channel or unit, mixed targets and
    a cycle on an effect channel are refused before any change."""
    document = case.create("FX key refusals")
    first = _card(case, document, "First")
    second = _card(case, document, "Second")
    linked = case.mutate("fx.link.add", document, fx_id=second, linked_to_fx_id=first)
    group = linked["linked_group"]["group_id"]
    red, _blue = _color_keys()
    written = _set(case, document, second, RED, ordered(red))
    case.assertEqual(
        (written["affected_fx_ids"], written["linked_group_id"]), ([first, second], group)
    )
    changed = {e["fx_id"] for e in written["graph_delta"]["entries"] if e["kind"] == "node"}
    case.assertEqual(changed, {first, second})
    _assert_model(case, document, first, RED, red)
    column = next(
        e["column_id"]
        for e in case.success("fx.graph", {"document_id": document})["effects"]
        if e["fx_id"] == first
    )
    case.mutate("column.update", document, column_id=column, locked=True)
    request = {"fx_id": second, **RED, "frames": [8]}
    case.handler_failure("animation.delete_keys", case.write_params(document, request), "locked")
    case.mutate("column.update", document, column_id=column, locked=False)
    gradient = case.mutate("fx.create", document, type="STD_iwa_LinearGradientFx")["fx_id"]
    point = case.success("animation.channels", {"document_id": document, "fx_id": gradient})
    start = next(item for item in point["channels"] if item["channel"].startswith("startPoint."))
    case.assertEqual((start["interpolations"], start["step_bounds"]), (["linear"], [1, 1]))
    eased = [key_record(key) for key in ordered(red)]
    request = {"fx_id": gradient, "channel": start["channel"], "unit": start["unit"], "keys": eased}
    case.handler_failure(
        "animation.set_keys", case.write_params(document, request), "UNSUPPORTED_PARAMETER"
    )
    for params, code in (
        ({"fx_id": first, "object_id": "object-1", **RED, "keys": eased}, "invalid_params"),
        ({"fx_id": first, **RED, "layer_id": "layer-1", "keys": eased}, "invalid_params"),
        (
            {"fx_id": first, "channel": "color.Cyan", "unit": RED["unit"], "keys": eased},
            "unsupported_channel",
        ),
        ({"fx_id": first, "channel": "color.Red", "unit": "inch", "keys": eased}, "invalid_unit"),
    ):
        with case.subTest(code=code, params=sorted(params)):
            case.handler_failure("animation.set_keys", case.write_params(document, params), code)
    cycle = {"fx_id": first, **RED, "enabled": True}
    case.schema_failure("animation.set_cycle", case.write_params(document, cycle))


def _parameter(items: list[dict[str, Any]], name: str) -> dict[str, Any]:
    return next(item for item in items if item["name"] == name)


@acceptance_case
def fx_parameter_descriptors_separate_defaults_bounds_and_units(case: NativeAcceptance) -> None:
    """Catalog and fx.parameter.get descriptors give kind, default, admitted, native and slider
    bounds with their source, measure and unit, menu choices by id, origin and hidden status;
    fx.parameter.get adds the sample, whether the parameter is animated and its edit
    constraint (none, a linked group or a locked column)."""
    catalog = case.success("fx.catalog", {})
    case.assertLess(len(json.dumps(catalog)), 64 * 1024)
    case.assertEqual(catalog["menu_categories"], "not_reported")
    entries = {entry["type"]: entry for entry in catalog["effects"] if entry["available"]}
    blur = entries["STD_blurFx"]
    case.assertEqual(
        (blur["origin"], blur["hidden"]), ({"kind": "builtin", "plugin_id": "STD"}, False)
    )
    case.assertEqual(blur["profile_version"], blur["native_fx_version"])
    value = _parameter(blur["parameters"], "value")
    case.assertEqual(
        (value["kind"], value["default"], value["measure"], value["unit"]),
        ("number", 20, "fxLength", BLUR["unit"]),
    )
    case.assertEqual(
        value["bounds"],
        {
            "admitted": {"minimum": 0, "maximum": 200},
            "native": {"minimum": 0, "maximum": None},
            "slider": value["bounds"]["slider"],
            "source": "profile",
        },
    )
    color = _parameter(entries["colorCardFx"]["parameters"], "color")
    case.assertEqual((color["kind"], color["default"]), ("color", [0, 255, 0, 255]))
    case.assertEqual(color["bounds"]["admitted"], {"minimum": 0, "maximum": 255})
    curve = _parameter(entries["STD_iwa_LinearGradientFx"]["parameters"], "curveType")
    case.assertEqual((curve["kind"], curve["label_is_identity"]), ("enum", False))
    case.assertEqual([choice["id"] for choice in curve["choices"]], curve["schema"]["enum"])
    document = case.create("Descriptors")
    first = _card(case, document, "First")
    second = _card(case, document, "Second")
    request = {"document_id": document, "fx_id": first, "frame": 3}
    read = case.success("fx.parameter.get", request)
    case.assertEqual(
        (read["revision"], read["type"], read["frame"]),
        (case.revisions[document], "colorCardFx", 3),
    )
    shown = _parameter(read["parameters"], "color")
    case.assertEqual(shown["sample"], {"frame": 3, "value": shown["value"]})
    case.assertEqual((shown["animated"], shown["edit_constraint"]), (False, None))
    group = case.mutate("fx.link.add", document, fx_id=second, linked_to_fx_id=first)
    _set(case, document, first, RED, [Key(0, 0.5, "linear")])
    shown = _parameter(case.success("fx.parameter.get", request)["parameters"], "color")
    case.assertTrue(shown["animated"])
    case.assertEqual(
        shown["edit_constraint"],
        {"kind": "linked_parameters", "via": group["linked_group"]["group_id"]},
    )
    graph = case.success("fx.graph", {"document_id": document})["effects"]
    column = next(e["column_id"] for e in graph if e["fx_id"] == second)
    case.mutate("column.update", document, column_id=column, locked=True)
    shown = _parameter(case.success("fx.parameter.get", request)["parameters"], "color")
    case.assertEqual(shown["edit_constraint"], {"kind": "locked_column", "via": column})
