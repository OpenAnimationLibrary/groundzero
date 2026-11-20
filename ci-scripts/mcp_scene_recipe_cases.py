"""Run the shot, rig and delivery recipes literally, review their previews, then reopen them."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_recipe_run import (
    RecipeRun,
    recipe_inputs,
    report_success,
    require_capabilities,
    require_deliverables,
    require_gate_blocks_skipped_review,
    run_step,
    run_steps,
)

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

Check = dict[str, Any]


def _recipe(case: NativeAcceptance, recipe_id: str, category: str) -> dict[str, Any]:
    listed = case.success("fx.recipe.list", {"recipe_id": recipe_id})["recipes"]
    case.assertEqual([recipe["id"] for recipe in listed], [recipe_id])
    recipe = listed[0]
    case.assertEqual(recipe["category"], category)
    case.assertEqual(case.success("fx.recipe.list", {"category": category})["recipes"], listed)
    case.assertTrue(recipe["available"], recipe["missing_types"])
    case.assertEqual(recipe["seeds"], [])
    case.assertEqual(recipe["units"]["frame_origin"], 0)
    for label in (item["label"] for item in recipe["deliverables"]):
        case.assertIn(label, {"editable", "approximated", "baked"})
    return recipe


def _check(case: NativeAcceptance, run: RecipeRun, check: Check) -> None:
    kind = check["kind"]
    if kind == "preview_alpha_nonempty":
        return  # the success report decodes and tests the reviewed image
    result = run.results[check["step"]]
    if kind == "samples_differ":
        values = [sample["value"] for sample in result["values"]]
        case.assertEqual(len(set(values)), len(values), values)
    elif kind == "cycle_repeats":
        first, second = (
            next(s for s in result["values"] if s["frame"] == frame) for frame in check["frames"]
        )
        case.assertAlmostEqual(first["base_value"], second["base_value"], delta=1e-9)
    elif kind == "layer_adds":
        sample = next(s for s in result["values"] if s["frame"] == check["frames"][0])
        case.assertNotEqual(sample["offset_value"], 0)
        case.assertAlmostEqual(
            sample["value"], sample["base_value"] + sample["offset_value"], delta=1e-9
        )
    elif kind == "preflight_passes":
        case.assertEqual([c for c in result["checks"] if c["status"] == "fail"], [])
    elif kind == "frame_artifacts":
        count = run.inputs["end_frame"] - run.inputs["start_frame"] + 1
        case.assertEqual(len(result["artifacts"]), count)
    else:
        case.fail(f"unknown recipe check {kind}")


def _finish(case: NativeAcceptance, run: RecipeRun) -> None:
    """Qualify a run: capabilities, deliverables, checks, the reviewed preview and its gate."""
    require_deliverables(case, run)
    for check in run.recipe["checks"]:
        _check(case, run, check)
    report_success(case, run)
    require_gate_blocks_skipped_review(case, run)


def _revise(case: NativeAcceptance, run: RecipeRun) -> dict[str, Any]:
    """Run the first control's step again with its input changed; return the new result."""
    control = run.recipe["controls"][0]
    name = control["input"]
    run.inputs[name] = run.inputs[name] * 2
    step = next(item for item in run.recipe["steps"] if item["id"] == control["step"])
    return run_step(case, run, step)


def _peg_samples(
    case: NativeAcceptance, document: str, name: str, channel: dict[str, Any], frames: list[int]
) -> list[float]:
    objects = case.success("scene.summary", {"document_id": document})["objects"]
    peg = next(item["object_id"] for item in objects if item["name"] == name)
    request = {"document_id": document, "object_id": peg, **channel, "frames": frames}
    return [sample["value"] for sample in case.success("animation.sample", request)["values"]]


def _reopen(case: NativeAcceptance, run: RecipeRun) -> str:
    package = run.results["package"]["package_directory"]
    return case.mutate("scene.open", None, path=package)["document_id"]


@acceptance_case
def recipe_dolly_shot_assembly_runs_literally(case: NativeAcceptance) -> None:
    """Build the dolly shot from scratch exactly as listed, read its preview, reopen the package."""
    recipe = _recipe(case, "dolly-shot-assembly", "shot")
    require_capabilities(case, recipe)
    given = {"artwork_path": str(case.fixture), "directory": str(case.workspace / "dolly")}
    run = run_steps(case, recipe, recipe_inputs(recipe, given))
    _finish(case, run)
    document = run.results["scene"]["document_id"]
    channel = {"channel": "x", "unit": "inch"}
    frames = [0, 12, 24]
    samples = [sample["value"] for sample in run.results["samples"]["values"]]
    case.assertEqual(samples[0], 0)
    case.assertAlmostEqual(samples[1], 0.5 * 0.25, delta=1e-9)
    case.assertAlmostEqual(samples[2], 0.5, delta=1e-9)
    reopened = _reopen(case, run)
    for actual, expected in zip(
        _peg_samples(case, reopened, "Dolly", channel, frames), samples, strict=True
    ):
        case.assertAlmostEqual(actual, expected, delta=1e-9)
    revised = _revise(case, run)
    case.assertEqual(revised["channel"], "x")
    after = _peg_samples(case, document, "Dolly", channel, frames)
    case.assertAlmostEqual(after[2], 1.0, delta=1e-9)
    case.assertEqual(after[1], samples[1])


@acceptance_case
def recipe_pendulum_rig_runs_literally(case: NativeAcceptance) -> None:
    """Rig an exposed column as listed, read its preview, and reopen the cycled swing."""
    recipe = _recipe(case, "pendulum-rig", "rig")
    require_capabilities(case, recipe)
    document, _level, column = case.populated_scene()
    given = {"document_id": document, "column_object_id": column["object_id"]}
    run = run_steps(case, recipe, recipe_inputs(recipe, given))
    _finish(case, run)
    peg = run.results["pendulum"]["object_id"]
    keys = case.success(
        "animation.keys",
        {"document_id": document, "object_id": peg, "channel": "rotation", "unit": "degree"},
    )
    case.assertTrue(keys["cycle"])
    case.assertEqual(len(keys["keys"]), 4)
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "rig"))
    reopened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    channel = {"channel": "rotation", "unit": "degree"}
    frames = [6, 12, 30]
    expected = [sample["value"] for sample in run.results["samples"]["values"]]
    for actual, wanted in zip(
        _peg_samples(case, reopened, "Pendulum", channel, frames), expected, strict=True
    ):
        case.assertAlmostEqual(actual, wanted, delta=1e-9)
    revised = _revise(case, run)
    case.assertTrue(revised["cycle"])
    case.assertNotEqual(
        _peg_samples(case, document, "Pendulum", channel, [12]), [expected[1]], "revision unseen"
    )


@acceptance_case
def recipe_frame_sequence_delivery_runs_literally(case: NativeAcceptance) -> None:
    """Deliver a populated scene as listed: package, reviewed preview, rendered PNG frames."""
    recipe = _recipe(case, "frame-sequence-delivery", "delivery")
    require_capabilities(case, recipe)
    case.assertTrue(recipe["limits"], "delivery must say what it does not publish")
    document, _level, _column = case.populated_scene()
    given = {"document_id": document, "directory": str(case.workspace / "delivery")}
    run = run_steps(case, recipe, recipe_inputs(recipe, given))
    _finish(case, run)
    frames = run.results["sequence_result"]["artifacts"]
    case.assertEqual(len(frames), 4)
    reopened = _reopen(case, run)
    summary = case.success("scene.summary", {"document_id": reopened})
    case.assertEqual(len(summary["columns"]), 1)
    case.assertEqual(run.recipe["controls"], [])
