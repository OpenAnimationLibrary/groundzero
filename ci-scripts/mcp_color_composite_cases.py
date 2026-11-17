"""Qualify the color/composite FX profile bounds, keys, copies and endpoints on the worker."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from mcp_fx_authoring_cases import (
    _artwork_fixture,
    _close_worker,
    _output,
    _pixels,
    _reject_unchanged,
    _reopen,
    _scene,
    presets_cover_native_types,
)

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

PROFILE_BOUNDS: dict[str, dict[str, tuple[float, float]]] = {
    "STD_brightContFx": {"brightness": (-127, 127), "contrast": (-127, 127)},
    "STD_hsvScaleFx": {
        "hue": (-180, 180),
        "saturation": (-1, 1),
        "hue_scale": (0, 400),
        "saturation_scale": (0, 400),
        "value_scale": (0, 400),
    },
    "multFx": {"value": (-255, 255)},
    "blendFx": {"value": (0, 100)},
}
SOURCE_ONE = (200, 40, 16, 255)
SOURCE_TWO = (24, 72, 192, 255)


def _parameters(case: NativeAcceptance, document: str, effect: str, frame: int) -> dict[str, Any]:
    params = {"document_id": document, "fx_id": effect, "frame": frame}
    entries = case.success("fx.parameter.get", params)["parameters"]
    return {entry["name"]: entry for entry in entries}


def _out_of_range(minimum: float, maximum: float) -> tuple[float, float]:
    return minimum - 0.5, maximum + 0.5


def color_composite_bounds_reject_before_mutation(case: NativeAcceptance) -> None:
    """Advertise the profile bounds and reject edits past them without changing the scene."""
    document = _scene(case)
    for kind, bounds in PROFILE_BOUNDS.items():
        effect = case.mutate("fx.create", document, type=kind)["fx_id"]
        described = _parameters(case, document, effect, 0)
        for name, (minimum, maximum) in bounds.items():
            schema = described[name]["schema"]
            case.assertEqual((schema["minimum"], schema["maximum"]), (minimum, maximum), name)
            for value in _out_of_range(minimum, maximum):
                params = {"fx_id": effect, "parameter": name, "value": value}
                _reject_unchanged(case, document, "fx.parameter.set", params, "OUT_OF_RANGE")
                keyed = {**params, "frame": 3}
                _reject_unchanged(case, document, "fx.parameter.set_key", keyed, "OUT_OF_RANGE")
            for value in (minimum, maximum):
                case.mutate("fx.parameter.set", document, fx_id=effect, parameter=name, value=value)
                actual = _parameters(case, document, effect, 0)[name]["value"]
                case.assertEqual(actual, value, (kind, name))
    _close_worker(case)


def _card(case: NativeAcceptance, document: str, color: tuple[int, ...]) -> str:
    card = case.mutate("fx.create", document, type="colorCardFx", frame_count=13)["fx_id"]
    case.mutate("fx.parameter.set", document, fx_id=card, parameter="color", value=list(color))
    return card


def color_composite_blend_endpoints_select_sources(case: NativeAcceptance) -> None:
    """Cross-dissolve value 0 renders Source2 and value 100 renders Source1 exactly."""
    document = _scene(case)
    blend = case.mutate("fx.create", document, type="blendFx")["fx_id"]
    for port, color in (("Source1", SOURCE_ONE), ("Source2", SOURCE_TWO)):
        source = _card(case, document, color)
        case.mutate("fx.connect", document, fx_id=blend, port=port, source_fx_id=source)
    _output(case, document, blend)
    for value, expected in ((0, SOURCE_TWO), (100, SOURCE_ONE)):
        case.mutate("fx.parameter.set", document, fx_id=blend, parameter="value", value=value)
        case.assertEqual(set(_pixels(case, document, 6)), {expected}, value)
    _close_worker(case)


def color_composite_keys_survive_copy_and_reopen(case: NativeAcceptance) -> None:
    """Keep keyed HSV animation independent in copies and exact through save and reopen."""
    document, artwork = _artwork_fixture(case)
    hsv = case.mutate("fx.create", document, type="STD_hsvScaleFx", name="Keyed hue")["fx_id"]
    case.mutate("fx.connect", document, fx_id=hsv, port="Source", source_fx_id=artwork)
    for frame, hue in ((0, -90), (12, 90)):
        case.mutate(
            "fx.parameter.set_key", document, fx_id=hsv, parameter="hue", frame=frame, value=hue
        )
    _output(case, document, hsv)
    rendered = {frame: _pixels(case, document, frame) for frame in (0, 6, 12)}
    case.assertEqual(len({tuple(pixels) for pixels in rendered.values()}), 3)
    before = _parameters(case, document, hsv, 6)
    copy = case.mutate("fx.duplicate", document, fx_id=hsv, name="Copied hue")["fx_id"]
    case.assertEqual(_parameters(case, document, copy, 6)["hue"], before["hue"])
    case.mutate("fx.parameter.set_key", document, fx_id=copy, parameter="hue", frame=12, value=0)
    case.assertEqual(_parameters(case, document, hsv, 6), before)
    restored, names = _reopen(case, document)
    case.assertEqual(_parameters(case, restored, names["Keyed hue"], 6), before)
    _output(case, restored, names["Keyed hue"])
    for frame, pixels in rendered.items():
        case.assertEqual(_pixels(case, restored, frame), pixels, frame)
    _close_worker(case)


def color_presets_cover_native_types(case: NativeAcceptance) -> None:
    """Replay presets, copies and reopened renders for the four color-adjustment FX."""
    presets_cover_native_types(case, ("STD_brightContFx", "STD_hsvScaleFx", "multFx", "invertFx"))


def composite_presets_cover_native_types(case: NativeAcceptance) -> None:
    """Replay presets, copies and reopened renders for the four compositing FX."""
    presets_cover_native_types(case, ("screenFx", "inFx", "outFx", "blendFx"))
