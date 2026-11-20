"""Qualify bounded production reads through the actual private native worker."""

from __future__ import annotations

import re
import shutil
from typing import TYPE_CHECKING, Any

import mcp_scene_access_cases as access
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


def _recorded(level: str, attributes: str, text: str) -> str:
    """Put recorded cast attributes on the level's path tag, as the editor writes them."""
    pattern = rf"({level} \s*<info[^>]*>\s*<path) "
    return re.sub(pattern, rf"\1 {attributes} ", text, count=1)


def _origin(item: str, relationship: int) -> str:
    fields = (
        f'{{\\"id\\":\\"{item}-id\\",\\"item\\":\\"{item}\\",\\"kind\\":\\"symbol\\",'
        f'\\"source\\":\\"$scenefolder/assets/{item}.pli\\",\\"revision\\":4,'
        f'\\"relationship\\":{relationship}}}'
    )
    return f'libraryOrigin="{fields}"'


def _cast_records(case: NativeAcceptance, document: str, revision: str) -> dict[str, Any]:
    params = {"document_id": document, "expected_revision": revision, "limit": 128}
    page = case.success("cast.list", params)
    case.assertIsNone(page["next_offset"])
    return {
        row["name"]: {key: row[key] for key in ("native_cast_id", "tags", "library", "cover")}
        for row in page["items"]
    }


@acceptance_case
def read_only_cast_rows_show_recorded_tags_and_origins_only(case: NativeAcceptance) -> None:
    """A scene file's cast tags and recorded Library origins come back exactly as stored, and
    a level that records none shows no tags and no origin."""
    package = case.inputs / "recorded"
    shutil.copytree(access.base_package(case), package)
    text = (package / "scene.tnz").read_text(encoding="utf-8")
    tags = 'castTags="[\\"hero\\",\\"caf\u00e9\\"]"'
    text = _recorded("Ink", f"{tags} {_origin('Rig', 2)}", text)
    text = _recorded("Cel", _origin("Pose", 3), text)
    (package / "scene.tnz").write_text(text, encoding="utf-8")
    opened = access.open_scene(case, case.worker, package / "scene.tnz", "standalone")
    before = _cast_records(case, opened["document_id"], "0")
    case.assertEqual(before["Ink"]["tags"], ["hero", "caf\u00e9"])
    case.assertEqual(before["Ink"]["library"]["relationship"], "local_copy")
    case.assertEqual(before["Ink"]["library"]["item_path"], "Rig")
    case.assertEqual(before["Cel"]["library"]["relationship"], "snapshot")
    case.assertEqual((before["Cel"]["tags"], before["Paint"]["tags"]), ([], []))
    case.assertIsNone(before["Paint"]["library"])
    access.discard(case, case.worker, opened["document_id"])
