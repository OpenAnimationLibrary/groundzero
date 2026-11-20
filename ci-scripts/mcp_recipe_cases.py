"""Run every FX recipe's steps literally, check its declared checks, revise a control, reopen."""

from __future__ import annotations

from collections.abc import Callable
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_fx_authoring_cases import _artwork_fixture, _close_worker, _graph, _pixels, _reopen
from mcp_recipe_run import (
    RecipeRun,
    recipe_inputs,
    require_capabilities,
    require_deliverables,
    run_steps,
)

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

Fixture = Callable[["NativeAcceptance"], dict[str, Any]]


def _run_steps(case: NativeAcceptance, recipe: dict[str, Any], inputs: dict[str, Any]) -> RecipeRun:
    return run_steps(case, recipe, inputs)


def _check(
    case: NativeAcceptance, document: str, check: dict[str, Any], created: set[str]
) -> dict[int, Any]:
    kind = check["kind"]
    if kind == "diagnostics_clean":
        for node in _graph(case, document)["effects"]:
            if node["fx_id"] in created:
                case.assertEqual(node["diagnostics"], [], node["type"])
        return {}
    rendered = {frame: _pixels(case, document, frame) for frame in check["frames"]}
    if kind == "render_alpha_nonempty":
        for frame, pixels in rendered.items():
            case.assertTrue(any(pixel[3] for pixel in pixels), frame)
    distinct = {tuple(pixels) for pixels in rendered.values()}
    if kind == "frames_differ":
        case.assertEqual(len(distinct), len(rendered))
    if kind == "frames_equal":
        case.assertEqual(len(distinct), 1)
    return rendered if kind == "reopen_pixels_equal" else {}


def _revised(schema: dict[str, Any], current: Any) -> Any:
    if schema["type"] == "array":
        return [255 - channel for channel in current[:3]] + [current[3]]
    middle = (schema["minimum"] + schema["maximum"]) / 2
    target = schema["minimum"] if current > middle else schema["maximum"]
    return round((current + target) / 2, 3)


def _revise(
    case: NativeAcceptance, document: str, recipe: dict[str, Any], results: dict[str, Any]
) -> None:
    control = recipe["controls"][0]
    effect = results[control["step"]]["fx_id"]
    params = {"document_id": document, "fx_id": effect, "frame": 0}
    entries = case.success("fx.parameter.get", params)["parameters"]
    entry = next(item for item in entries if item["name"] == control["parameter"])
    value = _revised(entry["schema"], entry["value"])
    method = "fx.parameter.set_key" if entry["keyframes"] else "fx.parameter.set"
    frame = {"frame": int(entry["keyframes"][0])} if entry["keyframes"] else {}
    case.mutate(
        method, document, fx_id=effect, parameter=control["parameter"], value=value, **frame
    )


def run_recipe_literally(case: NativeAcceptance, recipe_id: str, fixture: Fixture) -> None:
    """Run one recipe's steps with the existing tools, then verify, revise and reopen it.

    Args:
        case: A fresh native worker acceptance fixture.
        recipe_id: The recipe listed by fx.recipe.list.
        fixture: Builds the document and returns the recipe's required inputs.
    """
    listed = case.success("fx.recipe.list", {"recipe_id": recipe_id})["recipes"]
    case.assertEqual(len(listed), 1)
    recipe = listed[0]
    case.assertTrue(recipe["available"], recipe["missing_types"])
    case.assertEqual(recipe["review"], {"kind": "checks"})
    require_capabilities(case, recipe)
    inputs = recipe_inputs(recipe, fixture(case))
    document = inputs["document_id"]
    run = _run_steps(case, recipe, inputs)
    results = run.results
    require_deliverables(case, run)
    created = {result["fx_id"] for result in results.values() if "fx_id" in result}
    before: dict[int, Any] = {}
    for check in recipe["checks"]:
        before.update(_check(case, document, check, created))
    _revise(case, document, recipe, results)
    revised = {frame: _pixels(case, document, frame) for frame in before}
    case.assertTrue(revised != before, "Revising the first control changed no checked frame")
    restored, _names = _reopen(case, document)
    for frame, pixels in revised.items():
        same = _pixels(case, restored, frame) == pixels
        case.assertTrue(same, f"Reopened frame {frame} differs from the revised render")
    _close_worker(case)


def _generator(case: NativeAcceptance, document: str, kind: str, values: dict[str, Any]) -> str:
    effect = case.mutate("fx.create", document, type=kind, frame_count=13)["fx_id"]
    for name, value in values.items():
        case.mutate("fx.parameter.set", document, fx_id=effect, parameter=name, value=value)
    return effect


def _disk(case: NativeAcceptance, document: str) -> str:
    colors = {"color1": [240, 200, 120, 255], "color2": [0, 0, 0, 0]}
    return _generator(
        case, document, "STD_radialGradientFx", {"period": 3, "innerperiod": 3, **colors}
    )


def _card(case: NativeAcceptance, document: str, color: list[int]) -> str:
    return _generator(case, document, "colorCardFx", {"color": color})


def _light_wrap_inputs(case: NativeAcceptance) -> dict[str, Any]:
    document, _artwork = _artwork_fixture(case)
    return {
        "document_id": document,
        "foreground_fx_id": _disk(case, document),
        "background_fx_id": _card(case, document, [40, 120, 255, 255]),
    }


def _backlit_inputs(case: NativeAcceptance) -> dict[str, Any]:
    document, _artwork = _artwork_fixture(case)
    return {
        "document_id": document,
        "light_fx_id": _card(case, document, [255, 230, 160, 255]),
        "subject_fx_id": _disk(case, document),
    }


def _source_inputs(case: NativeAcceptance) -> dict[str, Any]:
    document, artwork = _artwork_fixture(case)
    return {"document_id": document, "source_fx_id": artwork}


def _dissolve_inputs(case: NativeAcceptance) -> dict[str, Any]:
    document, artwork = _artwork_fixture(case)
    return {
        "document_id": document,
        "from_fx_id": artwork,
        "to_fx_id": _card(case, document, [24, 72, 192, 255]),
    }


def _iris_inputs(case: NativeAcceptance) -> dict[str, Any]:
    document, artwork = _artwork_fixture(case)
    return {"document_id": document, "subject_fx_id": artwork}


@acceptance_case
def recipe_light_wrap_composite_runs_literally(case: NativeAcceptance) -> None:
    """Run the light-wrap composite recipe as listed and qualify its checks."""
    run_recipe_literally(case, "light-wrap-composite", _light_wrap_inputs)


@acceptance_case
def recipe_backlit_rim_light_runs_literally(case: NativeAcceptance) -> None:
    """Run the backlit rim-light recipe as listed and qualify its checks."""
    run_recipe_literally(case, "backlit-rim-light", _backlit_inputs)


@acceptance_case
def recipe_exposure_grade_runs_literally(case: NativeAcceptance) -> None:
    """Run the exposure grade recipe as listed and qualify its checks."""
    run_recipe_literally(case, "exposure-grade", _source_inputs)


@acceptance_case
def recipe_cross_dissolve_runs_literally(case: NativeAcceptance) -> None:
    """Run the cross-dissolve transition recipe as listed and qualify its checks."""
    run_recipe_literally(case, "cross-dissolve-transition", _dissolve_inputs)


@acceptance_case
def recipe_iris_wipe_runs_literally(case: NativeAcceptance) -> None:
    """Run the iris wipe transition recipe as listed and qualify its checks."""
    run_recipe_literally(case, "iris-wipe-transition", _iris_inputs)
