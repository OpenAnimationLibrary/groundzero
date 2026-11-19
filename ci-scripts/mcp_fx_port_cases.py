"""Dynamic port groups: an over of many sources composites in port order exactly as a chain of
two-input overs, keeps its order and minimum through edits, macros and reopen, and refuses a
port that the group no longer has."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_png import Pixel, decode_png

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

COLORS: tuple[Pixel, ...] = (
    (200, 40, 16, 128),
    (24, 72, 192, 128),
    (17, 149, 83, 128),
    (240, 220, 30, 255),
)


def _cards(case: NativeAcceptance, document: str) -> list[str]:
    cards = []
    for index, color in enumerate(COLORS):
        card = case.mutate("fx.create", document, type="colorCardFx", name=f"Card {index}")
        case.mutate(
            "fx.parameter.set", document, fx_id=card["fx_id"], parameter="color", value=list(color)
        )
        cards.append(card["fx_id"])
    return cards


def _render(case: NativeAcceptance, document: str, effect: str) -> list[Pixel]:
    output = case.success("fx.graph", {"document_id": document})["current_output_fx_id"]
    case.mutate("fx.connect", document, fx_id=output, port="source", source_fx_id=effect)
    _width, _height, pixels = decode_png(case.preview(document))
    return pixels


def _chain(case: NativeAcceptance, document: str, sources: list[str]) -> str:
    """Two-input overs stacked so sources[0] is on top, as one over of all of them."""
    below = sources[-1]
    for source in reversed(sources[:-1]):
        node = case.mutate("fx.create", document, type="overFx")["fx_id"]
        case.mutate("fx.connect", document, fx_id=node, port="Source1", source_fx_id=source)
        case.mutate("fx.connect", document, fx_id=node, port="Source2", source_fx_id=below)
        below = node
    return below


def _set(case: NativeAcceptance, document: str, over: str, sources: list[str]) -> dict[str, Any]:
    return case.mutate("fx.port_group.set", document, fx_id=over, group="Source", sources=sources)


def _ports(case: NativeAcceptance, document: str, over: str) -> list[tuple[str, Any]]:
    graph = case.success("fx.graph", {"document_id": document, "limit": 512})
    node = next(effect for effect in graph["effects"] if effect["fx_id"] == over)
    return [(port["name"], port.get("source_fx_id")) for port in node["ports"]]


@acceptance_case
def fx_over_group_order_and_minimum(case: NativeAcceptance) -> None:
    """Four sources render as the equivalent chain, a reorder renders as the reordered chain,
    an empty group keeps its two-port minimum, and the order survives a macro and reopen."""
    document = case.create("Over group")
    cards = _cards(case, document)
    over = case.mutate("fx.create", document, type="overFx", name="Stack")["fx_id"]
    catalog = case.success("fx.catalog", {})["effects"]
    groups = next(entry for entry in catalog if entry["type"] == "overFx")["port_groups"]
    case.assertEqual(
        groups, [{"prefix": "Source", "min_ports": 2, "max_ports": 64, "editable": True}]
    )
    grown = _set(case, document, over, cards)
    case.assertEqual(
        (grown["ports_added"], grown["ports_removed"]), (["Source3", "Source4", "Source5"], [])
    )
    case.assertEqual([p["source_fx_id"] for p in grown["ports"]], [*cards, None])
    case.assertEqual(
        _render(case, document, over), _render(case, document, _chain(case, document, cards))
    )
    order = [cards[2], cards[0], cards[3], cards[1]]
    _set(case, document, over, order)
    case.assertEqual(
        _render(case, document, over), _render(case, document, _chain(case, document, order))
    )
    revision = case.revisions[document]
    case.assertIsNone(_set(case, document, over, order)["graph_delta"])
    case.assertEqual(case.revisions[document], revision)
    empty = _set(case, document, over, [])
    case.assertEqual([p["name"] for p in empty["ports"]], ["Source1", "Source2"])
    case.assertEqual(empty["ports_removed"], ["Source5", "Source4", "Source3"])
    _set(case, document, over, order)
    blur = case.mutate("fx.create", document, type="STD_blurFx")["fx_id"]
    case.mutate("fx.connect", document, fx_id=blur, port="Source", source_fx_id=over)
    expected = _render(case, document, blur)
    macro = case.mutate("fx.macro.create", document, fx_ids=[over, blur], name="Stacked")["node"]
    case.assertEqual(_render(case, document, macro["fx_id"]), expected)
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "stack"))
    case.mutate("scene.discard", document)
    opened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    graph = case.success("fx.graph", {"document_id": opened, "limit": 512})
    reopened = next(effect for effect in graph["effects"] if "macro" in effect)
    case.assertEqual(_render(case, opened, reopened["fx_id"]), expected)
    case.mutate("fx.macro.expand", opened, fx_id=reopened["fx_id"])
    stack = next(
        effect
        for effect in case.success("fx.graph", {"document_id": opened, "limit": 512})["effects"]
        if effect["type"] == "overFx" and len(effect["ports"]) == 5
    )
    case.assertEqual([port["name"] for port in stack["ports"]], [f"Source{n}" for n in range(1, 6)])


@acceptance_case
def fx_removed_group_port_is_refused(case: NativeAcceptance) -> None:
    """After a group shrinks, its old port is UNKNOWN_PORT naming the group and the tool to
    use; a stale revision, an unknown group and a cycle are refused; nothing changes."""
    document = case.create("Shrunk group")
    cards = _cards(case, document)
    over = case.mutate("fx.create", document, type="overFx")["fx_id"]
    _set(case, document, over, cards[:3])
    stale = case.revisions[document]
    _set(case, document, over, cards[:1])
    revision = case.revisions[document]
    ports = _ports(case, document, over)
    request = {"fx_id": over, "port": "Source3", "source_fx_id": cards[0]}
    error = case.failure("fx.connect", case.write_params(document, request))
    case.assertEqual((error["code"], error["details"]["port_group"]), ("UNKNOWN_PORT", "Source"))
    old = {
        **case.write_params(document, {"fx_id": over, "group": "Source", "sources": cards}),
        "expected_revision": stale,
    }
    case.assertEqual(case.failure("fx.port_group.set", old)["code"], "revision_conflict")
    for params, code in (
        ({"fx_id": over, "group": "Light", "sources": []}, "unknown_port_group"),
        ({"fx_id": over, "group": "Source", "sources": [over]}, "GRAPH_CYCLE"),
        ({"fx_id": cards[0], "group": "Source", "sources": []}, "unknown_port_group"),
    ):
        refused = case.failure("fx.port_group.set", case.write_params(document, params))
        case.assertEqual(refused["code"], code, refused)
    case.assertEqual(case.revisions[document], revision)
    case.assertEqual(_ports(case, document, over), ports)
