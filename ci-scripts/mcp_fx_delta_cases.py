"""FX write results carry the complete graph delta of their write, and fx.graph.delta's net
delta equals an independent diff of two fx.graph reads."""

from __future__ import annotations

import random
from typing import TYPE_CHECKING, Any

import mcp_fx_authoring_cases as fx
from mcp_case import acceptance_case

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

KIND_ORDER = ("current_output", "node", "edge", "terminal", "linked_group", "group")
NODE_FIELDS = ("type", "name", "enabled", "terminal", "position", "group_ids", "linked_group_id")


def _write(case: NativeAcceptance, tool: str, document: str, **params: Any) -> dict[str, Any]:
    """Run a write and check the shape of its graph_delta against its revisions."""
    before = case.revisions[document]
    result = case.mutate(tool, document, **params)
    delta = result["graph_delta"]
    if result["revision"] == before:
        case.assertIsNone(delta, tool)
        return result
    case.assertEqual((delta["from_revision"], delta["revision"]), (before, result["revision"]))
    case.assertEqual((delta["tool"], delta["evidence_kind"]), (tool, "structural"))
    case.assertEqual(delta["total_entries"], len(delta["entries"]))
    kinds = [KIND_ORDER.index(entry["kind"]) for entry in delta["entries"]]
    case.assertEqual(kinds, sorted(kinds), tool)
    return result


def _entries(result: dict[str, Any], kind: str) -> list[dict[str, Any]]:
    return [entry for entry in result["graph_delta"]["entries"] if entry["kind"] == kind]


def _node(result: dict[str, Any], effect: str) -> dict[str, Any]:
    return next(entry for entry in _entries(result, "node") if entry["fx_id"] == effect)


def _no_op(case: NativeAcceptance, tool: str, document: str, **params: Any) -> None:
    revision = case.revisions[document]
    result = case.mutate(tool, document, **params)
    case.assertEqual(result["revision"], revision, tool)
    case.assertIsNone(result["graph_delta"], tool)


def _graph_writes(case: NativeAcceptance, document: str, blur: str, artwork: str) -> str:
    """fx.create, fx.connect, fx.disconnect, fx.terminal.set and fx.output.select."""
    created = _write(case, "fx.create", document, type="STD_glowFx", name="Glow")
    glow = created["fx_id"]
    entry = _node(created, glow)
    case.assertEqual(
        (entry["change"], entry["cause"], entry["before"]), ("created", "requested", None)
    )
    connected = _write(case, "fx.connect", document, fx_id=glow, port="Source", source_fx_id=blur)
    edge = _entries(connected, "edge")[0]
    case.assertEqual(
        (edge["fx_id"], edge["port"], edge["before"], edge["after"]), (glow, "Source", None, blur)
    )
    case.assertEqual(edge["cause"], "requested")
    _no_op(case, "fx.connect", document, fx_id=glow, port="Source", source_fx_id=blur)
    disconnected = _write(case, "fx.disconnect", document, fx_id=glow, port="Source")
    case.assertEqual(_entries(disconnected, "edge")[0]["after"], None)
    _no_op(case, "fx.disconnect", document, fx_id=glow, port="Source")
    terminal = _write(case, "fx.terminal.set", document, fx_id=glow, enabled=True)
    case.assertEqual(
        [
            (item["fx_id"], item["before"], item["after"], item["cause"])
            for item in _entries(terminal, "terminal")
        ],
        [(glow, False, True, "requested")],
    )
    case.assertIn("terminal", _node(terminal, glow)["aspects"])
    _no_op(case, "fx.terminal.set", document, fx_id=glow, enabled=True)
    output = _write(case, "fx.create", document, type="output", name="Second output")["fx_id"]
    selected = _write(case, "fx.output.select", document, fx_id=output)
    current = _entries(selected, "current_output")[0]
    case.assertEqual((current["after"], current["cause"]), (output, "requested"))
    _no_op(case, "fx.output.select", document, fx_id=output)
    return glow


def _node_writes(case: NativeAcceptance, document: str, blur: str, glow: str) -> None:
    """fx.parameter.set, set_key, rename, enabled.set and fx.duplicate."""
    changed = _write(
        case, "fx.parameter.set", document, fx_id=glow, parameter="brightness", value=50
    )
    entry = _node(changed, glow)
    case.assertEqual(
        (entry["aspects"], entry["parameters_changed"]), (["parameters"], ["brightness"])
    )
    _no_op(case, "fx.parameter.set", document, fx_id=glow, parameter="brightness", value=50)
    keyed = _write(
        case, "fx.parameter.set_key", document, fx_id=blur, parameter="value", frame=4, value=3
    )
    case.assertEqual(_node(keyed, blur)["parameters_changed"], ["value"])
    _no_op(case, "fx.parameter.set_key", document, fx_id=blur, parameter="value", frame=4, value=3)
    renamed = _write(case, "fx.rename", document, fx_id=glow, name="Renamed glow")
    entry = _node(renamed, glow)
    case.assertEqual(
        (entry["aspects"], entry["before"]["name"], entry["after"]["name"]),
        (["name"], "Glow", "Renamed glow"),
    )
    _no_op(case, "fx.rename", document, fx_id=glow, name="Renamed glow")
    disabled = _write(case, "fx.enabled.set", document, fx_id=glow, enabled=False)
    case.assertEqual(_node(disabled, glow)["aspects"], ["enabled"])
    _no_op(case, "fx.enabled.set", document, fx_id=glow, enabled=False)
    copied = _write(case, "fx.duplicate", document, fx_id=glow, name="Copy")
    case.assertEqual(_node(copied, copied["fx_id"])["change"], "created")


def _consume_only(case: NativeAcceptance, document: str, source: str, consumer: str) -> None:
    """Disconnect every input that reads source except consumer's."""
    for effect in case.success("fx.graph", {"document_id": document})["effects"]:
        for port in effect["ports"]:
            if port.get("source_fx_id") == source and effect["fx_id"] != consumer:
                case.mutate("fx.disconnect", document, fx_id=effect["fx_id"], port=port["name"])


def _preset_and_macro(case: NativeAcceptance, document: str, blur: str, glow: str) -> None:
    """fx.preset.apply, fx.macro.create, fx.macro.expand and fx.delete."""
    directory = str(case.workspace / "delta-preset")
    case.mutate("fx.preset.save", document, fx_id=glow, directory=directory)
    target = _write(case, "fx.create", document, type="STD_glowFx")["fx_id"]
    applied = _write(case, "fx.preset.apply", document, fx_id=target, directory=directory)
    case.assertIn("parameters", _node(applied, target)["aspects"])
    _no_op(case, "fx.preset.apply", document, fx_id=target, directory=directory)
    case.mutate("fx.enabled.set", document, fx_id=glow, enabled=True)
    case.mutate("fx.connect", document, fx_id=glow, port="Source", source_fx_id=blur)
    _consume_only(case, document, blur, glow)
    made = _write(case, "fx.macro.create", document, fx_ids=[blur, glow], name="Macro")
    macro = made["node"]["fx_id"]
    removed = {entry["fx_id"] for entry in _entries(made, "node") if entry["change"] == "removed"}
    case.assertEqual(removed, {blur, glow})
    case.assertEqual(_node(made, macro)["cause"], "requested")
    moved = [entry for entry in _entries(made, "terminal") if entry["cause"] == "implicit"]
    case.assertEqual(
        {(entry["fx_id"], entry["after"]) for entry in moved}, {(glow, False), (macro, True)}
    )
    expanded = _write(case, "fx.macro.expand", document, fx_id=macro)
    case.assertEqual(_node(expanded, macro)["change"], "removed")
    survivor = next(entry["fx_id"] for entry in _entries(expanded, "terminal") if entry["after"])
    deleted = _write(case, "fx.delete", document, fx_id=survivor)
    case.assertEqual(
        (_node(deleted, survivor)["change"], _node(deleted, survivor)["cause"]),
        ("removed", "requested"),
    )
    terminal = _entries(deleted, "terminal")
    case.assertEqual(
        [(item["before"], item["after"], item["cause"]) for item in terminal],
        [(True, False, "implicit")],
    )


@acceptance_case
def fx_write_results_carry_complete_deltas(case: NativeAcceptance) -> None:
    """Each of the fourteen FX writes returns its step's delta with requested and implicit
    causes; a repeated call that changes nothing keeps the revision and returns null; an
    exact retry returns the recorded delta unchanged."""
    document, artwork, blur = fx._blur_fixture(case)
    glow = _graph_writes(case, document, blur, artwork)
    _node_writes(case, document, blur, glow)
    _preset_and_macro(case, document, blur, glow)
    fresh = case.mutate("fx.create", document, type="STD_blurFx")["fx_id"]
    params = case.write_params(document, {"fx_id": fresh, "name": "Replayed"})
    first = case.success("fx.rename", params)
    case.assertEqual(case.success("fx.rename", params), first)


@acceptance_case
def fx_implicit_terminal_changes_are_reported(case: NativeAcceptance) -> None:
    """A new generator, the deletion of a terminal effect and a macro of a terminal root
    report the terminal links they change as implicit entries."""
    document = case.create("Implicit terminals")
    card = _write(case, "fx.create", document, type="colorCardFx", name="Card")
    entry = _node(card, card["fx_id"])
    case.assertEqual((entry["change"], entry["after"]["generator_wrapper"]), ("created", True))
    case.assertEqual(_entries(card, "terminal"), [])
    graph = case.success("fx.graph", {"document_id": document})
    output = graph["current_output_fx_id"]
    connected = _write(
        case, "fx.connect", document, fx_id=output, port="source", source_fx_id=card["fx_id"]
    )
    case.assertEqual(len(_entries(connected, "edge")), 1)
    blur = case.mutate("fx.create", document, type="STD_blurFx")["fx_id"]
    case.mutate("fx.connect", document, fx_id=blur, port="Source", source_fx_id=card["fx_id"])
    case.mutate("fx.terminal.set", document, fx_id=blur, enabled=True)
    deleted = _write(case, "fx.delete", document, fx_id=blur)
    case.assertEqual(
        [(item["fx_id"], item["cause"]) for item in _entries(deleted, "terminal")],
        [(blur, "implicit")],
    )
    case.assertEqual([item["cause"] for item in _entries(deleted, "edge")], ["implicit"])


def _states(case: NativeAcceptance, document: str) -> dict[str, Any]:
    """An independent view of the graph from fx.graph and fx.parameter.get."""
    graph = case.success("fx.graph", {"document_id": document})
    nodes, edges, terminals, parameters = {}, {}, set(), {}
    for effect in graph["effects"]:
        effect_id = effect["fx_id"]
        state = {field: effect.get(field) for field in NODE_FIELDS}
        state["port_names"] = [port["name"] for port in effect["ports"]]
        state["generator_wrapper"] = effect.get("generator_wrapper", False)
        state["macro"] = "macro" in effect
        nodes[effect_id] = state
        for port in effect["ports"]:
            if port.get("source_fx_id"):
                edges[(effect_id, port["name"])] = port["source_fx_id"]
        if effect["terminal"]:
            terminals.add(effect_id)
        if "macro" not in effect:
            values = case.success("fx.parameter.get", {"document_id": document, "fx_id": effect_id})
            parameters[effect_id] = {
                item["name"]: (item.get("value"), item.get("keyframes"))
                for item in values["parameters"]
            }
    groups = {item["group_id"]: (item["name"], item["member_count"]) for item in graph["groups"]}
    return {
        "output": graph["current_output_fx_id"],
        "nodes": nodes,
        "edges": edges,
        "terminals": terminals,
        "parameters": parameters,
        "groups": groups,
    }


def _expected_nodes(before: dict[str, Any], after: dict[str, Any]) -> set[tuple[Any, ...]]:
    expected: set[tuple[Any, ...]] = set()
    for node in before["nodes"].keys() | after["nodes"].keys():
        old, new = before["nodes"].get(node), after["nodes"].get(node)
        params_old = before["parameters"].get(node, {})
        params_new = after["parameters"].get(node, {})
        changed = sorted(
            name
            for name in params_old.keys() | params_new.keys()
            if params_old.get(name) != params_new.get(name)
        )
        if old != new or (old and new and changed):
            expected.add(
                ("node", node, _key(old), _key(new), tuple(changed) if old and new else ())
            )
    return expected


def _expected(before: dict[str, Any], after: dict[str, Any]) -> set[tuple[Any, ...]]:
    expected = _expected_nodes(before, after)
    if before["output"] != after["output"]:
        expected.add(("current_output", before["output"], after["output"]))
    for edge in before["edges"].keys() | after["edges"].keys():
        if before["edges"].get(edge) != after["edges"].get(edge):
            expected.add(("edge", *edge, before["edges"].get(edge), after["edges"].get(edge)))
    for node in before["terminals"] ^ after["terminals"]:
        expected.add(("terminal", node, node in before["terminals"], node in after["terminals"]))
    for group in before["groups"].keys() | after["groups"].keys():
        if before["groups"].get(group) != after["groups"].get(group):
            expected.add(("group", group, before["groups"].get(group), after["groups"].get(group)))
    return expected


def _key(state: dict[str, Any] | None) -> str | None:
    return None if state is None else repr(sorted(state.items()))


def _reported(
    case: NativeAcceptance, changes: list[dict[str, Any]], transient: set[str]
) -> set[tuple[Any, ...]]:
    reported: set[tuple[Any, ...]] = set()
    for item in changes:
        kind = item["kind"]
        if kind == "node" and item["change"] == "transient":
            transient.add(item["fx_id"])
            continue
        if kind == "current_output":
            reported.add((kind, item["before"], item["after"]))
        elif kind == "node":
            reported.add(
                (
                    kind,
                    item["fx_id"],
                    _key(item["before"]),
                    _key(item["after"]),
                    tuple(item["parameters_changed"]),
                )
            )
        elif kind == "edge":
            reported.add((kind, item["fx_id"], item["port"], item["before"], item["after"]))
        elif kind == "terminal":
            reported.add((kind, item["fx_id"], item["before"], item["after"]))
        elif kind == "group":
            old = (
                None
                if item["change"] == "created"
                else (item["name_before"], item["member_count_before"])
            )
            new = (
                None
                if item["change"] == "removed"
                else (item["name_after"], item["member_count_after"])
            )
            reported.add((kind, item["group_id"], old, new))
    return reported


def _random_write(case: NativeAcceptance, document: str, chooser: random.Random) -> None:
    graph = case.success("fx.graph", {"document_id": document})
    effects = [item["fx_id"] for item in graph["effects"] if item["type"] == "STD_blurFx"]
    action = chooser.choice(
        ("create", "delete", "connect", "disconnect", "terminal", "rename", "set", "place", "group")
    )
    if action == "create" or not effects:
        params: dict[str, Any] = {"type": "STD_blurFx"}
        tool = "fx.create"
    else:
        target = chooser.choice(effects)
        tool, params = {
            "delete": ("fx.delete", {"fx_id": target}),
            "connect": (
                "fx.connect",
                {"fx_id": target, "port": "Source", "source_fx_id": chooser.choice(effects)},
            ),
            "disconnect": ("fx.disconnect", {"fx_id": target, "port": "Source"}),
            "terminal": ("fx.terminal.set", {"fx_id": target, "enabled": chooser.random() < 0.5}),
            "rename": ("fx.rename", {"fx_id": target, "name": f"Blur {chooser.randrange(4)}"}),
            "set": (
                "fx.parameter.set",
                {"fx_id": target, "parameter": "value", "value": chooser.randrange(4)},
            ),
            "place": (
                "fx.layout.set",
                {"positions": [{"fx_id": target, "position": {"x": chooser.randrange(9), "y": 0}}]},
            ),
            "group": ("fx.group.create", {"fx_ids": chooser.sample(effects, min(2, len(effects)))}),
        }[action]
    response = case.worker.call(tool, case.write_params(document, params))
    if "result" in response:
        case.revisions[document] = response["result"]["revision"]


@acceptance_case
def fx_net_delta_matches_graph_diff(case: NativeAcceptance) -> None:
    """Over 64 seeded writes, refused ones included, the net delta from the first revision
    equals a diff of two fx.graph reads, page by page, and reading from the current revision
    is empty."""
    document = case.create("Net delta")
    start = case.revisions[document]
    before = _states(case, document)
    chooser = random.Random(20261119)
    for _ in range(64):
        _random_write(case, document, chooser)
    after = _states(case, document)
    changes: list[dict[str, Any]] = []
    offset: int | None = 0
    while offset is not None:
        page = case.success(
            "fx.graph.delta",
            {
                "document_id": document,
                "from_revision": start,
                "offset": offset,
                "limit": 7,
                "expected_revision": case.revisions[document],
            },
        )
        changes.extend(page["changes"])
        offset = page["next_offset"]
    case.assertEqual(page["total_changes"], len(changes))
    transient: set[str] = set()
    case.assertEqual(_reported(case, changes, transient), _expected(before, after))
    case.assertFalse(transient & (before["nodes"].keys() | after["nodes"].keys()))
    empty = case.success(
        "fx.graph.delta", {"document_id": document, "from_revision": case.revisions[document]}
    )
    case.assertEqual((empty["changes"], empty["next_offset"]), ([], None))
