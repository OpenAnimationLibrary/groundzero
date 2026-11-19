"""Column lifecycle cases: insert at a position, move, duplicate, split visibility and
delete with explicit FX and children policies, each saved and reopened through both
loaders."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_png import decode_png
from mcp_sheet_record_cases import save_and_reopen

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

ONE = {"number": 1, "suffix": ""}


def _summary(case: NativeAcceptance, document: str) -> dict[str, Any]:
    return case.success("scene.summary", {"document_id": document})


def _names(case: NativeAcceptance, document: str) -> list[str]:
    return [column["name"] for column in _summary(case, document)["columns"]]


def _graph(case: NativeAcceptance, document: str) -> dict[str, Any]:
    return case.success("fx.graph", {"document_id": document})


def _column_fx(case: NativeAcceptance, document: str, column: str) -> str:
    return next(
        node["fx_id"]
        for node in _graph(case, document)["effects"]
        if node.get("column_id") == column
    )


def _sources(case: NativeAcceptance, document: str, fx: str) -> list[str]:
    node = next(node for node in _graph(case, document)["effects"] if node["fx_id"] == fx)
    return [port.get("source_fx_id") for port in node["ports"] if port.get("connected")]


def _scene(case: NativeAcceptance, names: list[str]) -> tuple[str, str, list[str]]:
    document, level, first = case.populated_scene()
    case.mutate("column.update", document, column_id=first["column_id"], name=names[0])
    ids = [first["column_id"]]
    for name in names[1:]:
        column = case.mutate("column.create", document, name=name)["column_id"]
        case.mutate(
            "column.expose",
            document,
            column_id=column,
            level_id=level["level_id"],
            drawing_id=ONE,
            start_frame=0,
            length=4,
        )
        ids.append(column)
    return document, level["level_id"], ids


def _blur(case: NativeAcceptance, document: str, column: str) -> str:
    blur = case.mutate("fx.create", document, type="STD_blurFx", name="Blur")["fx_id"]
    case.mutate(
        "fx.connect",
        document,
        fx_id=blur,
        port="Source",
        source_fx_id=_column_fx(case, document, column),
    )
    return blur


def _refused(case: NativeAcceptance, method: str, document: str, code: str, **params: Any) -> dict:
    return case.handler_failure(method, case.write_params(document, params), code)


@acceptance_case
def columns_insert_and_move_keep_their_identity(case: NativeAcceptance) -> None:
    """A column inserted at a position and a block moved to the end keep every handle,
    object, parent and FX link; an unchanged order keeps the revision (SCN-06 #1, #3, F2)."""
    document, _level, (first, second, third) = _scene(case, ["A", "B", "C"])
    peg = case.mutate("object.create", document, name="Peg", kind="pegbar")["object_id"]
    objects = {c["column_id"]: c["object_id"] for c in _summary(case, document)["columns"]}
    case.mutate(
        "object.parent",
        document,
        object_id=objects[first],
        parent_id=peg,
        placement_policy="preserve_local",
    )
    blur = _blur(case, document, second)
    inserted = case.mutate("column.create", document, name="Z", position=0)
    case.assertEqual(inserted["position"], 0)
    case.assertEqual(_names(case, document), ["Z", "A", "B", "C"])
    result = case.mutate("column.move", document, column_ids=[first, second], before_position=4)
    case.assertEqual(_names(case, document), ["Z", "C", "A", "B"])
    case.assertEqual(
        {
            row["column_id"]: (row["position_before"], row["position_after"])
            for row in result["moved"]
        },
        {first: (1, 2), second: (2, 3), third: (3, 1)},
    )
    columns = {c["column_id"]: c for c in _summary(case, document)["columns"]}
    case.assertEqual(columns[first]["object_id"], objects[first])
    case.assertEqual(columns[first]["last_frame"], 24)
    objects_now = {o["object_id"]: o for o in _summary(case, document)["objects"]}
    case.assertEqual(objects_now[objects[first]]["parent_id"], peg)
    case.assertEqual(_sources(case, document, blur), [_column_fx(case, document, second)])
    revision = case.revisions[document]
    case.mutate("column.move", document, column_ids=[first, second], before_position=4)
    case.assertEqual(case.revisions[document], revision)
    case.mutate("column.delete", document, column_id=inserted["column_id"])
    save_and_reopen(case, document, "moved")


@acceptance_case
def camstand_and_preview_visibility_are_separate(case: NativeAcceptance) -> None:
    """Hiding a column on the camstand keeps it in renders; hiding it from preview removes
    it; the flags never change each other and survive a reopen (SCN-06 #4, F5)."""
    document, _level, column = case.populated_scene()
    target = column["column_id"]
    before = case.preview(document)
    updated = case.mutate("column.update", document, column_id=target, camstand_visible=False)
    case.assertEqual((updated["preview_visible"], updated["camstand_visible"]), (True, False))
    case.assertEqual(case.preview(document), before)
    case.mutate("column.update", document, column_id=target, preview_visible=False)
    hidden = decode_png(case.preview(document))
    case.assertNotEqual(hidden, decode_png(before))
    row = _summary(case, document)["columns"][0]
    case.assertEqual((row["preview_visible"], row["camstand_visible"]), (False, False))
    error = _refused(
        case,
        "column.update",
        document,
        "invalid_params",
        column_id=target,
        visible=True,
        camstand_visible=True,
    )
    case.assertEqual(error["details"]["reason"], "visibility_fields")
    error = _refused(
        case, "column.update", document, "invalid_params", column_id=target, color_filter_id=63
    )
    case.assertEqual(error["details"]["reason"], "color_filter_id")
    case.mutate("column.update", document, column_id=target, preview_visible=True)
    save_and_reopen(case, document, "visibility")


@acceptance_case
def column_delete_needs_explicit_fx_and_children_policies(case: NativeAcceptance) -> None:
    """A column feeding an effect, or with stage children, is refused until a policy is
    given; delete_exclusive removes the effect, disconnect keeps it unconnected, and
    reattached children report their world change (SCN-06 #5, F3)."""
    document, _level, (first, second, third) = _scene(case, ["A", "B", "C"])
    removed, kept = _blur(case, document, first), _blur(case, document, second)
    error = _refused(case, "column.delete", document, "has_dependents", column_id=first)
    case.assertEqual(error["details"]["fx"], [removed])
    result = case.mutate("column.delete", document, column_id=first, fx_policy="delete_exclusive")
    case.assertEqual(result["fx_removed"], [removed])
    case.assertNotIn(removed, [node["fx_id"] for node in _graph(case, document)["effects"]])
    result = case.mutate("column.delete", document, column_id=second, fx_policy="disconnect")
    case.assertEqual(result["fx_disconnected"], [kept])
    case.assertEqual(_sources(case, document, kept), [])
    objects = {c["column_id"]: c["object_id"] for c in _summary(case, document)["columns"]}
    child = case.mutate("column.create", document, name="Child")["object_id"]
    case.mutate(
        "object.parent",
        document,
        object_id=child,
        parent_id=objects[third],
        placement_policy="preserve_local",
    )
    error = _refused(case, "column.delete", document, "has_dependents", column_id=third)
    case.assertEqual(error["details"]["children"], [child])
    result = case.mutate(
        "column.delete",
        document,
        column_id=third,
        children_policy="reattach_to_parent",
        report_frames=[0],
    )
    case.assertEqual(result["children_reattached"][0]["object_id"], child)
    case.assertIn("world_changes", result["children_reattached"][0])
    case.failure(
        "column.cells", {"document_id": document, "column_id": third, "start_frame": 0, "length": 1}
    )


def _x_keys(case: NativeAcceptance, document: str, object_id: str) -> list:
    return case.success(
        "animation.keys",
        {"document_id": document, "object_id": object_id, "channel": "x", "unit": "inch"},
    )["keys"]


@acceptance_case
def column_duplicates_share_levels_and_copy_curves(case: NativeAcceptance) -> None:
    """A duplicate shows the same drawings, copies curves and the FX branch fed only by
    its source, and later edits of either column leave the other alone (SCN-06 #2)."""
    document, level, (source,) = _scene(case, ["Source"])
    peg = case.mutate("object.create", document, name="Peg", kind="pegbar")["object_id"]
    source_object = _summary(case, document)["columns"][0]["object_id"]
    case.mutate(
        "object.parent",
        document,
        object_id=source_object,
        parent_id=peg,
        placement_policy="preserve_local",
    )
    key = {"object_id": source_object, "channel": "x", "unit": "inch"}
    case.mutate(
        "animation.set_keys",
        document,
        **key,
        keys=[
            {"frame": 0, "value": 0.5, "interpolation": "linear"},
            {"frame": 10, "value": 1, "interpolation": "constant"},
        ],
    )
    blur = _blur(case, document, source)
    result = case.mutate(
        "column.duplicate",
        document,
        column_id=source,
        name="Copy",
        parent_policy="same_parent",
        fx_policy="exclusive_branch",
    )
    copy = result["column"]
    case.assertEqual((copy["position"], copy["name"]), (1, "Copy"))
    case.assertEqual(result["shared_levels"], [level])
    case.assertEqual(result["fx_map"][0]["source_fx_id"], blur)
    clone = result["fx_map"][0]["fx_id"]
    case.assertEqual(
        _sources(case, document, clone), [_column_fx(case, document, copy["column_id"])]
    )
    case.assertEqual(
        _x_keys(case, document, copy["object_id"]), _x_keys(case, document, source_object)
    )
    objects = {o["object_id"]: o for o in _summary(case, document)["objects"]}
    case.assertEqual(objects[copy["object_id"]]["parent_id"], peg)
    original = _x_keys(case, document, source_object)
    case.mutate(
        "animation.set_keys",
        document,
        object_id=copy["object_id"],
        channel="x",
        unit="inch",
        keys=[{"frame": 0, "value": 3, "interpolation": "linear"}],
    )
    case.assertEqual(_x_keys(case, document, source_object), original)
    table = case.mutate(
        "column.duplicate",
        document,
        column_id=source,
        name="Loose",
        parent_policy="table",
        fx_policy="column_only",
    )
    objects = {o["object_id"]: o for o in _summary(case, document)["objects"]}
    case.assertEqual(objects[table["column"]["object_id"]]["kind"], "column")
    case.assertNotEqual(objects[table["column"]["object_id"]]["parent_id"], peg)
    created = case.mutate("audio.column.create", document, name="Voice", volume_permille=1000)
    _refused(
        case,
        "column.duplicate",
        document,
        "incompatible_column",
        column_id=created["column"]["column_id"],
        name="Voice copy",
        parent_policy="table",
        fx_policy="column_only",
    )
    save_and_reopen(case, document, "duplicate")


@acceptance_case
def column_kinds_name_their_tools(case: NativeAcceptance) -> None:
    """Every native column kind is listed with the tool that creates it or why not."""
    kinds = {row["kind"]: row for row in case.success("column.kinds", {})["kinds"]}
    case.assertEqual(
        set(kinds), {"level", "sound", "generator", "annotation", "sub_xsheet", "palette", "mesh"}
    )
    case.assertEqual(kinds["level"]["create_tool"], "column.create")
    case.assertTrue(kinds["level"]["available"])
    case.assertEqual(kinds["mesh"]["reason"], "not_in_editable_profile")


@acceptance_case
def column_lifecycle_refuses_macros_and_other_sheets(case: NativeAcceptance) -> None:
    """A macro fed only by the column is refused rather than split, and a duplicate never
    names another sheet, so no cross-sheet copy can keep external links (SCN-06 F4)."""
    document, level, (column,) = _scene(case, ["Source"])
    first = _blur(case, document, column)
    second = case.mutate("fx.create", document, type="STD_blurFx", name="Second")["fx_id"]
    case.mutate("fx.connect", document, fx_id=second, port="Source", source_fx_id=first)
    case.mutate("fx.macro.create", document, fx_ids=[first, second], name="Pair")
    for method, extra in (
        ("column.delete", {"fx_policy": "delete_exclusive"}),
        (
            "column.duplicate",
            {"name": "Copy", "parent_policy": "table", "fx_policy": "exclusive_branch"},
        ),
    ):
        error = _refused(case, method, document, "unsupported_target", column_id=column, **extra)
        case.assertEqual(error["details"]["reason"], "macro_in_branch")
    error = _refused(
        case,
        "column.duplicate",
        document,
        "unsupported_target",
        column_id=column,
        name="Elsewhere",
        parent_policy="table",
        fx_policy="column_only",
        sheet_path=[{"column_id": column, "level_id": level}],
    )
    case.assertEqual(error["details"]["reason"], "nested_sheet_editing")
