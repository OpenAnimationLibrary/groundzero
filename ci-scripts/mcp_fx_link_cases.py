"""Linked effects share their parameters: an edit of one member changes and reports every
member, copies and unlinked members stay independent, and links survive save and reopen."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_png import Pixel, decode_png

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

RED: Pixel = (200, 40, 16, 255)
BLUE: Pixel = (24, 72, 192, 255)
GREEN: Pixel = (17, 149, 83, 255)


def _card(case: NativeAcceptance, document: str, name: str) -> dict[str, Any]:
    return case.mutate("fx.create", document, type="colorCardFx", name=name, frame_count=3)


def _color(case: NativeAcceptance, document: str, effect: str, frame: int = 0) -> list[int]:
    result = case.success(
        "fx.parameter.get", {"document_id": document, "fx_id": effect, "frame": frame}
    )
    return next(item["value"] for item in result["parameters"] if item["name"] == "color")


def _preview(case: NativeAcceptance, document: str, effect: str) -> set[Pixel]:
    output = case.success("fx.graph", {"document_id": document})["current_output_fx_id"]
    case.mutate("fx.connect", document, fx_id=output, port="source", source_fx_id=effect)
    _width, _height, pixels = decode_png(case.preview(document))
    return set(pixels)


def _cards(case: NativeAcceptance) -> tuple[str, list[str]]:
    document = case.create("Linked cards")
    cards = [_card(case, document, name)["fx_id"] for name in ("First", "Second", "Third")]
    case.mutate("fx.parameter.set", document, fx_id=cards[0], parameter="color", value=list(RED))
    return document, cards


def _link_three(case: NativeAcceptance, document: str, cards: list[str]) -> str:
    first = case.mutate("fx.link.add", document, fx_id=cards[1], linked_to_fx_id=cards[0])
    case.assertEqual(first["overwritten_parameters"], ["color"])
    case.assertEqual(first["affected_fx_ids"], [cards[1]])
    second = case.mutate("fx.link.add", document, fx_id=cards[2], linked_to_fx_id=cards[1])
    group = second["linked_group"]
    case.assertEqual(group["fx_ids"], cards)
    case.assertEqual(group["group_id"], first["linked_group"]["group_id"])
    again = case.mutate("fx.link.add", document, fx_id=cards[2], linked_to_fx_id=cards[0])
    case.assertFalse(again["changed"])
    case.assertIsNone(again["graph_delta"])
    return group["group_id"]


@acceptance_case
def fx_linked_edit_reports_every_member(case: NativeAcceptance) -> None:
    """A key set on one of three linked cards is reported for and rendered by all three; a
    duplicate and an unlinked member keep their own values; the group survives reopen."""
    document, cards = _cards(case)
    group = _link_three(case, document, cards)
    case.assertEqual([_color(case, document, card) for card in cards], [list(RED)] * 3)
    copy = case.mutate("fx.duplicate", document, fx_id=cards[0], name="Copy")["fx_id"]
    keyed = case.mutate(
        "fx.parameter.set_key",
        document,
        fx_id=cards[2],
        parameter="color",
        frame=0,
        value=list(BLUE),
    )
    case.assertEqual((keyed["affected_fx_ids"], keyed["linked_group_id"]), (cards, group))
    changed = {
        entry["fx_id"] for entry in keyed["graph_delta"]["entries"] if entry["kind"] == "node"
    }
    case.assertEqual(changed, set(cards))
    case.assertEqual(_preview(case, document, cards[0]), {BLUE})
    case.assertEqual(_color(case, document, copy), list(RED))
    removed = case.mutate("fx.link.remove", document, fx_id=cards[1])
    case.assertEqual(removed["group_before"]["fx_ids"], cards)
    case.assertEqual(removed["remaining_group"]["fx_ids"], [cards[0], cards[2]])
    case.assertEqual(_color(case, document, cards[1]), list(BLUE))
    case.mutate(
        "fx.parameter.set_key",
        document,
        fx_id=cards[0],
        parameter="color",
        frame=0,
        value=list(GREEN),
    )
    case.assertEqual(_color(case, document, cards[1]), list(BLUE))
    case.assertEqual(_color(case, document, cards[2]), list(GREEN))
    _assert_reopened(case, document)


def _assert_reopened(case: NativeAcceptance, document: str) -> None:
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "linked"))
    case.mutate("scene.discard", document)
    opened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    graph = case.success("fx.graph", {"document_id": opened})
    names = {effect["fx_id"]: effect["name"] for effect in graph["effects"]}
    groups = [sorted(names[item] for item in group["fx_ids"]) for group in graph["linked_groups"]]
    case.assertEqual(groups, [["First", "Third"]])
    first = next(item for item, name in names.items() if name == "First")
    third = next(item for item, name in names.items() if name == "Third")
    case.mutate(
        "fx.parameter.set_key", opened, fx_id=first, parameter="color", frame=0, value=list(RED)
    )
    case.assertEqual(_color(case, opened, third), list(RED))


@acceptance_case
def fx_link_lifetime_refusals(case: NativeAcceptance) -> None:
    """Links refuse what the editor could not build, a locked member stops every edit of its
    group, and deleting a member leaves the rest linked."""
    document, cards = _cards(case)
    blur = case.mutate("fx.create", document, type="STD_blurFx")["fx_id"]
    output = case.success("fx.graph", {"document_id": document})["current_output_fx_id"]
    revision = case.revisions[document]
    for params, code in (
        ({"fx_id": cards[0], "linked_to_fx_id": cards[0]}, "invalid_params"),
        ({"fx_id": blur, "linked_to_fx_id": cards[0]}, "link_incompatible"),
        ({"fx_id": output, "linked_to_fx_id": cards[0]}, "unsupported_link"),
    ):
        error = case.failure("fx.link.add", case.write_params(document, params))
        case.assertEqual(error["code"], code, error)
    case.assertEqual(case.revisions[document], revision)
    _link_three(case, document, cards)
    other = case.mutate("fx.create", document, type="colorCardFx", name="Other")["fx_id"]
    taken = case.failure(
        "fx.link.add", case.write_params(document, {"fx_id": cards[0], "linked_to_fx_id": other})
    )
    case.assertEqual(taken["code"], "already_linked")
    column = case.success("fx.graph", {"document_id": document})
    column_id = next(e["column_id"] for e in column["effects"] if e["fx_id"] == cards[1])
    case.mutate("column.update", document, column_id=column_id, locked=True)
    request = {"fx_id": cards[0], "parameter": "color", "value": list(BLUE)}
    locked = case.failure("fx.parameter.set", case.write_params(document, request))
    case.assertEqual(locked["code"], "locked")
    case.assertEqual(_color(case, document, cards[0]), list(RED))
    case.mutate("column.update", document, column_id=column_id, locked=False)
    case.mutate("fx.delete", document, fx_id=cards[2])
    graph = case.success("fx.graph", {"document_id": document})
    case.assertEqual([group["fx_ids"] for group in graph["linked_groups"]], [cards[:2]])
    stale = case.failure(
        "fx.link.remove",
        {**case.write_params(document, {"fx_id": cards[0]}), "expected_revision": revision},
    )
    case.assertEqual(stale["code"], "revision_conflict")
