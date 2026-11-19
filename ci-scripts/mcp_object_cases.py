"""Qualify peg duplicate, rename, delete, schematic groups and preserve-world reparenting."""

from __future__ import annotations

from collections.abc import Sequence
from typing import TYPE_CHECKING, Any

from mcp_animation_cases import _pixels_match, _render
from mcp_case import acceptance_case
from mcp_png import Pixel
from mcp_rig_cases import _column, _keys, _level, _read, _refused, _scene, _square

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

FRAMES = (0, 2, 4)


def _peg(case: NativeAcceptance, document: str, name: str) -> str:
    return case.mutate("object.create", document, name=name, kind="pegbar")["object_id"]


def _art(case: NativeAcceptance, document: str, name: str, rows: int = 5) -> dict[str, Any]:
    level = _level(case, document, name, 1)
    _square(case, document, level, (0, 0))
    return _column(case, document, name, level, [1] * rows)


def _link(
    case: NativeAcceptance, document: str, child: str, parent: str, policy: str, **params: Any
) -> dict[str, Any]:
    return case.mutate(
        "object.parent",
        document,
        object_id=child,
        parent_id=parent,
        placement_policy=policy,
        **params,
    )


def _placements(
    case: NativeAcceptance, document: str, objects: Sequence[str], frames: Sequence[int] = FRAMES
) -> list[dict[str, float]]:
    samples = _read(case, "rig.evaluate", document, object_ids=list(objects), frames=list(frames))[
        "samples"
    ]
    return [sample["placement"] for sample in samples]


def _same_placements(
    case: NativeAcceptance, actual: list[dict[str, float]], expected: list[dict[str, float]]
) -> None:
    case.assertEqual(len(actual), len(expected))
    for left, right in zip(actual, expected, strict=True):
        for key, value in right.items():
            case.assertAlmostEqual(left[key], value, 6, (key, left, right))


def _renders(case: NativeAcceptance, document: str) -> list[list[Pixel]]:
    return [_render(case, document, frame) for frame in FRAMES]


def _same_renders(case: NativeAcceptance, document: str, expected: list[list[Pixel]]) -> None:
    for actual, reference in zip(_renders(case, document), expected, strict=True):
        _pixels_match(case, actual, reference)


def _reopen(case: NativeAcceptance, document: str, name: str) -> str:
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / name))
    return case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]


def _by_name(case: NativeAcceptance, document: str) -> dict[str, dict[str, Any]]:
    objects = _read(case, "rig.inspect", document)["objects"]
    return {item["name"]: item for item in objects}


def _hierarchy(case: NativeAcceptance, document: str) -> dict[str, Any]:
    named = _by_name(case, document)
    names = {item["object_id"]: name for name, item in named.items()}
    return {
        name: (names.get(item["parent_id"]), item["parent_handle"]["native"])
        for name, item in named.items()
    }


def _groups(case: NativeAcceptance, document: str) -> list[tuple[str, list[str]]]:
    names = {item["object_id"]: name for name, item in _by_name(case, document).items()}
    groups = _read(case, "object.groups", document)["groups"]
    return sorted(
        (group["name"], sorted(names[i] for i in group["object_ids"])) for group in groups
    )


def _shot_rig(case: NativeAcceptance) -> tuple[str, dict[str, str]]:
    document = _scene(case, "Shot rig")
    background, character, camera_rig = (
        _peg(case, document, name) for name in ("BG", "Char", "Cam rig")
    )
    hero = _art(case, document, "Hero")["object_id"]
    _link(case, document, hero, character, "preserve_local")
    _link(case, document, character, background, "preserve_local")
    _keys(case, document, background, "x", "inch", f0=-0.25)
    _keys(case, document, character, "y", "inch", f0=0.25, f4=-0.25)
    _keys(case, document, hero, "rotation", "degree", f0=0, f4=90)
    objects = case.success("scene.summary", {"document_id": document})["objects"]
    camera = next(item["object_id"] for item in objects if item["kind"] == "camera")
    _link(case, document, camera, camera_rig, "preserve_local")
    rig = {"bg": background, "char": character, "cam": camera_rig, "hero": hero}
    return document, rig


@acceptance_case
def object_hierarchy_rename_group_round_trip(case: NativeAcceptance) -> None:
    """Rename and group a background, character and camera peg hierarchy, then reopen it."""
    document, rig = _shot_rig(case)
    for key, name in (("bg", "Background"), ("char", "Hero peg"), ("cam", "Camera rig")):
        renamed = case.mutate("object.rename", document, object_id=rig[key], name=name)
        case.assertEqual(renamed["name"], name)
    case.mutate("object.rename", document, object_id=rig["hero"], name="Héros ✓")
    before = _placements(case, document, [rig["hero"]])
    renders = _renders(case, document)
    character = case.mutate(
        "object.group", document, object_ids=[rig["char"], rig["hero"]], name="Character"
    )["group"]
    _refused(
        case,
        "object.group",
        document,
        "invalid_params",
        object_ids=[rig["hero"], rig["bg"]],
        name="Split",
    )
    case.mutate(
        "object.group", document, object_ids=[rig["char"], rig["hero"], rig["bg"]], name="Shot"
    )
    case.mutate(
        "object.group.rename",
        document,
        group_number=character["group_number"],
        name="Hero group",
    )
    _same_placements(case, _placements(case, document, [rig["hero"]]), before)
    reopened = _reopen(case, document, "hierarchy")
    case.assertEqual(_hierarchy(case, reopened), _hierarchy(case, document))
    case.assertEqual(_groups(case, reopened), _groups(case, document))
    case.assertEqual(
        _groups(case, reopened),
        [("Hero group", ["Hero peg", "Héros ✓"]), ("Shot", ["Background", "Hero peg", "Héros ✓"])],
    )
    hero = _by_name(case, reopened)["Héros ✓"]["object_id"]
    _same_placements(case, _placements(case, reopened, [hero]), before)
    _same_renders(case, reopened, renders)


def _moving_art(case: NativeAcceptance) -> tuple[str, str, str]:
    document = _scene(case, "Reparent")
    start = _peg(case, document, "Start")
    _keys(case, document, start, "x", "inch", f0=-0.25)
    art = _art(case, document, "Mover")["object_id"]
    _link(case, document, art, start, "preserve_local")
    _keys(case, document, art, "x", "inch", f0=0, f4=0.5)
    _keys(case, document, art, "rotation", "degree", f0=0, f4=45)
    return document, start, art


@acceptance_case
def object_reparent_preserve_local_and_world(case: NativeAcceptance) -> None:
    """Reparent a moving column under preserve_local and preserve_world, exactly or by a bake."""
    document, start, art = _moving_art(case)
    still = _peg(case, document, "Still")
    _keys(case, document, still, "y", "inch", f0=0.25)
    before = _placements(case, document, [art])
    renders = _renders(case, document)
    local = _link(case, document, art, still, "preserve_local", report_frames=list(FRAMES))
    case.assertFalse(local["world_placement_preserved"])
    case.assertNotIn("world_preservation", local)
    _link(case, document, art, start, "preserve_local")
    _same_placements(case, _placements(case, document, [art]), before)
    world = _link(case, document, art, still, "preserve_world", report_frames=list(FRAMES))
    case.assertTrue(world["world_placement_preserved"])
    preservation = world["world_preservation"]
    case.assertEqual(
        (preservation["method"], preservation["exact_at"]), ("exact_rewrite", "all_frames")
    )
    case.assertEqual(preservation["channels_rewritten"], ["x", "y"])
    _same_placements(case, _placements(case, document, [art]), before)
    _same_renders(case, document, renders)
    walker = _peg(case, document, "Walker")
    _keys(case, document, walker, "x", "inch", f0=0, f4=0.25)
    error = _refused(
        case,
        "object.parent",
        document,
        "unsupported_target",
        object_id=art,
        parent_id=walker,
        placement_policy="preserve_world",
    )
    proposal = error["details"]["bake_proposal"]
    case.assertEqual(error["details"]["reason"], "parent_branch_moves")
    case.assertEqual((proposal["start_frame"], proposal["end_frame"]), (0, 4))
    case.assertTrue(proposal["covers_all_animation"])
    policy = {key: proposal[key] for key in ("kind", "start_frame", "end_frame")}
    baked = _link(
        case,
        document,
        art,
        walker,
        "preserve_world",
        world_policy={**policy, "new_key_interpolation": "linear"},
    )["world_preservation"]
    case.assertEqual((baked["method"], baked["exact_at"]), ("baked", "integer_frames_in_range"))
    case.assertLessEqual(baked["max_translation_deviation_inch"], 1e-6)
    _same_placements(case, _placements(case, document, [art]), before)
    _same_renders(case, document, renders)
    _same_renders(case, _reopen(case, document, "reparent"), renders)


@acceptance_case
def object_subtree_duplicate_and_independent_removal(case: NativeAcceptance) -> None:
    """Duplicate a peg hierarchy that shares its level, animate the copy, then remove it."""
    document = _scene(case, "Duplicate")
    peg = _peg(case, document, "Rig")
    column = _art(case, document, "Shared")
    _link(case, document, column["object_id"], peg, "preserve_local")
    _keys(case, document, peg, "x", "inch", f0=-0.25, f4=0)
    before = _placements(case, document, [column["object_id"]])
    renders = _renders(case, document)
    copied = case.mutate(
        "object.duplicate",
        document,
        object_id=peg,
        name="Rig copy",
        scope="subtree",
        level_sharing="shared",
    )
    case.assertEqual(
        copied["object"]["parent_id"],
        _read(case, "rig.inspect", document, root_object_id=peg)["objects"][0]["parent_id"],
    )
    copy_peg = copied["object"]["object_id"]
    copy_column = next(item for item in copied["objects"] if item["kind"] == "column")
    case.assertEqual(copy_column["source_object_id"], column["object_id"])
    case.assertTrue(copy_column["output_connected"])
    cells = {"start_frame": 0, "length": 5}
    source_cells = case.success(
        "column.cells", {"document_id": document, "column_id": column["column_id"], **cells}
    )["cells"]
    copy_cells = case.success(
        "column.cells", {"document_id": document, "column_id": copy_column["column_id"], **cells}
    )["cells"]
    case.assertEqual(copy_cells, source_cells)
    _keys(case, document, copy_peg, "y", "inch", f0=0.5)
    _same_placements(case, _placements(case, document, [column["object_id"]]), before)
    moved = _placements(case, document, [copy_column["object_id"]])
    case.assertAlmostEqual(moved[0]["dy_inch"] - before[0]["dy_inch"], 0.5, 6)
    case.mutate("column.delete", document, column_id=copy_column["column_id"])
    removed = case.mutate(
        "object.delete",
        document,
        object_id=copy_peg,
        children_policy="refuse",
        report_frames=[0],
    )
    case.assertEqual(removed["reattached"], [])
    _same_placements(case, _placements(case, document, [column["object_id"]]), before)
    _same_renders(case, document, renders)
    _same_renders(case, _reopen(case, document, "duplicate"), renders)


@acceptance_case
def object_delete_preflight_and_refusals(case: NativeAcceptance) -> None:
    """List a peg's dependents, refuse its deletion, then re-attach its children."""
    document = _scene(case, "Delete")
    root = _peg(case, document, "Root")
    middle = _peg(case, document, "Middle")
    _keys(case, document, middle, "x", "inch", f0=0.5)
    art = _art(case, document, "Leaf")
    _link(case, document, middle, root, "preserve_local")
    _link(case, document, art["object_id"], middle, "preserve_local")
    target = {"kind": "object", "object_id": middle}
    dependents = _read(case, "rig.dependents", document, target=target)
    case.assertTrue(dependents["complete"])
    case.assertEqual(
        [(item["kind"], item["object_id"]) for item in dependents["dependents"]],
        [("child_object", art["object_id"])],
    )
    error = _refused(
        case,
        "object.delete",
        document,
        "has_dependents",
        object_id=middle,
        children_policy="refuse",
        report_frames=[0],
    )
    case.assertEqual(error["details"]["dependents"], dependents["dependents"])
    _refused(
        case, "column.delete", document, "has_dependents", column_id=_parent_column(case, document)
    )
    _refused(
        case,
        "object.delete",
        document,
        "unsupported_kind",
        object_id=art["object_id"],
        children_policy="refuse",
        report_frames=[0],
    )
    _refused(
        case,
        "object.rename",
        document,
        "unsupported_target",
        object_id=_table(case, document),
        name="Stage",
    )
    deleted = case.mutate(
        "object.delete",
        document,
        object_id=middle,
        children_policy="reattach_to_parent",
        report_frames=[0, 4],
    )
    moved = deleted["reattached"][0]
    case.assertEqual((moved["object_id"], moved["parent_id"]), (art["object_id"], root))
    change = moved["world_changes"][0]
    case.assertAlmostEqual(change["before"]["dx_inch"] - change["after"]["dx_inch"], 0.5, 6)
    case.assertFalse(change["preserved"])
    _refused(case, "rig.dependents", document, "invalid_handle", target=target)


def _table(case: NativeAcceptance, document: str) -> str:
    objects = case.success("scene.summary", {"document_id": document})["objects"]
    return next(item["object_id"] for item in objects if item["kind"] == "table")


def _parent_column(case: NativeAcceptance, document: str) -> str:
    holder = _art(case, document, "Holder")
    rider = case.mutate("column.create", document, name="Rider")["object_id"]
    _link(case, document, rider, holder["object_id"], "preserve_local")
    return holder["column_id"]
