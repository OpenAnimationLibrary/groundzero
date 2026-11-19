"""Run a recipe from fx.recipe.list exactly as its data lists it, and gate its success report.

A recipe never runs on the worker: the client substitutes placeholders, supplies the revision
and operation id each call needs, repeats a polled step until it is terminal, and may report
success only after the recipe's review has been done. This module is that client for the
acceptance cases.
"""

from __future__ import annotations

import base64
import time
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from mcp_png import Pixel, decode_png

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

LABELS = {"editable", "approximated", "baked"}
GRANTS = {"inspect", "draft", "file", "job"}


@dataclass
class RecipeRun:
    """The steps a client has executed and their results, in order."""

    recipe: dict[str, Any]
    inputs: dict[str, Any]
    results: dict[str, Any] = field(default_factory=dict)
    executed: list[str] = field(default_factory=list)


def walk(value: Any, path: list[str]) -> Any:
    """Follow dotted field names and array indexes through a JSON value."""
    for part in path:
        value = value[int(part)] if isinstance(value, list) else value[part]
    return value


def substitute(value: Any, inputs: dict[str, Any], results: dict[str, Any]) -> Any:
    """Replace every whole-string placeholder with the JSON value it names."""
    if isinstance(value, dict):
        return {key: substitute(item, inputs, results) for key, item in value.items()}
    if isinstance(value, list):
        return [substitute(item, inputs, results) for item in value]
    if not (isinstance(value, str) and value.startswith("${") and value.endswith("}")):
        return value
    scope, _, rest = value[2:-1].partition(".")
    parts = rest.split(".")
    return inputs[parts[0]] if scope == "input" else walk(results[parts[0]], parts[1:])


def recipe_inputs(recipe: dict[str, Any], given: dict[str, Any]) -> dict[str, Any]:
    """The given inputs, with the declared default for every input not given."""
    inputs = {}
    for entry in recipe["inputs"]:
        name = entry["name"]
        inputs[name] = given[name] if name in given else entry["schema"]["default"]
    return inputs


def _call(case: NativeAcceptance, tool: dict[str, Any], arguments: dict[str, Any]) -> Any:
    """One call with the revision and operation id the tool's schema asks for."""
    arguments = dict(arguments)
    document = arguments.pop("document_id", None)
    if tool["annotations"]["readOnlyHint"]:
        if document is not None:
            arguments["document_id"] = document
            if "expected_revision" in tool["inputSchema"]["properties"]:
                arguments["expected_revision"] = case.revisions[document]
        return case.success(tool["name"], arguments)
    return case.mutate(tool["name"], document, **arguments)


def _poll(case: NativeAcceptance, tool: dict[str, Any], step: dict[str, Any], args: Any) -> Any:
    poll = step["poll"]
    for _ in range(poll["max_attempts"]):
        result = _call(case, tool, args)
        if result[poll["field"]] in poll["terminal"]:
            case.assertIn(result[poll["field"]], poll["success"], result)
            return result
        time.sleep(0.05)
    raise AssertionError(f"step {step['id']} never reached a terminal {poll['field']}")


def run_step(case: NativeAcceptance, run: RecipeRun, step: dict[str, Any]) -> Any:
    """Execute one step as listed and record its result."""
    tools = {tool["name"]: tool for tool in case.capabilities()["tools"]}
    arguments = substitute(step["arguments"], run.inputs, run.results)
    tool = tools[step["tool"]]
    result = _poll(case, tool, step, arguments) if "poll" in step else _call(case, tool, arguments)
    run.results[step["id"]] = result
    run.executed.append(step["id"])
    return result


def run_steps(case: NativeAcceptance, recipe: dict[str, Any], inputs: dict[str, Any]) -> RecipeRun:
    """Execute every step in order, with nothing added and nothing left out."""
    run = RecipeRun(recipe, inputs)
    for step in recipe["steps"]:
        run_step(case, run, step)
    return run


def require_capabilities(case: NativeAcceptance, recipe: dict[str, Any]) -> None:
    """Every capability the recipe lists exists in the live catalog and is what its steps use."""
    caps = case.capabilities()
    tools = {tool["name"]: tool for tool in caps["tools"]}
    names = {toolset["name"] for toolset in caps["toolsets"]}
    listed = recipe["required_capabilities"]
    used = {step["tool"] for step in recipe["steps"]}
    case.assertEqual(listed["tools"], sorted(used))
    case.assertTrue(used <= set(tools), sorted(used - set(tools)))
    case.assertEqual(listed["toolsets"], sorted({tools[name]["toolset"] for name in used}))
    case.assertTrue(set(listed["toolsets"]) <= names)
    case.assertEqual(
        listed["grants"], sorted({g for name in used for g in tools[name]["effects"]["grants"]})
    )
    case.assertTrue(set(listed["grants"]) <= GRANTS)
    authority = caps["authority"]
    case.assertTrue(set(listed["grants"]) <= set(authority["grants"]), authority["grants"])
    case.assertTrue(set(listed["toolsets"]) <= set(authority["domains"]), authority["domains"])


def require_deliverables(case: NativeAcceptance, run: RecipeRun) -> None:
    """Every deliverable names a result field that exists after the run, under a known label."""
    for item in run.recipe["deliverables"]:
        case.assertIn(item["label"], LABELS)
        _steps, step, *path = item["from"].split(".")
        value = walk(run.results[step], path)
        case.assertTrue(value not in (None, "", [], {}), item["from"])


def preview_pixels(case: NativeAcceptance, run: RecipeRun) -> list[Pixel]:
    """Decode the image the review's last read step returned."""
    image = run.results[run.recipe["review"]["read_steps"][-1]]["image"]
    data = base64.b64decode(image["data"], validate=True)
    width, height, pixels = decode_png(data)
    case.assertGreater(width * height, 0)
    return pixels


def report_success(case: NativeAcceptance, run: RecipeRun) -> list[Pixel]:
    """Return the reviewed preview's pixels, or fail when the review was not done.

    A client may call a preview recipe successful only after it executed every read step of
    its review and the image it read shows something.
    """
    review = run.recipe["review"]
    if review["kind"] != "preview":
        return []
    missing = [step for step in review["read_steps"] if step not in run.executed]
    case.assertEqual(missing, [], "the review read was skipped")
    pixels = preview_pixels(case, run)
    case.assertTrue(any(pixel[3] for pixel in pixels), "the reviewed preview is empty")
    return pixels


def require_gate_blocks_skipped_review(case: NativeAcceptance, run: RecipeRun) -> None:
    """Prove the success report fails for every review read left out in turn."""
    review = run.recipe["review"]
    if review["kind"] != "preview":
        return
    for skipped in review["read_steps"]:
        partial = RecipeRun(
            run.recipe,
            run.inputs,
            {key: value for key, value in run.results.items() if key != skipped},
            [step for step in run.executed if step != skipped],
        )
        with case.assertRaises(AssertionError, msg=f"skipping {skipped} was not caught"):
            report_success(case, partial)
