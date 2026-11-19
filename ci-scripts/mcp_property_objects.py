"""Stage objects, schematic groups and column order for the property model.

Everything here is written from the tools' documented behavior. A scene's pegs and columns are
its objects; each has one parent, a peg or None for the table, and a stack of schematic groups,
innermost first. The functions at module level only read and change a scene, so a unit test
can call them without a worker; `build` makes the operations from the model's own classes.

Not modelled: a column as a parent, a peg that follows a path, copies of an object that has
children (the copies keep their names, and the model names objects uniquely), and a copy of an
object that is in a group (what the copy's group stack holds is not documented).
"""

from __future__ import annotations

import dataclasses
import random
from collections.abc import Callable
from typing import Any

from mcp_property_curves import ordered, sample
from mcp_property_state import Column, ModelError

Step = dict[str, Any]
CHANNELS = ("x", "y")
MOTION_CHANNELS = ("x", "y", "rotation", "scale")
TILTING_CHANNELS = ("rotation", "scale")
MAX_DEPTH = 3
MAX_COLUMNS = 6
MAX_PEGS = 6
MOVE_COUNT = 3
GROUP_MEMBERS = 4


def objects(scene: Any) -> list[str]:
    """The names of the pegs and the columns, pegs first."""
    return [*scene.pegs, *(column.name for column in scene.columns)]


def require(scene: Any, *names: str) -> None:
    """Raise ModelError for an object the scene does not hold, as in a shortened sequence."""
    for name in names:
        if name not in objects(scene):
            raise ModelError(f"no object named {name!r}")


def is_column(scene: Any, name: str) -> bool:
    """Whether the object called `name` is a column's."""
    return any(column.name == name for column in scene.columns)


def children(scene: Any, name: str) -> list[str]:
    """The objects whose parent is `name`."""
    return [child for child in objects(scene) if scene.parents.get(child) == name]


def ancestry(scene: Any, name: str | None) -> list[str]:
    """`name` and the pegs above it, nearest first; the table is not listed."""
    chain: list[str] = []
    while name is not None:
        chain.append(name)
        name = scene.parents.get(name)
    return chain


def descendants(scene: Any, name: str) -> list[str]:
    """Every object below `name`."""
    found: list[str] = []
    pending = children(scene, name)
    while pending:
        current = pending.pop()
        found.append(current)
        pending.extend(children(scene, current))
    return found


def locked(scene: Any, name: str) -> bool:
    """Whether a column's lock stops edits of its object."""
    return any(column.name == name and column.locked for column in scene.columns)


def static_value(scene: Any, name: str, channel: str) -> float:
    """The value of an object's x or y at frame 0: its curve's, or the channel's default."""
    keys = scene.curves.get((name, channel))
    if keys:
        return sample(ordered(keys), 0.0)
    return scene.defaults.get((name, channel), 0.0)


def origin(scene: Any, name: str) -> list[float]:
    """The x and y of an object at frame 0."""
    return [static_value(scene, name, channel) for channel in CHANNELS]


def forget_object(scene: Any, name: str) -> None:
    """Remove what the scene holds under an object's name; its children go to its parent."""
    parent = scene.parents.pop(name, None)
    for child in children(scene, name):
        scene.parents[child] = parent
    for key in [key for key in scene.curves if key[0] == name]:
        del scene.curves[key]
    scene.cycles = {key for key in scene.cycles if key[0] != name}
    scene.defaults = {key: value for key, value in scene.defaults.items() if key[0] != name}
    scene.stacks.pop(name, None)
    if name in scene.pegs:
        scene.pegs.remove(name)


def rename_object(scene: Any, old: str, new: str) -> None:
    """Give an object another name everywhere the scene keys it."""
    scene.pegs = [new if peg == old else peg for peg in scene.pegs]
    scene.parents = {
        (new if name == old else name): (new if parent == old else parent)
        for name, parent in scene.parents.items()
    }
    scene.curves = {(new if k[0] == old else k[0], k[1]): v for k, v in scene.curves.items()}
    scene.cycles = {(new if k[0] == old else k[0], k[1]) for k in scene.cycles}
    scene.defaults = {(new if k[0] == old else k[0], k[1]): v for k, v in scene.defaults.items()}
    if old in scene.stacks:
        scene.stacks[new] = scene.stacks.pop(old)


def copy_object(scene: Any, source: str, name: str) -> None:
    """Give `name` its own copy of an object's curves, defaults and parent."""
    scene.parents[name] = scene.parents.get(source)
    for (owner, channel), keys in list(scene.curves.items()):
        if owner == source:
            scene.curves[(name, channel)] = {
                frame: dataclasses.replace(key) for frame, key in keys.items()
            }
            if (owner, channel) in scene.cycles:
                scene.cycles.add((name, channel))
    for (owner, channel), value in list(scene.defaults.items()):
        if owner == source:
            scene.defaults[(name, channel)] = value


# --------------------------------------------------------------------- columns


def insert_column(scene: Any, column: Any, position: int) -> None:
    """Put a column into the sheet at a position and make its object a child of the table."""
    scene.columns.insert(position, column)
    scene.parents[column.name] = None


def column_insert_effect(scene: Any, step: Step) -> str | None:
    """column.create with a position."""
    if step["position"] > len(scene.columns):
        return "invalid_params/position"
    insert_column(scene, Column(step["name"]), step["position"])
    return None


def moved_order(names: list[str], moving: list[str], before: int) -> list[str]:
    """The column names after a block moves, in its current order, before an original position."""
    block = [name for name in names if name in moving]
    rest = [name for name in names if name not in moving]
    at = sum(1 for index, name in enumerate(names) if name not in moving and index < before)
    return rest[:at] + block + rest[at:]


def column_move_effect(scene: Any, step: Step) -> str | None:
    """column.move."""
    names = [column.name for column in scene.columns]
    if not set(step["columns"]) <= set(names):
        raise ModelError("a column to move does not exist")
    if step["before"] > len(names):
        return "invalid_params/position"
    if len(set(step["columns"])) != len(step["columns"]):
        return "invalid_params/duplicate_column"
    order = moved_order(names, step["columns"], step["before"])
    by_name = {column.name: column for column in scene.columns}
    scene.columns[:] = [by_name[name] for name in order]
    return None


def column_duplicate_effect(scene: Any, step: Step) -> str | None:
    """column.duplicate of a level column, its effects not copied."""
    source = scene.column(step["column"])
    index = scene.columns.index(source)
    position = step.get("position", index + 1)
    if position > len(scene.columns):
        return "invalid_params/position"
    copy = dataclasses.replace(source, name=step["name"], cells=dict(source.cells))
    insert_column(scene, copy, position)
    if step["parent_policy"] == "same_parent":
        scene.parents[step["name"]] = scene.parents.get(source.name)
    for (owner, channel), value in list(scene.defaults.items()):
        if owner == source.name:
            scene.defaults[(step["name"], channel)] = value
    return None


# ------------------------------------------------------------------ the parent


def world_refusal(scene: Any, name: str, parent: str) -> str | None:
    """The refusal when a parent branch moves over time, else None."""
    old, new = ancestry(scene, scene.parents.get(name)), ancestry(scene, parent)
    branches = [[o for o in old if o not in new], [o for o in new if o not in old]]
    for branch in branches:
        for member in branch:
            for channel in MOTION_CHANNELS:
                if len(scene.curves.get((member, channel), {})) > 1:
                    return "unsupported_target"
    return None


def tilted(scene: Any, name: str, parent: str) -> bool:
    """Whether a rotated or scaled frame lies between the old and the new parent."""
    old, new = ancestry(scene, scene.parents.get(name)), ancestry(scene, parent)
    between = [o for o in old if o not in new] + [o for o in new if o not in old]
    return any(scene.curves.get((o, c)) for o in between for c in TILTING_CHANNELS)


def attachment_shift(scene: Any, name: str, parent: str) -> list[float]:
    """How far the object's own x and y move so that it stays where it is in the world."""
    old, new = ancestry(scene, scene.parents.get(name)), ancestry(scene, parent)
    old_branch = [o for o in old if o not in new]
    new_branch = [o for o in new if o not in old]
    shift = [0.0, 0.0]
    for member in old_branch:
        shift = [a + b for a, b in zip(shift, origin(scene, member), strict=True)]
    for member in new_branch:
        shift = [a - b for a, b in zip(shift, origin(scene, member), strict=True)]
    return shift


def shift_object(scene: Any, name: str, shift: list[float]) -> None:
    """Add a constant to an object's x and y: its default and every key."""
    for channel, delta in zip(CHANNELS, shift, strict=True):
        if delta == 0:
            continue
        key = (name, channel)
        scene.defaults[key] = scene.defaults.get(key, 0.0) + delta
        for frame, item in scene.curves.get(key, {}).items():
            scene.curves[key][frame] = dataclasses.replace(item, value=item.value + delta)


def parent_effect(scene: Any, step: Step) -> str | None:
    """object.parent under a peg."""
    name, parent = step["object"], step["parent"]
    require(scene, name, parent)
    if locked(scene, name):
        return "locked"
    if parent == name or parent in descendants(scene, name):
        return "cycle"
    if scene.parents.get(name) == parent:
        return None
    if step["policy"] == "preserve_world":
        refusal = world_refusal(scene, name, parent)
        if refusal:
            return refusal
        if tilted(scene, name, parent):
            raise ModelError("a rotated or scaled frame between the parents is not modelled")
        shift_object(scene, name, attachment_shift(scene, name, parent))
    scene.parents[name] = parent
    return None


def parent_depth_fits(scene: Any, name: str, parent: str) -> bool:
    """Whether the object and everything below it stay within the depth the model generates."""
    below = max(
        (len(ancestry(scene, d)) - len(ancestry(scene, name)) for d in descendants(scene, name)),
        default=0,
    )
    return len(ancestry(scene, parent)) + 1 + below <= MAX_DEPTH


# ---------------------------------------------------------- rename, copy, delete


def rename_effect(scene: Any, step: Step) -> str | None:
    """object.rename of a peg or a column."""
    require(scene, step["object"])
    if locked(scene, step["object"]):
        return "locked"
    if is_column(scene, step["object"]):
        scene.column(step["object"]).name = step["name"]
    rename_object(scene, step["object"], step["name"])
    return None


def duplicate_effect(scene: Any, step: Step) -> str | None:
    """object.duplicate of a peg, or of a column with nothing below it."""
    source = step["object"]
    require(scene, source)
    if step.get("level_sharing") is None and step["scope"] == "subtree":
        return "invalid_params"
    if step["scope"] == "object" and is_column(scene, source):
        return "unsupported_kind"
    if is_column(scene, source):
        column = scene.column(source)
        copy = dataclasses.replace(column, name=step["name"], cells=dict(column.cells))
        scene.columns.append(copy)
        copy_object(scene, source, step["name"])
    else:
        scene.pegs.append(step["name"])
        copy_object(scene, source, step["name"])
    return None


def delete_effect(scene: Any, step: Step) -> str | None:
    """object.delete of a peg."""
    if step["object"] not in scene.pegs:
        raise ModelError(f"no peg named {step['object']!r}")
    kids = children(scene, step["object"])
    if kids and step["policy"] == "refuse":
        return "has_dependents"
    if any(locked(scene, kid) for kid in kids):
        return "locked"
    forget_object(scene, step["object"])
    return None


# ----------------------------------------------------------------------- groups


def members(scene: Any, group: int) -> list[str]:
    """The objects that belong to a group, directly or through an inner group."""
    return [name for name in objects(scene) if group in scene.stacks.get(name, [])]


def group_effect(scene: Any, step: Step) -> str | None:
    """object.group."""
    chosen = step["objects"]
    require(scene, *chosen)
    if len(set(chosen)) != len(chosen):
        return "invalid_params"
    for name in chosen:
        stack = scene.stacks.get(name, [])
        if not stack:
            continue
        top = stack[-1]
        shown = [m for m in members(scene, top) if scene.stacks[m][-1] == top]
        if any(member not in chosen for member in shown):
            return "invalid_params"
    scene.group_counter = step["key"]
    scene.group_names[step["key"]] = step["name"]
    for name in chosen:
        scene.stacks.setdefault(name, []).append(step["key"])
    return None


def ungroup_effect(scene: Any, step: Step) -> str | None:
    """object.ungroup."""
    group = step["group"]
    if group not in scene.group_names:
        raise ModelError(f"no group {group}")
    inside = members(scene, group)
    if not inside:
        return "not_found"
    if any(scene.stacks[name][-1] != group for name in inside):
        return "invalid_params"
    for name in inside:
        scene.stacks[name].pop()
    del scene.group_names[group]
    return None


def group_rename_effect(scene: Any, step: Step) -> str | None:
    """object.group.rename."""
    if step["group"] not in scene.group_names:
        raise ModelError(f"no group {step['group']}")
    if not members(scene, step["group"]):
        return "not_found"
    scene.group_names[step["group"]] = step["name"]
    return None


def live_groups(scene: Any) -> list[int]:
    """The groups that still have a member, oldest first."""
    return [group for group in sorted(scene.group_names) if members(scene, group)]


def group_record(scene: Any, group: int) -> dict[str, Any]:
    """A group as the model expects the worker to list it, by the model's own group numbers."""
    inside = members(scene, group)
    stack = scene.stacks[inside[0]]
    position = stack.index(group)
    outer = stack[position + 1] if position + 1 < len(stack) else None
    return {
        "name": scene.group_names[group],
        "members": sorted(inside),
        "parent": None if outer is None else str(outer),
    }


def expected_objects(scene: Any) -> dict[str, Any]:
    """The parents, the frame 0 positions and the groups of the scene's objects."""
    names = objects(scene)
    return {
        "parents": {name: scene.parents.get(name) for name in names},
        "origins": {name: origin(scene, name) for name in names},
        "groups": {str(group): group_record(scene, group) for group in live_groups(scene)},
    }


# ------------------------------------------------------------------- choosing


def choose_insert(rng: random.Random, scene: Any, fresh: Callable[..., str]) -> Step:
    """A column inserted at a position, now and then past the end."""
    count = len(scene.columns)
    position = count + 1 if rng.random() < 0.06 else rng.randint(0, count)
    return {"name": fresh(rng, scene, "C"), "position": position}


def choose_move(rng: random.Random, scene: Any) -> Step:
    """A block of columns moved before a position, now and then badly."""
    names = [column.name for column in scene.columns]
    chosen = rng.sample(names, rng.randint(1, min(MOVE_COUNT, len(names))))
    before = rng.randint(0, len(names))
    roll = rng.random()
    if roll < 0.05:
        before = len(names) + 1
    elif roll < 0.1:
        chosen = [*chosen, chosen[0]]
    return {"columns": chosen, "before": before}


def choose_column_duplicate(rng: random.Random, scene: Any, fresh: Callable[..., str]) -> Step:
    """A duplicate of a column, placed after it or at a position."""
    step: Step = {
        "column": rng.choice(scene.columns).name,
        "name": fresh(rng, scene, "D"),
        "parent_policy": rng.choice(("same_parent", "table")),
    }
    if rng.random() < 0.4:
        count = len(scene.columns)
        step["position"] = count + 1 if rng.random() < 0.1 else rng.randint(0, count)
    return step


def choose_parent(rng: random.Random, scene: Any, merged: bool) -> Step:
    """An object under a peg, keeping local keys or the world placement where that is modelled.

    Before the rigging tools are merged the worker neither places the world nor declares the
    `locked` answer of object.parent, so a locked column is not chosen.
    """
    names = [n for n in objects(scene) if merged or not locked(scene, n)]
    name = rng.choice(names or scene.pegs)
    candidates = [p for p in scene.pegs if parent_depth_fits(scene, name, p) or p == name]
    parent = rng.choice(candidates)
    policy = "preserve_world" if merged and rng.random() < 0.6 else "preserve_local"
    if policy == "preserve_world" and tilted(scene, name, parent):
        policy = "preserve_local"
    return {"object": name, "parent": parent, "policy": policy}


def choose_duplicate(rng: random.Random, scene: Any, fresh: Callable[..., str]) -> Step:
    """A copy of a peg or a column that has nothing below it and is in no group."""
    names = [n for n in objects(scene) if not children(scene, n) and not scene.stacks.get(n)]
    source = rng.choice(names)
    scope = "subtree" if is_column(scene, source) or rng.random() < 0.5 else "object"
    step: Step = {"object": source, "name": fresh(rng, scene, "X"), "scope": scope}
    if scope == "subtree" and rng.random() > 0.05:
        step["level_sharing"] = "shared"
    return step


def choose_group(rng: random.Random, scene: Any, fresh: Callable[..., str]) -> Step:
    """Two to four objects grouped under a name, now and then one twice."""
    names = objects(scene)
    chosen = rng.sample(names, rng.randint(2, min(GROUP_MEMBERS, len(names))))
    if rng.random() < 0.05:
        chosen = [*chosen, chosen[0]]
    return {"objects": chosen, "name": fresh(rng, scene, "G"), "key": scene.group_counter + 1}


def available_for_duplicate(scene: Any) -> bool:
    """Whether some object can be copied by the operations that are modelled."""
    room = len(scene.columns) < MAX_COLUMNS and len(scene.pegs) < MAX_PEGS
    return room and any(not children(scene, n) and not scene.stacks.get(n) for n in objects(scene))


# ------------------------------------------------------------------- the tools


def object_id(handles: dict[str, dict[str, str]], name: str) -> str:
    """The handle of a peg's or a column's stage object."""
    return handles["peg"].get(name) or handles["column_object"][name]


def _insert_request(step: Step, handles: dict[str, dict[str, str]]) -> tuple[str, dict[str, Any]]:
    return "column.create", {"name": step["name"], "position": step["position"]}


def _learn_column(step: Step, result: dict[str, Any], handles: dict[str, Any]) -> None:
    record = result.get("column", result)
    handles["column"][step["name"]] = record["column_id"]
    handles["column_object"][step["name"]] = record["object_id"]


def _move_request(step: Step, handles: dict[str, dict[str, str]]) -> tuple[str, dict[str, Any]]:
    ids = [handles["column"][name] for name in step["columns"]]
    return "column.move", {"column_ids": ids, "before_position": step["before"]}


def _column_duplicate_request(
    step: Step, handles: dict[str, dict[str, str]]
) -> tuple[str, dict[str, Any]]:
    params: dict[str, Any] = {
        "column_id": handles["column"][step["column"]],
        "name": step["name"],
        "parent_policy": step["parent_policy"],
        "fx_policy": "column_only",
    }
    if "position" in step:
        params["position"] = step["position"]
    return "column.duplicate", params


def _parent_request(step: Step, handles: dict[str, dict[str, str]]) -> tuple[str, dict[str, Any]]:
    return "object.parent", {
        "object_id": object_id(handles, step["object"]),
        "parent_id": handles["peg"][step["parent"]],
        "placement_policy": step["policy"],
    }


def _rename_request(step: Step, handles: dict[str, dict[str, str]]) -> tuple[str, dict[str, Any]]:
    return "object.rename", {"object_id": object_id(handles, step["object"]), "name": step["name"]}


def _learn_rename(step: Step, result: dict[str, Any], handles: dict[str, Any]) -> None:
    old, new = step["object"], step["name"]
    for kind in ("peg", "column", "column_object"):
        if old in handles[kind]:
            handles[kind][new] = handles[kind].pop(old)


def _duplicate_request(
    step: Step, handles: dict[str, dict[str, str]]
) -> tuple[str, dict[str, Any]]:
    params: dict[str, Any] = {
        "object_id": object_id(handles, step["object"]),
        "name": step["name"],
        "scope": step["scope"],
    }
    if "level_sharing" in step:
        params["level_sharing"] = step["level_sharing"]
    return "object.duplicate", params


def _learn_duplicate(step: Step, result: dict[str, Any], handles: dict[str, Any]) -> None:
    copy = result["object"]
    if copy["kind"] == "column":
        handles["column"][step["name"]] = result["objects"][0]["column_id"]
        handles["column_object"][step["name"]] = copy["object_id"]
    else:
        handles["peg"][step["name"]] = copy["object_id"]


def _delete_request(step: Step, handles: dict[str, dict[str, str]]) -> tuple[str, dict[str, Any]]:
    return "object.delete", {
        "object_id": handles["peg"][step["object"]],
        "children_policy": step["policy"],
        "report_frames": [],
    }


def _group_request(step: Step, handles: dict[str, dict[str, str]]) -> tuple[str, dict[str, Any]]:
    ids = [object_id(handles, name) for name in step["objects"]]
    return "object.group", {"object_ids": ids, "name": step["name"]}


def _learn_group(step: Step, result: dict[str, Any], handles: dict[str, Any]) -> None:
    handles["group"][str(step["key"])] = result["group"]["group_number"]


def _ungroup_request(step: Step, handles: dict[str, dict[str, str]]) -> tuple[str, dict[str, Any]]:
    return "object.ungroup", {"group_number": handles["group"][str(step["group"])]}


def _group_rename_request(
    step: Step, handles: dict[str, dict[str, str]]
) -> tuple[str, dict[str, Any]]:
    number = handles["group"][str(step["group"])]
    return "object.group.rename", {"group_number": number, "name": step["name"]}


def build(model: Any) -> dict[str, tuple[Any, tuple[str, ...]]]:
    """Return each operation with the tools it needs, for the model to register."""
    op, fresh, pick = model.Operation, model._name, model._pick

    def one(
        name: str, weight: int, available: Any, choose: Any, effect: Any, request: Any, learn: Any
    ):
        return op(name, weight, available, choose, effect, request, learn)

    def nothing(step: Step, result: dict[str, Any], handles: dict[str, Any]) -> None:
        return None

    def forget_peg(step: Step, result: dict[str, Any], handles: dict[str, Any]) -> None:
        del handles["peg"][step["object"]]

    def pick_group(rng: random.Random, scene: Any) -> int:
        return pick(rng, live_groups(scene))

    return {
        "column_insert": (
            one(
                "column_insert",
                3,
                lambda scene: len(scene.columns) < MAX_COLUMNS,
                lambda rng, scene: choose_insert(rng, scene, fresh),
                column_insert_effect,
                _insert_request,
                _learn_column,
            ),
            ("column.move",),
        ),
        "column_move": (
            one(
                "column_move",
                4,
                lambda scene: len(scene.columns) > 1,
                choose_move,
                column_move_effect,
                _move_request,
                nothing,
            ),
            ("column.move",),
        ),
        "column_duplicate": (
            one(
                "column_duplicate",
                3,
                lambda scene: bool(scene.columns) and len(scene.columns) < MAX_COLUMNS,
                lambda rng, scene: choose_column_duplicate(rng, scene, fresh),
                column_duplicate_effect,
                _column_duplicate_request,
                _learn_column,
            ),
            ("column.duplicate",),
        ),
        "object_parent": (
            one(
                "object_parent",
                4,
                lambda scene: bool(scene.pegs),
                lambda rng, scene: choose_parent(rng, scene, "rig.dependents" in model.TOOLS),
                parent_effect,
                _parent_request,
                nothing,
            ),
            ("object.parent",),
        ),
        "object_rename": (
            one(
                "object_rename",
                3,
                lambda scene: bool(objects(scene)),
                lambda rng, scene: {
                    "object": pick(rng, objects(scene)),
                    "name": fresh(rng, scene, "N"),
                },
                rename_effect,
                _rename_request,
                _learn_rename,
            ),
            ("object.rename",),
        ),
        "object_duplicate": (
            one(
                "object_duplicate",
                3,
                available_for_duplicate,
                lambda rng, scene: choose_duplicate(rng, scene, fresh),
                duplicate_effect,
                _duplicate_request,
                _learn_duplicate,
            ),
            ("object.duplicate",),
        ),
        "object_delete": (
            one(
                "object_delete",
                2,
                lambda scene: bool(scene.pegs),
                lambda rng, scene: {
                    "object": pick(rng, scene.pegs),
                    "policy": pick(rng, ("refuse", "reattach_to_parent")),
                },
                delete_effect,
                _delete_request,
                forget_peg,
            ),
            ("object.delete",),
        ),
        "group_create": (
            one(
                "group_create",
                3,
                lambda scene: len(objects(scene)) > 1,
                lambda rng, scene: choose_group(rng, scene, fresh),
                group_effect,
                _group_request,
                _learn_group,
            ),
            ("object.group",),
        ),
        "group_rename": (
            one(
                "group_rename",
                1,
                lambda scene: bool(live_groups(scene)),
                lambda rng, scene: {
                    "group": pick_group(rng, scene),
                    "name": fresh(rng, scene, "R"),
                },
                group_rename_effect,
                _group_rename_request,
                nothing,
            ),
            ("object.group.rename",),
        ),
        "group_ungroup": (
            one(
                "group_ungroup",
                1,
                lambda scene: bool(live_groups(scene)),
                lambda rng, scene: {"group": pick_group(rng, scene)},
                ungroup_effect,
                _ungroup_request,
                nothing,
            ),
            ("object.ungroup",),
        ),
    }
