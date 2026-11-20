"""Exercise passive component metadata through the actual native worker."""

from __future__ import annotations

from typing import TYPE_CHECKING

from mcp_case import acceptance_case

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


@acceptance_case
def passive_inspection_preserves_state(case: NativeAcceptance) -> None:
    """Inspect selected components without changing documents or claiming readiness.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Passive assistance")
    before = case.success("scene.summary", {"document_id": document})
    result = case.success("assistance.inspect", {})
    case.assertEqual(result["format"], 1)
    case.assertEqual(
        [item["component"] for item in result["components"]], ["cadmium", "neural_style"]
    )
    for item in result["components"]:
        selected = case.success("assistance.inspect", {"component": item["component"]})
        case.assertEqual(selected["components"], [item])
        case.assertEqual(item["verification"], "not_requested")
        case.assertEqual(item["runtime"], "not_observed")
        case.assertEqual(item["qualification"], "not_performed")
        case.assertIs(item["inference_available"], False)
        case.assertEqual(item["package_source"], "app_adjacent")
        case.assertEqual(item["configuration"], "not_observed")
        case.assertEqual(len(item["reasons"]), 1)
    for params in (
        {"component": "other"},
        {"component": ""},
        {"component": None},
        {"component": 0},
        {"path": "/not-a-component-input"},
        {"operation_id": "not-a-write"},
    ):
        case.schema_failure("assistance.inspect", params)
    case.assertEqual(case.success("scene.summary", {"document_id": document}), before)
