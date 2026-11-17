"""Exercise native vector selection and independent ink opacity through workers."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING, Any

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
