"""Qualify bounded production reads through the actual private native worker."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

TOOLS = ("cast.list", "cast.usage", "scene.dependencies", "scene.validate")


def _read(case: NativeAcceptance, document: str, method: str, **options: Any) -> dict[str, Any]:
    params = {"document_id": document, "expected_revision": case.revisions[document], **options}
    return case.success(method, params)


@acceptance_case
def cast_paging_preserves_native_state(case: NativeAcceptance) -> None:
    """Read stable pages and reject stale or invalid page inputs without mutation.

    Args:
        case: A fresh real-worker acceptance fixture.
    """
    document = case.create("Cast pages")
    first = case.mutate("level.create", document, name="First", format="vector")
    second = case.mutate("level.create", document, name="Second", format="vector")
    before = case.success("scene.summary", {"document_id": document})
    page = _read(case, document, "cast.list", limit=1)
    case.assertEqual(page["source_revision"], before["revision"])
    case.assertEqual(page["items"][0]["level_id"], first["level_id"])
    case.assertEqual(page["next_offset"], 1)
    following = _read(case, document, "cast.list", offset=1, limit=1)
    case.assertEqual(following["items"][0]["level_id"], second["level_id"])
    case.assertIsNone(following["next_offset"])
    case.assertEqual(_read(case, document, "cast.list", offset=2)["items"], [])
    for method in TOOLS:
        params = {"document_id": document, "expected_revision": case.revisions[document]}
        case.schema_failure(method, {**params, "unknown": True})
        case.schema_failure(method, {**params, "limit": 0})
        case.schema_failure(method, {"document_id": document})
    case.assertEqual(case.success("scene.summary", {"document_id": document}), before)
    old_revision = case.revisions[document]
    case.mutate("scene.settings", document, width_px=64)
    for method in TOOLS:
        stale = {"document_id": document, "expected_revision": old_revision, "offset": 1}
        case.assertEqual(case.failure(method, stale)["code"], "stale_revision")


@acceptance_case
def scene_usage_and_dependencies(case: NativeAcceptance) -> None:
    """Keep usage ranges, dependency coverage and basic scene checks truthful.

    Args:
        case: A fresh real-worker acceptance fixture.
    """
    document, level, column = case.populated_scene()
    unused = case.mutate("level.create", document, name="Unused", format="vector")
    before = case.success("scene.summary", {"document_id": document})
    usage = _read(case, document, "cast.usage")
    case.assertIs(usage["traversal_complete"], True)
    case.assertEqual(
        usage["items"],
        [
            {
                "level_id": level["level_id"],
                "column_id": column["column_id"],
                "column_index": 0,
                "first_frame": 0,
                "last_frame": 24,
            }
        ],
    )
    checks = _read(case, document, "scene.validate")
    case.assertEqual([item["code"] for item in checks["items"]], ["unusedLevel"])
    case.assertEqual(checks["items"][0]["level_id"], unused["level_id"])
    dependencies = _read(case, document, "scene.dependencies")
    primary = next(item for item in dependencies["items"] if item["owner_id"] == level["level_id"])
    case.assertEqual(primary["path_kind"], "sequence_pattern")
    case.assertEqual(primary["availability"], "not_checked")
    case.assertEqual(primary["authority"], "workspace")
    case.assertIn("sidecar_discovery", dependencies["excluded_checks"])
    case.assertEqual(case.success("scene.summary", {"document_id": document}), before)
    case.assertEqual(_read(case, document, "cast.usage"), usage)


@acceptance_case
def scene_cell_limit_suppresses_unused_claims(case: NativeAcceptance) -> None:
    """Stop a real long exposure at the cell budget without certifying unused cast.

    Args:
        case: A fresh real-worker acceptance fixture.
    """
    document, level, column = case.populated_scene()
    case.mutate("level.create", document, name="Possibly unused", format="vector")
    for start in range(0, 100001, 10000):
        case.mutate(
            "column.expose",
            document,
            column_id=column["column_id"],
            level_id=level["level_id"],
            drawing_id={"number": 1, "suffix": ""},
            start_frame=start,
            length=min(10000, 100001 - start),
            overwrite=True,
        )
    report = _read(case, document, "cast.usage")
    case.assertIs(report["traversal_complete"], False)
    case.assertEqual(report["scanned"]["scanned_cells"], 100000)
    case.assertIn("cell_limit", report["reasons"])
    case.assertEqual(report["items"][0]["last_frame"], 99999)
    checks = _read(case, document, "scene.validate")
    case.assertNotIn("unusedLevel", [item["code"] for item in checks["items"]])
