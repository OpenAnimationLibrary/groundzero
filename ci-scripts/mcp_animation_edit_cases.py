"""Qualify breakdown, key move, key duplicate and cyclic curves against the native worker."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from mcp_animation_cases import _inspect, _peg, _reopen, _rich_keys, _sample, _simple
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
