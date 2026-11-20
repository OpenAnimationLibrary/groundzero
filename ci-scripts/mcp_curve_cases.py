"""Qualify whole-curve ownership and exact timing using the native worker."""

from __future__ import annotations

from copy import deepcopy
from typing import TYPE_CHECKING, Any

from mcp_animation_cases import (
    _animated_scene,
    _inspect,
    _peg,
    _pixels_match,
    _render,
    _reopen,
    _rich_keys,
    _sample,
    _simple,
    _target,
)
from mcp_case import acceptance_case

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


def _mapping(numerator: int, denominator: int, shift: int = 0) -> dict[str, Any]:
    return {
        "origin_frame": 0,
        "shift_frames": shift,
        "scale": {"numerator": numerator, "denominator": denominator},
        "step_policy": "scale_exact",
    }


def _copy(source: dict[str, str], destination: dict[str, str]) -> dict[str, Any]:
    return {"source": source, "destination": destination, "destination_policy": "replace_all"}


@acceptance_case
def whole_curve_copy_preserves_independence_and_receipts(case: NativeAcceptance) -> None:
    """Copy every supported unit, retain exact receipts, and replace with empty defaults."""
    document = case.create()
    source, destination = _peg(case, document), _peg(case, document)
    channels = {"x": "inch", "y": "inch", "z": "native_z", "rotation": "degree"}
    channels.update(scale="scalar", scale_x="scalar", scale_y="scalar")
    for channel, unit in channels.items():
        left = {**source, "channel": channel, "unit": unit}
        right = {**destination, "channel": channel, "unit": unit}
        case.mutate("animation.set_keys", document, **left, keys=_rich_keys())
        request = case.write_params(document, _copy(left, right))
        copied = case.success("animation.copy_curve", request)
        case.revisions[document] = copied["revision"]
        case.assertTrue(copied["independent"])
        case.assertEqual(copied["affected_domains"], ["destination_channel"])
        case.assertEqual(copied["keys"], _inspect(case, document, left)["keys"])
        case.assertEqual(case.success("animation.copy_curve", request), copied)
        revision = case.revisions[document]
        same = case.mutate("animation.copy_curve", document, **_copy(right, right))
        case.assertEqual(same["revision"], revision)
        same = case.mutate("animation.copy_curve", document, **_copy(left, right))
        case.assertEqual(same["revision"], revision)
        case.mutate("animation.delete_keys", document, **left, frames=[0, 12, 24, 36])
        case.assertEqual(_inspect(case, document, right)["keys"], copied["keys"])
        emptied = case.mutate("animation.copy_curve", document, **_copy(left, right))
        case.assertEqual(emptied["keys"], [])
        default = 1 if unit == "scalar" else 0
        case.assertEqual(emptied["default_value"], default)
        case.assertEqual(_sample(case, document, right, [0, 6.5]), [default, default])
    case.mutate("animation.set_keys", document, **source, keys=[_simple(0, 3)])
    cross = {**destination, "channel": "y"}
    copied = case.mutate("animation.copy_curve", document, **_copy(source, cross))
    case.assertEqual(copied["keys"][0]["value"], 3)


def _samples_match(case: NativeAcceptance, actual: list[float], expected: list[float]) -> None:
    for value, reference in zip(actual, expected, strict=True):
        case.assertAlmostEqual(value, reference, delta=1e-7)


@acceptance_case
def whole_curve_retime_preserves_samples_rendering_and_reopen(case: NativeAcceptance) -> None:
    """Verify rational retime against pre-edit samples, native pixels and saved generations."""
    document, target = _animated_scene(case)
    camera = case.success("camera.inspect", {"document_id": document})
    case.mutate("animation.delete_keys", document, **_target(camera["object_id"]), frames=[0, 36])
    frames = [index / 4 for index in range(145)]
    values = _sample(case, document, target, frames)
    pixels = [_render(case, document, frame) for frame in (0, 6, 12, 18)]
    before = case.success("scene.summary", {"document_id": document})
    request = case.write_params(document, {**target, **_mapping(3, 2, 4)})
    result = case.success("animation.retime_curve", request)
    case.revisions[document] = result["revision"]
    case.assertEqual(result["affected_domains"], ["target_channel"])
    case.assertEqual([key["frame"] for key in result["keys"]], [4, 22, 40, 58])
    case.assertEqual(result["keys"][1]["incoming"]["frame_delta"], -6)
    case.assertEqual(case.success("animation.retime_curve", request), result)
    after = case.success("scene.summary", {"document_id": document})
    case.assertEqual(after["columns"], before["columns"])
    mapped = [frame * 1.5 + 4 for frame in frames]
    _samples_match(case, _sample(case, document, target, mapped), values)
    for frame, expected in zip((4, 13, 22, 31), pixels, strict=True):
        _pixels_match(case, _render(case, document, frame), expected)
    revision = case.revisions[document]
    identity = case.mutate("animation.retime_curve", document, **target, **_mapping(7, 7))
    case.assertEqual(identity["revision"], revision)
    reopened, reopened_target = _reopen(case, document, "retimed-copy")
    case.mutate("scene.discard", document)
    _samples_match(case, _sample(case, reopened, reopened_target, mapped), values)
    for frame, expected in zip((4, 13, 22, 31), pixels, strict=True):
        _pixels_match(case, _render(case, reopened, frame), expected)


def _reject_unchanged(
    case: NativeAcceptance, document: str, method: str, params: dict[str, Any]
) -> None:
    before = case.success("scene.summary", {"document_id": document})
    case.failure(method, case.write_params(document, params))
    case.assertEqual(case.success("scene.summary", {"document_id": document}), before)


@acceptance_case
def whole_curve_rejections_and_quantized_steps(case: NativeAcceptance) -> None:
    """Refuse nonexact timing, locks and invalid endpoints without changing keys or revisions."""
    document = case.create()
    source, target = _peg(case, document), _peg(case, document)
    case.mutate("animation.set_keys", document, **source, keys=_rich_keys())
    case.mutate("animation.copy_curve", document, **_copy(source, target))
    before = _inspect(case, document, target)
    for mapping in (_mapping(1, 5), _mapping(1, 1, -1), _mapping(1000000, 1), _mapping(0, 1)):
        _reject_unchanged(case, document, "animation.retime_curve", {**target, **mapping})
        case.assertEqual(_inspect(case, document, target), before)
    for bad in (
        {**target, "unit": "degree"},
        {**target, "object_id": "missing"},
        {**target, "channel": "rotation", "unit": "degree"},
    ):
        _reject_unchanged(case, document, "animation.copy_curve", _copy(source, bad))
    stepped = deepcopy(_rich_keys())
    stepped[0]["step"] = 2
    case.mutate("animation.set_keys", document, **target, keys=stepped)
    _reject_unchanged(case, document, "animation.retime_curve", {**target, **_mapping(1, 2)})
    frames = [0, 1.99, 2, 2.01, 5.99, 6, 12, 18, 36]
    values = _sample(case, document, target, frames)
    doubled = case.mutate("animation.retime_curve", document, **target, **_mapping(2, 1, 3))
    case.assertEqual([key["step"] for key in doubled["keys"]], [4, 1, 1, 1])
    for actual, expected in zip(
        _sample(case, document, target, [frame * 2 + 3 for frame in frames]), values, strict=True
    ):
        case.assertAlmostEqual(actual, expected, delta=1e-7)
    stale = case.write_params(document, {**target, **_mapping(1, 1)})
    stale["expected_revision"] = "0"
    case.assertEqual(case.failure("animation.retime_curve", stale)["code"], "revision_conflict")
    column = case.mutate("column.create", document, name="Locked")
    case.mutate("column.update", document, column_id=column["column_id"], locked=True)
    locked = _target(column["object_id"])
    _reject_unchanged(case, document, "animation.copy_curve", _copy(source, locked))
    _reject_unchanged(case, document, "animation.retime_curve", {**locked, **_mapping(1, 1)})
    copied = case.mutate("animation.copy_curve", document, **_copy(locked, target))
    case.assertEqual(copied["keys"], [])
