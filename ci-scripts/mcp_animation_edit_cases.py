"""Qualify breakdown, key move, key duplicate and cyclic curves against the native worker."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from mcp_animation_cases import _inspect, _peg, _reopen, _rich_keys, _sample, _simple, _target
from mcp_case import acceptance_case

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

DENSE = [index / 4 for index in range(0, 4 * 56)]
NATIVE_DELTA = 1e-9


def _curve_keys() -> list[dict[str, Any]]:
    """Speed, ease, linear, constant and linear segments across 0 to 48."""
    keys = _rich_keys()
    return [*keys, _simple(48, 100)]


def _by_frame(keys: list[dict[str, Any]]) -> dict[float, dict[str, Any]]:
    return {key["frame"]: key for key in keys}


def _samples(case: NativeAcceptance, document: str, target: dict[str, str]) -> list[float]:
    # Sample requests are bounded per call; the grid fits one call.
    return _sample(case, document, target, DENSE)


def _expect_untouched(
    case: NativeAcceptance,
    before: dict[str, Any],
    after: dict[str, Any],
    touched: set[float],
    window: tuple[float, float],
) -> None:
    """Every key outside touched and every sample outside window is exactly as before."""
    old, new = _by_frame(before["keys"]), _by_frame(after["keys"])
    for frame, key in old.items():
        if frame not in touched:
            case.assertEqual(new[frame], key, f"key {frame} changed")
    for frame, value in zip(DENSE, before["samples"], strict=True):
        if not window[0] < frame < window[1]:
            case.assertEqual(after["samples"][DENSE.index(frame)], value, f"sample {frame}")


def _state(case: NativeAcceptance, document: str, target: dict[str, str]) -> dict[str, Any]:
    state = _inspect(case, document, target)
    state["samples"] = _samples(case, document, target)
    return state


def _setup(case: NativeAcceptance) -> tuple[str, dict[str, str]]:
    document = case.create()
    target = _peg(case, document)
    case.mutate("animation.set_keys", document, **target, keys=_curve_keys())
    return document, target


@acceptance_case
def breakdown_splits_each_segment_and_keeps_the_rest(case: NativeAcceptance) -> None:
    """Insert breakdowns into speed, ease, linear and constant segments; reopen unchanged."""
    document, target = _setup(case)
    ends = {5: (0, 12, 25), 18: (12, 24, 50), 30: (24, 36, 110), 40: (36, 48, -10)}
    for frame, (previous, following, favour) in ends.items():
        before = _state(case, document, target)
        old = _by_frame(before["keys"])
        result = case.mutate(
            "animation.insert_breakdown",
            document,
            **target,
            frame=frame,
            favour_percent=favour,
        )
        after = _state(case, document, target)
        expected = old[previous]["value"] + (old[following]["value"] - old[previous]["value"]) * (
            favour / 100
        )
        case.assertEqual(result["inserted_frame"], frame)
        case.assertAlmostEqual(result["inserted_value"], expected, delta=NATIVE_DELTA)
        new = _by_frame(after["keys"])
        case.assertEqual(len(after["keys"]), len(before["keys"]) + 1)
        case.assertEqual(new[frame]["interpolation"], old[previous]["interpolation"])
        case.assertEqual(new[frame]["step"], old[previous]["step"])
        _expect_untouched(case, before, after, {previous, following}, (previous, following))
        for edge in (previous, following):
            for field in ("frame", "value", "interpolation", "step"):
                case.assertEqual(new[edge][field], old[edge][field])
    final = _state(case, document, target)
    reopened, reopened_target = _reopen(case, document, "breakdown-reopen")
    again = _state(case, reopened, reopened_target)
    case.assertEqual(again["keys"], final["keys"])
    for actual, expected in zip(again["samples"], final["samples"], strict=True):
        case.assertAlmostEqual(actual, expected, delta=NATIVE_DELTA)


@acceptance_case
def breakdown_by_value_and_retries(case: NativeAcceptance) -> None:
    """An explicit value works, an exact retry replays, and refusals change nothing."""
    document, target = _setup(case)
    request = case.write_params(document, {**target, "frame": 30, "value": 777})
    first = case.success("animation.insert_breakdown", request)
    case.revisions[document] = first["revision"]
    case.assertEqual(case.success("animation.insert_breakdown", request), first)
    case.assertEqual(_by_frame(_inspect(case, document, target)["keys"])[30]["value"], 777)
    before = _state(case, document, target)
    revision = case.revisions[document]
    for fields in (
        {"frame": 30, "value": 1},
        {"frame": 0, "value": 1},
        {"frame": 49, "value": 1},
        {"frame": 5},
        {"frame": 5, "value": 1, "favour_percent": 10},
        {"frame": 5, "favour_percent": 201},
        {"frame": 5, "value": 2000000},
        {"frame": 5.5, "value": 1},
    ):
        case.failure(
            "animation.insert_breakdown", case.write_params(document, {**target, **fields})
        )
        case.assertEqual(_state(case, document, target), before)
    case.assertEqual(case.success("scene.summary", {"document_id": document})["revision"], revision)
    case.failure(
        "animation.insert_breakdown",
        case.write_params(
            document, {**target, "channel": "path_position", "unit": "percent", "frame": 5}
        ),
    )
    case.failure(
        "animation.insert_breakdown",
        case.write_params(document, {**target, "layer_id": "x", "frame": 5, "value": 1}),
    )
    column_document, _level, column = case.populated_scene()
    column_target = {"object_id": column["object_id"], "channel": "x", "unit": "inch"}
    case.mutate("animation.set_keys", column_document, **column_target, keys=_curve_keys())
    case.mutate("column.update", column_document, column_id=column["column_id"], locked=True)
    case.failure(
        "animation.insert_breakdown",
        case.write_params(column_document, {**column_target, "frame": 5, "value": 1}),
    )


def _refused(
    case: NativeAcceptance,
    tool: str,
    document: str,
    target: dict[str, str],
    reason: str,
    **fields: Any,
) -> None:
    """Require a refusal that names its reason and leaves keys, samples and revision alone."""
    before = _state(case, document, target)
    revision = case.revisions[document]
    error = case.failure(tool, case.write_params(document, {**target, **fields}))
    case.assertEqual(error["code"], "invalid_argument", error)
    case.assertEqual(error["details"].get("reason"), reason, error)
    case.assertEqual(_state(case, document, target), before)
    case.assertEqual(case.success("scene.summary", {"document_id": document})["revision"], revision)


@acceptance_case
def move_keys_keep_every_other_key_and_segment(case: NativeAcceptance) -> None:
    """Move one key, a group and a value only; untouched keys and segments stay identical."""
    document, target = _setup(case)
    moves = [([36], 4, 10, (24, 48)), ([12, 24], 3, 0, (0, 40))]
    for frames, frame_offset, value_offset, window in moves:
        before = _state(case, document, target)
        old = _by_frame(before["keys"])
        result = case.mutate(
            "animation.move_keys",
            document,
            **target,
            frames=frames,
            frame_offset=frame_offset,
            value_offset=value_offset,
        )
        after = _state(case, document, target)
        new = _by_frame(after["keys"])
        case.assertEqual(result["moved_frames"], sorted(f + frame_offset for f in frames))
        case.assertEqual(len(after["keys"]), len(before["keys"]))
        for frame in frames:
            source, moved = old[frame], new[frame + frame_offset]
            for field in ("interpolation", "step", "incoming", "outgoing", "linked_handles"):
                case.assertEqual(moved.get(field), source.get(field), field)
            case.assertAlmostEqual(moved["value"], source["value"] + value_offset, delta=1e-9)
        _expect_untouched(
            case, before, after, set(frames) | {f + frame_offset for f in frames}, window
        )
    _check_group_shape(case, document, target)
    before = _state(case, document, target)
    case.mutate("animation.move_keys", document, **target, frames=[27], value_offset=5)
    after = _state(case, document, target)
    _expect_untouched(case, before, after, {27}, (15, 40))
    reopened, reopened_target = _reopen(case, document, "move-reopen")
    final = _state(case, document, target)
    again = _state(case, reopened, reopened_target)
    case.assertEqual(again["keys"], final["keys"])
    for actual, expected in zip(again["samples"], final["samples"], strict=True):
        case.assertAlmostEqual(actual, expected, delta=NATIVE_DELTA)


def _check_group_shape(case: NativeAcceptance, document: str, target: dict[str, str]) -> None:
    """A group shifted 3 frames later keeps its inner ease segment sample for sample."""
    keys = _by_frame(_inspect(case, document, target)["keys"])
    case.assertIn(15, keys)
    case.assertIn(27, keys)
    before = _sample(case, document, target, [15 + i / 4 for i in range(0, 49)])
    reference = case.create()
    peg = _peg(case, reference)
    case.mutate("animation.set_keys", reference, **peg, keys=_curve_keys())
    original = _sample(case, reference, peg, [12 + i / 4 for i in range(0, 49)])
    case.assertEqual(before, original)


@acceptance_case
def move_and_duplicate_refusals_change_nothing(case: NativeAcceptance) -> None:
    """Colliding, crossing, out of range and kind changing edits are refused with a reason."""
    document, target = _setup(case)
    move = "animation.move_keys"
    copy = "animation.duplicate_keys"
    _refused(case, move, document, target, "key_exists", frames=[12], frame_offset=12)
    _refused(case, move, document, target, "order_changed", frames=[12], frame_offset=13)
    _refused(case, move, document, target, "controls", frames=[12], frame_offset=10)
    _refused(case, move, document, target, "frame_out_of_range", frames=[0], frame_offset=-1)
    _refused(case, move, document, target, "value_out_of_bounds", frames=[36], value_offset=1999999)
    _refused(case, copy, document, target, "key_exists", frames=[0], frame_offset=12)
    _refused(
        case, copy, document, target, "incoming_kind_changed", frames=[12, 24], frame_offset=40
    )
    _refused(case, copy, document, target, "incoming_kind_changed", frames=[0], frame_offset=18)
    _refused(case, copy, document, target, "frame_out_of_range", frames=[0], frame_offset=-1)
    for tool, extra in ((move, {"frame_offset": 1}), (copy, {"frame_offset": 70})):
        case.failure(tool, case.write_params(document, {**target, "frames": [35], **extra}))
        case.failure(tool, case.write_params(document, {**target, "frames": [36, 36], **extra}))
        case.failure(
            tool,
            case.write_params(
                document, {**target, "channel": "path_position", "unit": "percent", "frames": [0]}
            ),
        )
        case.failure(
            tool,
            case.write_params(document, {**target, "layer_id": "x", "frames": [36], **extra}),
        )
    column_document, _level, column = case.populated_scene()
    column_target = {"object_id": column["object_id"], "channel": "x", "unit": "inch"}
    case.mutate("animation.set_keys", column_document, **column_target, keys=_curve_keys())
    case.mutate("column.update", column_document, column_id=column["column_id"], locked=True)
    for tool, extra in ((move, {"frame_offset": 1}), (copy, {"frame_offset": 70})):
        case.failure(
            tool,
            case.write_params(column_document, {**column_target, "frames": [36], **extra}),
        )


@acceptance_case
def duplicate_keys_copy_records_verbatim(case: NativeAcceptance) -> None:
    """Copy keys past the end and into a segment; existing keys and earlier samples stay."""
    document, target = _setup(case)
    before = _state(case, document, target)
    old = _by_frame(before["keys"])
    result = case.mutate(
        "animation.duplicate_keys",
        document,
        **target,
        frames=[36, 48],
        frame_offset=20,
        value_offset=7,
    )
    case.assertEqual(result["created_frames"], [56, 68])
    after = _state(case, document, target)
    new = _by_frame(after["keys"])
    case.assertEqual(len(after["keys"]), len(before["keys"]) + 2)
    for frame, key in old.items():
        case.assertEqual(new[frame], key)
    for frame in (36, 48):
        for field in ("interpolation", "step", "incoming", "outgoing", "linked_handles"):
            case.assertEqual(new[frame + 20].get(field), old[frame].get(field), field)
        case.assertAlmostEqual(new[frame + 20]["value"], old[frame]["value"] + 7, delta=1e-9)
    for frame, value in zip(DENSE, before["samples"], strict=True):
        if frame <= 48:
            case.assertEqual(after["samples"][DENSE.index(frame)], value)
    case.mutate("animation.duplicate_keys", document, **target, frames=[36], frame_offset=4)
    case.assertEqual(
        [key["frame"] for key in _inspect(case, document, target)["keys"]],
        [0, 12, 24, 36, 40, 48, 56, 68],
    )
    final = _state(case, document, target)
    reopened, reopened_target = _reopen(case, document, "duplicate-reopen")
    again = _state(case, reopened, reopened_target)
    case.assertEqual(again["keys"], final["keys"])
    for actual, expected in zip(again["samples"], final["samples"], strict=True):
        case.assertAlmostEqual(actual, expected, delta=NATIVE_DELTA)


CYCLE_FRAMES = [index / 4 for index in range(0, 4 * 200)]


def _folded(case: NativeAcceptance, reference: list[float], frame: float) -> float:
    """The value a cycled curve takes at frame, from the same keys sampled without a cycle."""
    first, last, drift = 0, 48, 100 - 0
    if frame < first:
        return reference[0]
    folds = int((frame - first) // (last - first))
    return reference[int((frame - folds * (last - first)) * 4)] + folds * drift


def _cycled_scene(case: NativeAcceptance) -> tuple[str, dict[str, str], list[float]]:
    """A scene whose x curve is cycled, and the same curve sampled without a cycle."""
    document, target = _setup(case)
    reference = _sample(case, document, target, [index / 4 for index in range(0, 4 * 48 + 1)])
    result = case.mutate("animation.set_cycle", document, **target, enabled=True)
    case.assertTrue(result["cycle"])
    return document, target, reference


@acceptance_case
def cyclic_curves_sample_edit_and_reopen(case: NativeAcceptance) -> None:
    """Cycle a curve; samples repeat, edits keep the cycle, a saved copy reopens identical."""
    document, target, reference = _cycled_scene(case)
    descriptor = case.success(
        "animation.channels", {"document_id": document, "object_id": target["object_id"]}
    )
    channels = {entry["channel"]: entry for entry in descriptor["channels"]}
    case.assertTrue(channels["x"]["cycle"])
    case.assertTrue(channels["x"]["cycle_supported"])
    case.assertTrue(channels["x"]["writable"])
    case.assertFalse(channels["y"]["cycle"])
    case.assertFalse(channels["path_position"]["cycle_supported"])
    samples = _sample(case, document, target, CYCLE_FRAMES)
    for frame, value in zip(CYCLE_FRAMES, samples, strict=True):
        case.assertAlmostEqual(value, _folded(case, reference, frame), delta=1e-9, msg=str(frame))
    case.assertTrue(_inspect(case, document, target)["cycle"])
    before = samples
    case.mutate("animation.insert_breakdown", document, **target, frame=40, favour_percent=50)
    case.mutate("animation.move_keys", document, **target, frames=[24], frame_offset=2)
    case.mutate("animation.duplicate_keys", document, **target, frames=[36], frame_offset=2)
    state = _inspect(case, document, target)
    case.assertTrue(state["cycle"])
    after = _sample(case, document, target, CYCLE_FRAMES)
    for frame, old, new in zip(CYCLE_FRAMES, before, after, strict=True):
        if frame % 48 < 12:
            case.assertEqual(new, old, f"cycled segment before frame 12 changed at {frame}")
    reopened, reopened_target = _reopen(case, document, "cycle-reopen")
    case.assertTrue(_inspect(case, reopened, reopened_target)["cycle"])
    again = _sample(case, reopened, reopened_target, CYCLE_FRAMES)
    for actual, expected in zip(again, after, strict=True):
        case.assertAlmostEqual(actual, expected, delta=NATIVE_DELTA)
    case.mutate("animation.set_cycle", document, **target, enabled=False)
    case.assertFalse(_inspect(case, document, target)["cycle"])
    plain = _sample(case, document, target, CYCLE_FRAMES)
    case.assertEqual(plain[-1], plain[4 * 60])


@acceptance_case
def cycle_requests_refusals_and_copies(case: NativeAcceptance) -> None:
    """Idempotent set_cycle, whole-curve copy and retime carry the cycle, sampling is bounded."""
    document, target, _reference = _cycled_scene(case)
    revision = case.revisions[document]
    repeat = case.mutate("animation.set_cycle", document, **target, enabled=True)
    case.assertEqual(repeat["revision"], revision)
    case.failure(
        "animation.set_cycle",
        case.write_params(
            document, {**target, "channel": "path_position", "unit": "percent", "enabled": True}
        ),
    )
    case.failure(
        "animation.set_cycle",
        case.write_params(document, {**target, "layer_id": "x", "enabled": True}),
    )
    case.failure("animation.set_cycle", case.write_params(document, {**target, "enabled": 1}))
    other = case.mutate("object.create", document, kind="pegbar", name="Copy")
    destination = _target(other["object_id"])
    copied = case.mutate(
        "animation.copy_curve",
        document,
        source=target,
        destination=destination,
        destination_policy="replace_all",
    )
    case.assertTrue(copied["cycle"])
    case.assertEqual(
        _inspect(case, document, destination)["keys"], _inspect(case, document, target)["keys"]
    )
    retimed = case.mutate(
        "animation.retime_curve",
        document,
        **destination,
        origin_frame=0,
        shift_frames=3,
        scale={"numerator": 2, "denominator": 1},
        step_policy="scale_exact",
    )
    case.assertTrue(retimed["cycle"])
    shifted = _sample(case, document, destination, [3 + 2 * f for f in range(0, 150)])
    original = _sample(case, document, target, list(range(0, 150)))
    for actual, expected in zip(shifted, original, strict=True):
        case.assertAlmostEqual(actual, expected, delta=1e-9)
    case.mutate("animation.delete_keys", document, **destination, frames=[48 * 2 + 3])
    case.assertTrue(_inspect(case, document, destination)["cycle"])
    fast = case.mutate("object.create", document, kind="pegbar", name="Fast")
    fast_target = _target(fast["object_id"])
    case.mutate("animation.set_keys", document, **fast_target, keys=[_simple(0, 0), _simple(1, 1)])
    case.mutate("animation.set_cycle", document, **fast_target, enabled=True)
    case.success("animation.sample", {"document_id": document, **fast_target, "frames": [1000000]})
    error = case.failure(
        "animation.sample",
        {"document_id": document, **fast_target, "frames": [1000000, 999999, 999998]},
    )
    case.assertEqual(error["code"], "resource_limit")


def _descriptor(descriptors: dict[str, Any], channel: str) -> dict[str, Any]:
    return next(item for item in descriptors["channels"] if item["channel"] == channel)


@acceptance_case
def fx_remap_and_offset_layer_targets_are_described(case: NativeAcceptance) -> None:
    """Descriptors name every target and its tool; effect and remap curves read and sample."""
    document, _level, column = case.populated_scene()
    effect = case.mutate("fx.create", document, type="colorCardFx", name="Card", frame_count=5)[
        "fx_id"
    ]
    for frame, color in ((0, [255, 0, 0, 255]), (4, [0, 0, 255, 255])):
        case.mutate(
            "fx.parameter.set_key",
            document,
            fx_id=effect,
            parameter="color",
            frame=frame,
            value=color,
        )
    described = case.success("animation.channels", {"document_id": document, "fx_id": effect})
    case.assertNotIn("object_id", described)
    names = {item["channel"] for item in described["channels"]}
    case.assertTrue({"color.r", "color.g", "color.b", "color.m"} <= names, names)
    red = _descriptor(described, "color.r")
    case.assertEqual(red["kind"], "fx_parameter")
    case.assertEqual(red["edit_via"], "fx.parameter.set_key")
    case.assertFalse(red["writable"])
    case.assertEqual(red["key_count"], 2)
    case.assertEqual(red["target"]["fx_id"], effect)
    channel = {"channel": "color.r", "unit": red["unit"]}
    keys = case.success("animation.keys", {"document_id": document, "fx_id": effect, **channel})
    case.assertEqual(keys["value_space"], "fx_parameter")
    case.assertEqual([key["frame"] for key in keys["keys"]], [0, 4])
    sampled = case.success(
        "animation.sample",
        {"document_id": document, "fx_id": effect, **channel, "frames": [0, 1, 2.5, 4]},
    )
    shown = case.success("fx.parameter.get", {"document_id": document, "fx_id": effect, "frame": 0})
    color = next(item for item in shown["parameters"] if item["name"] == "color")["value"]
    case.assertAlmostEqual(sampled["values"][0]["value"] * 255, color[0], delta=0.5)
    case.assertAlmostEqual(sampled["values"][3]["value"], 0, delta=1e-9)
    case.failure(
        "animation.set_keys",
        case.write_params(document, {"fx_id": effect, **channel, "keys": [_simple(0, 0)]}),
    )
    remap = case.success(
        "animation.channels", {"document_id": document, "column_id": column["column_id"]}
    )
    entry = _descriptor(remap, "time_remap")
    case.assertEqual(entry["kind"], "time_remap")
    case.assertIsNone(entry["edit_via"])
    case.assertFalse(entry["writable"])
    case.assertFalse(remap["remap_enabled"])
    request = {
        "document_id": document,
        "column_id": column["column_id"],
        "channel": "time_remap",
        "unit": "frame_number",
    }
    case.assertEqual(case.success("animation.keys", request)["keys"], [])
    case.assertEqual(
        len(case.success("animation.sample", {**request, "frames": [0, 1]})["values"]), 2
    )
    for params in (
        {
            "document_id": document,
            "column_id": column["column_id"],
            "object_id": column["object_id"],
        },
        {"document_id": document, "fx_id": effect, "column_id": column["column_id"]},
        {"document_id": document, "column_id": "column-404"},
        {"document_id": document},
    ):
        case.failure("animation.channels", params)
    case.failure("animation.keys", {**request, "unit": "inch"})
    case.failure("animation.keys", {**request, "channel": "x"})
    peg = _peg(case, document)
    layer = case.mutate("offset_layer.create", document, object_id=peg["object_id"], name="Layer")
    layer_id = layer["layer"]["layer_id"]
    stage = case.success("animation.channels", {"document_id": document, **peg})
    case.assertEqual(_descriptor(stage, "x")["kind"], "stage_channel")
    case.assertEqual(_descriptor(stage, "x")["target"], peg)
    layered = case.success(
        "animation.channels",
        {"document_id": document, "object_id": peg["object_id"], "layer_id": layer_id},
    )
    weight = _descriptor(layered, "weight")
    case.assertEqual(weight["kind"], "offset_layer_weight")
    case.assertEqual(weight["target"]["layer_id"], layer_id)
    case.assertEqual(_descriptor(layered, "rotation")["kind"], "offset_layer_channel")
    case.assertEqual(_descriptor(layered, "rotation")["edit_via"], "animation.set_keys")
