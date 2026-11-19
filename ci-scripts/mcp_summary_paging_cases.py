"""Qualify the paging of scene.summary on editable and read-only documents."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_scene_access_cases import corpus, open_scene
from mcp_studio_palette_cases import call

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

LISTS = ("columns", "levels", "objects")


def _summary(case: NativeAcceptance, document: str, **params: Any) -> dict[str, Any]:
    return call(case, case.worker, "scene.summary", {"document_id": document, **params})


def _walk(case: NativeAcceptance, document: str, limit: int) -> dict[str, list[Any]]:
    """Follow every list's next_offset until each is null, one call per step."""
    collected: dict[str, list[Any]] = {name: [] for name in LISTS}
    offsets: dict[str, int | None] = {name: 0 for name in LISTS}
    for _step in range(64):
        params = {f"{name}_offset": offset or 0 for name, offset in offsets.items()}
        page = _summary(case, document, limit=limit, **params)
        case.assertEqual(page["limit"], limit)
        for name in LISTS:
            if offsets[name] is None:
                continue
            case.assertLessEqual(len(page[name]), limit)
            collected[name].extend(page[name])
            offsets[name] = page["pages"][name]["next_offset"]
        if all(offset is None for offset in offsets.values()):
            return collected
    case.fail("scene.summary pages did not end")


@acceptance_case
def scene_summary_pages_every_list(case: NativeAcceptance) -> None:
    """Small pages of columns, levels and objects join to the whole summary, the last
    page has a null next_offset and an offset past a list is refused."""
    document = case.create("Paged")
    for index in range(5):
        case.mutate("level.create", document, name=f"L{index}", format="vector")
        case.mutate("column.create", document, name=f"C{index}")
        case.mutate("object.create", document, name=f"P{index}", kind="pegbar")
    whole = _summary(case, document, limit=1024)
    for name in LISTS:
        case.assertIsNone(whole["pages"][name]["next_offset"])
        case.assertEqual(whole["pages"][name]["total"], len(whole[name]))
    case.assertEqual(_walk(case, document, 2), {name: whole[name] for name in LISTS})
    default = _summary(case, document)
    case.assertEqual(default["limit"], 256)
    case.assertEqual(default["columns"], whole["columns"])
    response = case.worker.call("scene.summary", {"document_id": document, "columns_offset": 99})
    case.assertEqual(response["error"]["code"], "invalid_params", response)


@acceptance_case
def scene_summary_pages_read_only_documents(case: NativeAcceptance) -> None:
    """A read-only document's summary pages in its scene host the same way."""
    scenes = corpus(case)
    document = open_scene(case, case.worker, scenes["B"], "standalone")["document_id"]
    whole = _summary(case, document, limit=1024)
    case.assertEqual(_walk(case, document, 1), {name: whole[name] for name in LISTS})
