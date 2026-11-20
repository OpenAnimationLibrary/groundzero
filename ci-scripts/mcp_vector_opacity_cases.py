"""Exercise native vector selection and independent ink opacity through workers."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_text_cases import _pixels

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

DRAWING = {"number": 1, "suffix": ""}
PROFILE = [
    {"position": 0, "opacity": 0.2},
    {"position": 0.12345678901234567, "opacity": 0.7},
    {"position": 0.5, "opacity": 0.9},
    {"position": 0.5, "opacity": 0.1},
    {"position": 1, "opacity": 0.8},
]


def _fixture(case: NativeAcceptance) -> tuple[str, dict[str, Any]]:
    document = case.mutate("scene.create", None, name="Stroke opacity")["document_id"]
    level = case.mutate("level.create", document, name="Ink", format="vector")
    target = {"level_id": level["level_id"], "drawing_id": DRAWING}
    case.mutate("drawing.create", document, **target, format="vector")
    case.mutate(
        "vector.edit",
        document,
        **target,
        coordinate_space="drawing_native",
        sharing="all_exposures",
        edits=[
            {
                "action": "add",
                "style_id": 1,
                "closed": False,
                "points": [
                    {"x": -20, "y": 0, "thickness": 3},
                    {"x": 0, "y": 0, "thickness": 3},
                    {"x": 20, "y": 0, "thickness": 3},
                ],
            }
        ],
    )
    column = case.mutate("column.create", document, name="Ink exposure")
    case.mutate(
        "column.expose",
        document,
        **target,
        column_id=column["column_id"],
        start_frame=0,
        length=1,
    )
    return document, target


def _inspect(case: NativeAcceptance, document: str, target: dict[str, Any]) -> dict[str, Any]:
    return case.success(
        "vector.opacity.inspect",
        {
            "document_id": document,
            "expected_revision": case.revisions[document],
            **target,
            "stroke_index": 0,
            "offset": 0,
            "limit": 128,
        },
    )


@acceptance_case
def vector_opacity_round_trip(case: NativeAcceptance) -> None:
    """Verify receipts, scalar/profile independence and source-discard PLI pixels.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, target = _fixture(case)
    pixels_before = _pixels(case, document)
    write = {**target, "sharing": "all_exposures", "stroke_indices": [0]}
    params = case.write_params(document, {**write, "opacity_per_mille": 327})
    result = case.success("vector.opacity.set", params)
    case.revisions[document] = result["revision"]
    case.assertEqual(result["geometry_revision"], result["revision"])
    case.assertEqual(case.success("vector.opacity.set", params), result)
    case.mutate("vector.opacity.profile.set", document, **write, knots=PROFILE)
    inspected = _inspect(case, document, target)
    case.assertEqual(inspected["opacity_per_mille"], 327)
    case.assertEqual(inspected["knots"], PROFILE)
    revision = case.revisions[document]
    noop = case.mutate("vector.opacity.profile.set", document, **write, knots=PROFILE)
    case.assertFalse(noop["changed"])
    case.assertEqual(case.revisions[document], revision)
    pixels = _pixels(case, document)
    case.assertNotEqual(pixels_before, pixels)
    case.assertTrue(any(pixel[3] for pixel in pixels))
    saved = case.mutate(
        "scene.save_copy", document, directory=str(case.workspace / "opacity-package")
    )
    manifest = json.loads((case.workspace / "opacity-package" / "manifest.json").read_text())
    case.assertRegex(manifest["levels"][0]["stroke_opacity_sha256"], "^[0-9a-f]{64}$")
    case.mutate("scene.discard", document)
    opened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    summary = case.success("scene.summary", {"document_id": opened})
    target["level_id"] = summary["levels"][0]["level_id"]
    case.assertEqual(_inspect(case, opened, target)["knots"], PROFILE)
    case.assertEqual(_pixels(case, opened), pixels)
    case.mutate(
        "vector.opacity.profile.set",
        opened,
        **target,
        sharing="all_exposures",
        stroke_indices=[0],
        knots=[],
    )
    case.assertEqual(_inspect(case, opened, target)["opacity_per_mille"], 327)


@acceptance_case
def vector_selection_and_rejection_purity(case: NativeAcceptance) -> None:
    """Verify revision-bound selection and failure purity on the actual worker.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, target = _fixture(case)
    query = {
        "document_id": document,
        "expected_revision": case.revisions[document],
        **target,
        "coordinate_space": "drawing_native",
        "query": {"mode": "nearest_centerline", "x": 0, "y": 1, "max_distance": 1},
    }
    selected = case.success("vector.select", query)
    case.assertEqual(selected["stroke_indices"], [0])
    case.assertEqual(selected["nearest"]["distance"], 1)
    before = _inspect(case, document, target)
    write = {**target, "sharing": "all_exposures", "stroke_indices": [0]}
    for knots in (
        [{"position": 0, "opacity": 1}],
        PROFILE + [PROFILE[-1], PROFILE[-1]],
    ):
        response = case.worker.call(
            "vector.opacity.profile.set",
            case.write_params(document, {**write, "knots": knots}),
        )
        case.assertEqual(response["error"]["code"], "invalid_params", response)
        case.assertEqual(_inspect(case, document, target), before)
    case.mutate("vector.opacity.set", document, **write, opacity_per_mille=100)
    response = case.worker.call("vector.select", query)
    case.assertEqual(response["error"]["code"], "revision_conflict", response)


def _segment(x0: float, x1: float, y: float, thickness: float, style: int = 1) -> dict[str, Any]:
    middle = (x0 + x1) / 2
    return {
        "action": "add",
        "style_id": style,
        "closed": False,
        "points": [
            {"x": x0, "y": y, "thickness": thickness},
            {"x": middle, "y": y, "thickness": thickness},
            {"x": x1, "y": y, "thickness": thickness},
        ],
    }


def _drawing(case: NativeAcceptance, strokes: list[dict[str, Any]]) -> tuple[str, dict[str, Any]]:
    """A document whose one vector drawing holds the strokes in stacking order."""
    document = case.mutate("scene.create", None, name="Inspection")["document_id"]
    level = case.mutate("level.create", document, name="Ink", format="vector")
    target = {"level_id": level["level_id"], "drawing_id": DRAWING}
    style = case.mutate(
        "palette.create_style",
        document,
        level_id=level["level_id"],
        name="Second",
        color={"r": 200, "g": 30, "b": 30, "a": 255},
    )
    case.assertEqual(style["style_id"], 2)
    case.mutate("drawing.create", document, **target, format="vector")
    for start in range(0, len(strokes), 128):
        case.mutate(
            "vector.edit",
            document,
            **target,
            coordinate_space="drawing_native",
            sharing="all_exposures",
            edits=strokes[start : start + 128],
        )
    return document, target


def _page(case: NativeAcceptance, document: str, target: dict[str, Any], **page: Any) -> Any:
    return case.success("vector.inspect", {"document_id": document, **target, **page})


@acceptance_case
def vector_roles_and_style_summary(case: NativeAcceptance) -> None:
    """Roles equal the native art-layer rule and the summary covers the whole drawing.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    strokes = [
        _segment(-20, 20, 0, 2, style=1),
        _segment(-20, 20, 10, 0, style=2),
        _segment(-20, 20, 20, 2, style=1),
        _segment(-20, 20, 30, 0, style=2),
        _segment(-20, 20, 40, 2, style=1),
    ]
    document, target = _drawing(case, strokes)
    revision = case.revisions[document]
    first = _page(case, document, target, offset=0, limit=2, expected_revision=revision)
    last = _page(case, document, target, offset=2, limit=128)
    roles = ["underlay", "colour_art", "line_art", "colour_art", "overlay"]
    found = [stroke["role"] for stroke in first["strokes"] + last["strokes"]]
    case.assertEqual(found, roles)
    case.assertEqual(
        [stroke["zero_thickness"] for stroke in first["strokes"] + last["strokes"]],
        [False, True, False, True, False],
    )
    case.assertEqual(first["next_offset"], 2)
    case.assertIsNone(last["next_offset"])
    for stroke in first["strokes"] + last["strokes"]:
        case.assertEqual(stroke["opacity_per_mille"], 1000)
        case.assertEqual(stroke["opacity_knots"], 0)
    counts = {"line_art": 1, "colour_art": 2, "underlay": 1, "overlay": 1}
    summary = first["summary"]
    case.assertEqual(summary, last["summary"])
    case.assertEqual(summary["roles"], counts)
    case.assertEqual(summary["colour_range"], {"first": 1, "last": 3, "layered": True})
    case.assertEqual(summary["regions"], "not_computed")
    case.assertEqual(
        summary["style_usage"],
        [
            {"style_id": 1, "strokes": 3, "defined": True},
            {"style_id": 2, "strokes": 2, "defined": True},
        ],
    )
    case.assertEqual((first["revision"], first["geometry_revision"]), (revision, revision))
    case.assertEqual(case.revisions[document], revision)
    case.mutate(
        "vector.opacity.set",
        document,
        **target,
        sharing="all_exposures",
        stroke_indices=[2],
        opacity_per_mille=400,
    )
    changed = _page(case, document, target, offset=2, limit=1)
    case.assertEqual(changed["strokes"][0]["opacity_per_mille"], 400)
    case.assertEqual(changed["revision"], case.revisions[document])
    stale = case.failure(
        "vector.inspect", {"document_id": document, **target, "expected_revision": revision}
    )
    case.assertEqual(stale["code"], "stale_revision", stale)
    case.assertEqual(stale["details"]["revision"], case.revisions[document])


@acceptance_case
def vector_roles_without_layering(case: NativeAcceptance) -> None:
    """Colour Art below or without Line Art in between makes no Underlay or Overlay.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    plain = [_segment(-20, 20, 0, 2), _segment(-20, 20, 10, 2)]
    document, target = _drawing(case, plain)
    summary = _page(case, document, target)["summary"]
    case.assertEqual(summary["roles"]["line_art"], 2)
    case.assertEqual(summary["colour_range"], {"first": None, "last": None, "layered": False})
    block = [_segment(-20, 20, 0, 0), _segment(-20, 20, 10, 0), _segment(-20, 20, 20, 2)]
    document, target = _drawing(case, block)
    page = _page(case, document, target)
    case.assertEqual(
        [stroke["role"] for stroke in page["strokes"]], ["colour_art"] * 2 + ["line_art"]
    )
    case.assertEqual(page["summary"]["colour_range"], {"first": 0, "last": 1, "layered": False})


SQUARE = [
    {"x": -50, "y": -50},
    {"x": 100, "y": -50},
    {"x": 100, "y": 50},
    {"x": -50, "y": 50},
]


def _select(case: NativeAcceptance, document: str, target: dict[str, Any], **extra: Any) -> Any:
    return case.success(
        "vector.select",
        {
            "document_id": document,
            "expected_revision": case.revisions[document],
            **target,
            "coordinate_space": "drawing_native",
            **extra,
        },
    )


def _lasso(relation: str, points: list[dict[str, Any]]) -> dict[str, Any]:
    return {"query": {"mode": "lasso", "relation": relation, "points": points}}


@acceptance_case
def vector_lasso_and_selection_handles(case: NativeAcceptance) -> None:
    """Lasso relations follow the editor; handles bind to a revision and are bounded.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    strokes = [
        _segment(-20, 20, 0, 2),
        _segment(80, 120, 0, 2),
        _segment(300, 340, 300, 2),
        _segment(20, 40, 40, 20),
        _segment(-20, 20, -10, 0),
    ]
    document, target = _drawing(case, strokes)
    contained = _select(case, document, target, **_lasso("contained", SQUARE))
    case.assertEqual(contained["stroke_indices"], [0, 4])
    crossing = _select(case, document, target, **_lasso("intersects", SQUARE))
    case.assertEqual(crossing["stroke_indices"], [0, 1, 4])
    rule = contained["rule"]
    case.assertEqual(
        (rule["mode"], rule["relation"], rule["geometry"], rule["roles"]),
        ("lasso", "contained", "lasso_region", None),
    )
    case.assertEqual(
        contained["expires"], {"on_revision_change": True, "retained_per_document": 256}
    )
    case.assertRegex(contained["selection_id"], "^selection_[0-9a-f-]{36}$")
    bounds = {"mode": "centerline_bounds", "relation": "contained"}
    box = {"x0": -50, "y0": -50, "x1": 100, "y1": 50}
    by_box = _select(case, document, target, query={**bounds, **box})
    case.assertEqual(by_box["stroke_indices"], [0, 3, 4])
    case.assertEqual(by_box["rule"]["geometry"], "centerline_bbox")
    only_colour = _select(
        case, document, target, roles=["colour_art"], **_lasso("intersects", SQUARE)
    )
    case.assertEqual(only_colour["stroke_indices"], [4])
    case.assertEqual(only_colour["rule"]["roles"], ["colour_art"])
    closed = [*SQUARE, SQUARE[0]]
    case.assertEqual(
        _select(case, document, target, **_lasso("contained", closed))["stroke_indices"],
        [0, 4],
    )
    for points, reason in (
        (
            [{"x": 0, "y": 0}, {"x": 100, "y": 100}, {"x": 100, "y": 0}, {"x": 0, "y": 100}],
            "self_intersecting",
        ),
        ([{"x": 0, "y": 0}, {"x": 50, "y": 50}, {"x": 100, "y": 100}], "degenerate"),
    ):
        error = case.failure(
            "vector.select",
            {
                "document_id": document,
                "expected_revision": case.revisions[document],
                **target,
                "coordinate_space": "drawing_native",
                **_lasso("contained", points),
            },
        )
        case.assertEqual(error["code"], "invalid_params", error)
        case.assertEqual(error["details"]["reason"], reason)
    _handle_lifetime(case, document, target, contained["selection_id"])


def _handle_lifetime(
    case: NativeAcceptance, document: str, target: dict[str, Any], handle: str
) -> None:
    write = {**target, "sharing": "all_exposures", "opacity_per_mille": 500}
    unknown = case.failure(
        "vector.opacity.set",
        case.write_params(document, {**write, "selection_id": "selection_" + "0" * 36}),
    )
    case.assertEqual(unknown["code"], "unknown_selection", unknown)
    case.mutate("vector.opacity.set", document, **write, selection_id=handle)
    stale = case.failure(
        "vector.opacity.set", case.write_params(document, {**write, "selection_id": handle})
    )
    case.assertEqual(stale["code"], "stale_selection", stale)
    case.assertEqual(stale["details"]["reason"], "revision_changed")
    case.assertEqual(stale["details"]["revision"], case.revisions[document])
    first = _select(case, document, target, query={"mode": "indices", "stroke_indices": [0]})
    for _ in range(256):
        _select(case, document, target, query={"mode": "indices", "stroke_indices": [1]})
    evicted = case.failure(
        "vector.opacity.set",
        case.write_params(document, {**write, "selection_id": first["selection_id"]}),
    )
    case.assertEqual(evicted["code"], "stale_selection", evicted)
    case.assertEqual(evicted["details"]["reason"], "evicted")


@acceptance_case
def vector_selection_is_bounded(case: NativeAcceptance) -> None:
    """More than 128 selected strokes refuse the whole selection; nothing is returned.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    strokes = [_segment(i * 4, i * 4 + 2, 0, 2) for i in range(130)]
    document, target = _drawing(case, strokes)
    everything = [
        {"x": -10, "y": -10},
        {"x": 600, "y": -10},
        {"x": 600, "y": 10},
        {"x": -10, "y": 10},
    ]
    error = case.failure(
        "vector.select",
        {
            "document_id": document,
            "expected_revision": case.revisions[document],
            **target,
            "coordinate_space": "drawing_native",
            **_lasso("contained", everything),
        },
    )
    case.assertEqual(error["code"], "resource_limit", error)
    narrower = [everything[0], {"x": 100, "y": -10}, {"x": 100, "y": 10}, everything[3]]
    selected = _select(case, document, target, **_lasso("contained", narrower))
    case.assertEqual(len(selected["stroke_indices"]), 25)


@acceptance_case
def opacity_set_accepts_selection_handle(case: NativeAcceptance) -> None:
    """Both opacity writes take a handle in place of indices, never both.

    Args:
        case: A fresh native worker acceptance fixture.
    """
    document, target = _drawing(case, [_segment(-20, 20, 0, 2), _segment(-20, 20, 30, 2)])
    write = {**target, "sharing": "all_exposures"}
    both = case.write_params(
        document,
        {**write, "stroke_indices": [0], "selection_id": "selection_x", "opacity_per_mille": 1},
    )
    case.handler_failure("vector.opacity.set", both, "invalid_params")
    neither = case.write_params(document, {**write, "opacity_per_mille": 1})
    case.handler_failure("vector.opacity.set", neither, "invalid_params")
    profile = {**write, "knots": PROFILE}
    both_profile = {**profile, "stroke_indices": [0], "selection_id": "selection_x"}
    case.handler_failure(
        "vector.opacity.profile.set", case.write_params(document, both_profile), "invalid_params"
    )
    case.handler_failure(
        "vector.opacity.profile.set", case.write_params(document, profile), "invalid_params"
    )
    nearest = {"mode": "nearest_centerline", "x": 0, "y": 28, "max_distance": 5}
    handle = _select(case, document, target, query=nearest)
    case.assertEqual(handle["stroke_indices"], [1])
    result = case.mutate(
        "vector.opacity.set",
        document,
        **write,
        selection_id=handle["selection_id"],
        opacity_per_mille=250,
    )
    case.assertEqual(result["stroke_indices"], [1])
    scalars = [stroke["opacity_per_mille"] for stroke in _page(case, document, target)["strokes"]]
    case.assertEqual(scalars, [1000, 250])
    again = _select(case, document, target, query=nearest)
    knots = [{"position": 0, "opacity": 0.5}, {"position": 1, "opacity": 0.5}]
    profile = case.mutate(
        "vector.opacity.profile.set",
        document,
        **write,
        selection_id=again["selection_id"],
        knots=knots,
    )
    case.assertEqual(profile["stroke_indices"], [1])
    knot_counts = [stroke["opacity_knots"] for stroke in _page(case, document, target)["strokes"]]
    case.assertEqual(knot_counts, [0, 2])
    other = {"number": 2, "suffix": ""}
    case.mutate("drawing.clone", document, **target, destination_drawing_id=other)
    foreign = _select(case, document, target, query={"mode": "indices", "stroke_indices": [0]})
    elsewhere = {**write, "drawing_id": other, "selection_id": foreign["selection_id"]}
    error = case.failure(
        "vector.opacity.set",
        case.write_params(document, {**elsewhere, "opacity_per_mille": 10}),
    )
    case.assertEqual(error["code"], "stale_selection", error)
