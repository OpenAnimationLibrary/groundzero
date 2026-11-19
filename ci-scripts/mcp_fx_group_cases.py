"""Schematic groups, group names and node placement change no render, survive save and
reopen, and refuse what the editor could not produce."""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Any

import mcp_fx_authoring_cases as fx
from mcp_case import acceptance_case

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


def _graph(case: NativeAcceptance, document: str) -> dict[str, Any]:
    return case.success("fx.graph", {"document_id": document})


def _by_name(graph: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {effect["name"]: effect for effect in graph["effects"] if effect["name"]}


def _reopen(case: NativeAcceptance, document: str, name: str) -> tuple[str, Path]:
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / name))
    case.mutate("scene.discard", document)
    opened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    return opened, Path(saved["package_directory"])


def _named_groups(graph: dict[str, Any]) -> set[tuple[str, frozenset[str], str | None]]:
    """Groups by name, member names and enclosing group name, which survive a reopen."""
    names = {effect["fx_id"]: effect["name"] for effect in graph["effects"]}
    groups = {group["group_id"]: group for group in graph["groups"]}
    result = set()
    for group in graph["groups"]:
        parent = groups.get(group["parent_group_id"]) if group["parent_group_id"] else None
        members = frozenset(names[member] for member in group["fx_ids"])
        result.add((group["name"], members, parent["name"] if parent else None))
    return result


@acceptance_case
def fx_positions_survive_reopen(case: NativeAcceptance) -> None:
    """Placed, unplaced and generator positions read back, change no pixel, and reopen equal
    from a saved scene that carries them."""
    document, artwork, blur = fx._blur_fixture(case)
    card = case.mutate("fx.create", document, type="colorCardFx", name="Card")["fx_id"]
    output = _graph(case, document)["current_output_fx_id"]
    before = fx._pixels(case, document, 0)
    positions = [
        {"fx_id": blur, "position": {"x": 120.5, "y": -40}},
        {"fx_id": card, "position": {"x": -1000000, "y": 1000000}},
        {"fx_id": output, "position": None},
    ]
    result = case.mutate("fx.layout.set", document, positions=positions)
    case.assertEqual((result["changed_count"], result["positions"]), (2, positions))
    case.assertEqual(
        case.mutate("fx.layout.set", document, positions=positions)["changed_count"], 0
    )
    case.assertEqual(fx._pixels(case, document, 0), before)
    placed = {effect["fx_id"]: effect["position"] for effect in _graph(case, document)["effects"]}
    case.assertEqual(
        [placed[item["fx_id"]] for item in positions], [item["position"] for item in positions]
    )
    fine = case.mutate(
        "fx.layout.set",
        document,
        positions=[{"fx_id": artwork, "position": {"x": 0.123456789, "y": 7}}],
    )
    case.assertEqual(fine["positions"][0]["position"], {"x": 0.123457, "y": 7})
    reopened, package = _reopen(case, document, "placed")
    case.assertIn("dagNodePos", (package / "scene.tnz").read_text(errors="replace"))
    effects = _by_name(_graph(case, reopened))
    case.assertEqual(effects["Original blur"]["position"], {"x": 120.5, "y": -40})
    case.assertEqual(effects["Card"]["position"], {"x": -1000000, "y": 1000000})
    column = next(
        effect for effect in _graph(case, reopened)["effects"] if effect.get("column_index") == 0
    )
    case.assertEqual(column["position"], {"x": 0.123457, "y": 7})
    case.assertEqual(fx._pixels(case, reopened, 0), before)
    for bad in ({"x": 1000001, "y": 0}, {"x": 0}):
        error = case.failure(
            "fx.layout.set",
            case.write_params(reopened, {"positions": [{"fx_id": blur, "position": bad}]}),
        )
        case.assertIn(error["code"], {"invalid_params", "INVALID_ARGUMENTS"}, error)


def _group_fixture(case: NativeAcceptance) -> tuple[str, dict[str, str]]:
    document, artwork, blur = fx._blur_fixture(case)
    glow = case.mutate("fx.create", document, type="STD_glowFx", name="Glow")["fx_id"]
    case.mutate("fx.connect", document, fx_id=glow, port="Source", source_fx_id=blur)
    fx._output(case, document, glow)
    over = case.mutate("fx.create", document, type="overFx", name="Over")["fx_id"]
    spare = case.mutate("fx.create", document, type="STD_blurFx", name="Spare")["fx_id"]
    case.mutate("fx.connect", document, fx_id=over, port="Source1", source_fx_id=spare)
    return document, {"blur": blur, "glow": glow, "over": over, "spare": spare}


def _refusals(case: NativeAcceptance, document: str, nodes: dict[str, str], inner: str) -> None:
    output = _graph(case, document)["current_output_fx_id"]
    revision = case.revisions[document]
    for fx_ids, reason in (
        ([nodes["blur"], nodes["blur"]], "duplicate_member"),
        ([nodes["over"], output], "unsupported_member"),
        ([nodes["blur"], nodes["over"]], "splits_group"),
    ):
        error = case.failure("fx.group.create", case.write_params(document, {"fx_ids": fx_ids}))
        case.assertEqual((error["code"], error["details"]["reason"]), ("invalid_group", reason))
    split = case.failure(
        "fx.group.create", case.write_params(document, {"fx_ids": [nodes["blur"], nodes["over"]]})
    )
    case.assertEqual(split["details"]["group_id"], inner)
    unknown = "fxgroup_" + "0" * 8 + "-0000-0000-0000-" + "0" * 12
    error = case.failure("fx.group.remove", case.write_params(document, {"group_id": unknown}))
    case.assertEqual(error["code"], "unknown_group")
    macro = case.failure(
        "fx.macro.create",
        case.write_params(document, {"fx_ids": [nodes["blur"], nodes["glow"]], "name": "M"}),
    )
    case.assertEqual(
        (macro["code"], macro["details"]["reason"]), ("INVALID_MACRO", "grouped_member")
    )
    case.assertEqual(case.revisions[document], revision)
    summary = case.success("scene.summary", {"document_id": document})
    case.assertEqual(summary["revision"], revision)


@acceptance_case
def fx_groups_preserve_render_and_reopen(case: NativeAcceptance) -> None:
    """Nested groups, a macro in a group, a rename and an ungroup keep every pixel, read back
    with their names and nesting, survive reopen, and refuse splits, duplicates and outputs."""
    document, nodes = _group_fixture(case)
    before = fx._pixels(case, document, 0)
    inner = case.mutate("fx.group.create", document, fx_ids=[nodes["blur"], nodes["glow"]])["group"]
    case.assertTrue(inner["name"].startswith("Group "))
    case.assertEqual((inner["member_count"], inner["parent_group_id"]), (2, None))
    _refusals(case, document, nodes, inner["group_id"])
    macro = case.mutate(
        "fx.macro.create", document, fx_ids=[nodes["over"], nodes["spare"]], name="Pair"
    )
    case.mutate("fx.rename", document, fx_id=macro["node"]["fx_id"], name="Renamed pair")
    outer = case.mutate(
        "fx.group.create",
        document,
        fx_ids=[nodes["blur"], nodes["glow"], macro["node"]["fx_id"]],
        name="Outer",
    )["group"]
    case.assertEqual(outer["member_count"], 3)
    renamed = case.mutate("fx.group.rename", document, group_id=inner["group_id"], name="Inner")
    case.assertEqual((renamed["changed"], renamed["name_before"]), (True, inner["name"]))
    case.assertFalse(
        case.mutate("fx.group.rename", document, group_id=inner["group_id"], name="Inner")[
            "changed"
        ]
    )
    graph = _graph(case, document)
    effects = {effect["fx_id"]: effect for effect in graph["effects"]}
    case.assertEqual(effects[nodes["blur"]]["group_ids"], [inner["group_id"], outer["group_id"]])
    expected = {
        ("Inner", frozenset({"Original blur", "Glow"}), "Outer"),
        ("Outer", frozenset({"Original blur", "Glow", "Renamed pair"}), None),
    }
    case.assertEqual(_named_groups(graph), expected)
    case.assertEqual(fx._pixels(case, document, 0), before)
    reopened, _package = _reopen(case, document, "grouped")
    case.assertEqual(_named_groups(_graph(case, reopened)), expected)
    case.assertEqual(fx._pixels(case, reopened, 0), before)
    inner_again = next(
        group for group in _graph(case, reopened)["groups"] if group["name"] == "Inner"
    )
    removed = case.mutate("fx.group.remove", reopened, group_id=inner_again["group_id"])
    case.assertEqual((removed["name"], removed["former_member_count"]), ("Inner", 2))
    case.assertEqual(
        _named_groups(_graph(case, reopened)),
        {("Outer", frozenset({"Original blur", "Glow", "Renamed pair"}), None)},
    )
    members = _ids(case, reopened, "Original blur", "Glow", "Renamed pair")
    newest = case.mutate("fx.group.create", reopened, fx_ids=members)["group"]
    case.assertEqual((newest["name"], newest["member_count"]), ("Group 3", 3))
    case.assertEqual(fx._pixels(case, reopened, 0), before)


def _ids(case: NativeAcceptance, document: str, *names: str) -> list[str]:
    effects = _by_name(_graph(case, document))
    return [effects[name]["fx_id"] for name in names]
