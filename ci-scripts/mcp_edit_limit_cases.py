"""Qualify refusals of vector edits, vector selections and render requests."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

DRAWING = {"number": 1, "suffix": ""}
SPACE = {"coordinate_space": "drawing_native", "sharing": "all_exposures"}
SELECT_ALL = {"mode": "centerline_bounds", "x0": -10, "y0": -10, "x1": 1000, "y1": 10}


def _stroke(index: int, style: float = 1) -> dict[str, Any]:
    points = [{"x": index + step / 2, "y": 0, "thickness": 1} for step in range(3)]
    return {"action": "add", "style_id": style, "closed": False, "points": points}


def _fixture(case: NativeAcceptance) -> tuple[str, dict[str, Any]]:
    document = case.create("Vector limits")
    level = case.mutate("level.create", document, name="Ink", format="vector")
    target = {"level_id": level["level_id"], "drawing_id": DRAWING}
    case.mutate("drawing.create", document, **target, format="vector")
    return document, target


def _select(
    case: NativeAcceptance, document: str, target: dict[str, Any], query: dict[str, Any]
) -> dict[str, Any]:
    return case.worker.call(
        "vector.select",
        {
            "document_id": document,
            "expected_revision": case.revisions[document],
            **target,
            "coordinate_space": "drawing_native",
            "query": query,
        },
    )


def unknown_style_rejects_the_whole_vector_batch(case: NativeAcceptance) -> None:
    """Refuse a style the palette lacks before any stroke of the batch is applied.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document, target = _fixture(case)
    palette = case.success(
        "palette.inspect", {"document_id": document, "level_id": target["level_id"]}
    )
    styles = len(palette["styles"])
    case.mutate("vector.edit", document, **target, **SPACE, edits=[_stroke(0)])
    before = case.success("vector.inspect", {"document_id": document, **target})
    revision = case.revisions[document]
    update = {"action": "update", "index": 0, "style_id": styles, "closed": False}
    update["points"] = _stroke(0)["points"]
    for style in (styles, styles + 1, 4095, 4096, -1, 1.5):
        label = f"style {style}"
        code = "invalid_params" if style in (styles, styles + 1, 4095) else case.schema_code()
        edits = [_stroke(5), _stroke(6, style)]
        error = case.failure(
            "vector.edit", case.write_params(document, {**target, **SPACE, "edits": edits})
        )
        case.assertEqual(error["code"], code, label)
        replace = {**update, "style_id": style}
        params = case.write_params(document, {**target, **SPACE, "edits": [replace]})
        case.assertEqual(case.failure("vector.edit", params)["code"], code, label)
        after = case.success("vector.inspect", {"document_id": document, **target})
        case.assertEqual(after, before, label)
        case.assertEqual(case.revisions[document], revision, label)
    case.mutate("vector.edit", document, **target, **SPACE, edits=[_stroke(5, styles - 1)])
    case.assertEqual(
        case.success("vector.inspect", {"document_id": document, **target})["total"], 2
    )


def oversized_vector_selection_is_refused(case: NativeAcceptance) -> None:
    """Return at most 128 selected strokes and refuse a larger result, never truncating it.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document, target = _fixture(case)
    case.mutate("vector.edit", document, **target, **SPACE, edits=[_stroke(i) for i in range(128)])
    exact = _select(case, document, target, {**SELECT_ALL, "relation": "intersects"})
    case.assertEqual(exact["result"]["stroke_indices"], list(range(128)))
    case.mutate("vector.edit", document, **target, **SPACE, edits=[_stroke(200)])
    for relation in ("intersects", "contained"):
        response = _select(case, document, target, {**SELECT_ALL, "relation": relation})
        case.assertEqual(response["error"]["code"], "resource_limit", relation)
        case.assertIn("128", response["error"]["message"])
    narrow = {**SELECT_ALL, "x1": 10, "relation": "contained"}
    case.assertEqual(len(_select(case, document, target, narrow)["result"]["stroke_indices"]), 10)
    indices = {"mode": "indices", "stroke_indices": list(range(129))}
    response = _select(case, document, target, indices)
    case.assertEqual(response["error"]["code"], case.schema_code())
    indices["stroke_indices"] = list(range(128))
    case.assertEqual(len(_select(case, document, target, indices)["result"]["stroke_indices"]), 128)


def _exposed_card(case: NativeAcceptance) -> str:
    document = case.create("Render limits")
    case.mutate("fx.create", document, type="colorCardFx", frame_count=3)
    return document


def render_frame_ranges_are_refused_before_a_job_exists(case: NativeAcceptance) -> None:
    """Refuse an end before the start, a negative frame and a zero step without creating a job.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = _exposed_card(case)
    revision = case.revisions[document]
    ranges = (
        ({"start_frame": 2, "end_frame": 1}, "invalid_params"),
        ({"start_frame": 1000000, "end_frame": 0}, "invalid_params"),
        ({"start_frame": 0, "end_frame": 4, "step": 0}, case.schema_code()),
        ({"start_frame": -1, "end_frame": 1}, case.schema_code()),
        ({"start_frame": 0, "end_frame": 1000001}, case.schema_code()),
    )
    for frames, code in ranges:
        error = case.failure("render.submit", case.write_params(document, frames))
        case.assertEqual(error["code"], code, frames)
    case.assertEqual(case.revisions[document], revision)
    case.assertEqual(list(case.workspace.glob("**/renders/job_*")), [])
    job = case.mutate("render.submit", document, start_frame=1, end_frame=1)
    case.assertEqual(case.wait_job(job["job_id"])["status"], "completed")


def exact_render_retry_returns_the_same_job(case: NativeAcceptance) -> None:
    """Replay a submit receipt instead of starting a second render, even after the job ends.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = _exposed_card(case)
    request = case.write_params(
        document, {"start_frame": 0, "end_frame": 1, "width": 32, "height": 32}
    )
    first = case.success("render.submit", request)
    case.assertEqual(case.success("render.submit", request), first)
    case.assertEqual(case.wait_job(first["job_id"])["status"], "completed")
    case.revisions[document] = first["revision"]
    case.mutate("scene.settings", document, width_px=64)
    case.assertEqual(case.success("render.submit", request), first)
    case.assertEqual(
        case.failure("render.submit", {**request, "end_frame": 2})["code"], "operation_conflict"
    )
    jobs = sorted(path.name for path in case.workspace.glob("**/renders/job_*"))
    case.assertEqual(jobs, [first["job_id"]])
    other = case.mutate("render.submit", document, start_frame=0, end_frame=1, width=32, height=32)
    case.assertNotEqual(other["job_id"], first["job_id"])
