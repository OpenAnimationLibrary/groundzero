"""An independent model of what the draft tools do to settings, cells and curves.

The model is written from the tools' documented behavior and from nothing the worker returns:
it computes the cell map, the key curves and their samples, and the scene and column settings
from the operations alone. `mcp_property_cases.py` generates sequences of operations from a
seed, replays them against the worker and compares what the worker reads back with
`expected`.

Every operation is one entry of `OPERATIONS`. An entry says when the operation can be chosen
(`available`), how to choose its arguments (`choose`), what it does to the model (`effect`,
returning the error code a correct worker answers, or None), and which tool call carries it
out (`request`, with `learn` to remember the handles the answer names). A tool that is added
to the worker is added to the model by one more entry.
"""

from __future__ import annotations

import math
import random
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any

from mcp_property_curves import (
    FRAME_GRID,
    STEPS_THAT_FIT,
    Key,
    duplicate_keys,
    insert_breakdown,
    is_rich,
    key_from_row,
    key_record,
    key_row,
    move_keys,
    ordered,
    random_keys,
    sample,
    shape_is_defined,
)

Drawing = tuple[int, str]
Step = dict[str, Any]
Handles = dict[str, dict[str, str]]
Request = tuple[str, dict[str, Any]]

CHANNEL_UNITS = {"x": "inch", "y": "inch", "rotation": "degree", "scale": "scalar"}
SAMPLE_FRAMES = [0, 0.5, 3, 5.99, 6, 8, 11.9, 12, 17.5, 30, 43, 55.25, 100, 133.5, 250]
CELL_HORIZON = 48
OPACITY_LEVELS = 255
EPSILON = 1e-9
FRAME_RATES = (12, 24, 25, 29.97, 30, 48, 60)
PIXELS = (16, 24, 32, 48, 64, 96, 128, 256)
INCHES = (0.125, 0.25, 0.5, 1, 1.5, 2.5, 4)
SUFFIXES = ("", "a", "b", "ab", "zz", "abcdefgh")
NAME_ALPHABET = "abcXYZ019 _-.<>&\"'中é "


class ModelError(ValueError):
    """A step that cannot be applied to the model's state, as in a hand-edited sequence."""


@dataclass
class Column:
    """A level column: its settings and its cells, keyed by frame."""

    name: str
    visible: bool = True
    locked: bool = False
    opacity: int = OPACITY_LEVELS
    cells: dict[int, tuple[str, Drawing]] = field(default_factory=dict)


@dataclass
class Scene:
    """What a draft scene holds, as far as the modelled tools can change it."""

    frame_rate: float
    camera: dict[str, float]
    levels: dict[str, set[Drawing]] = field(default_factory=dict)
    columns: list[Column] = field(default_factory=list)
    pegs: list[str] = field(default_factory=list)
    curves: dict[tuple[str, str], dict[int, Key]] = field(default_factory=dict)
    cycles: set[tuple[str, str]] = field(default_factory=set)
    reopens: int = 0
    counter: int = 0

    def column(self, name: str) -> Column:
        """Return the column called `name`."""
        for column in self.columns:
            if column.name == name:
                return column
        raise ModelError(f"no column named {name!r}")

    def fresh_name(self, prefix: str) -> str:
        """Return a name no level, column or peg has had in this scene."""
        self.counter += 1
        return f"{prefix}{self.counter}"


def new_scene(frame_rate: float = 24, size_px: int = 32, size_in: float = 0.25) -> Scene:
    """Return the scene `scene.create` makes with these explicit settings."""
    camera = {"width_px": size_px, "height_px": size_px, "width_in": size_in, "height_in": size_in}
    return Scene(frame_rate=frame_rate, camera=camera)


def quantized_opacity(value: float) -> int:
    """Return the 8-bit step a column's opacity is stored as, rounding half up."""
    return math.floor(value * OPACITY_LEVELS + 0.5)


def expected(scene: Scene) -> dict[str, Any]:
    """Return the observation a correct worker's read tools give for this scene."""
    columns = []
    for column in scene.columns:
        rows = [column.cells.get(frame) for frame in range(CELL_HORIZON)]
        columns.append(
            {
                "name": column.name,
                "visible": column.visible,
                "locked": column.locked,
                "opacity": column.opacity / OPACITY_LEVELS,
                "rows": [None if row is None else [row[0], row[1][0], row[1][1]] for row in rows],
            }
        )
    last = max((max(c.cells) + 1 for c in scene.columns if c.cells), default=0)
    curves = {}
    for (peg, channel), keys in scene.curves.items():
        if not keys:
            continue
        cycle = (peg, channel) in scene.cycles
        items = ordered(keys)
        defined = shape_is_defined(keys)
        curves[f"{peg}/{channel}"] = {
            "keys": [key_row(key) for key in items],
            "cycle": cycle,
            "samples": [sample(items, frame, cycle) for frame in SAMPLE_FRAMES]
            if defined
            else None,
            "rich": is_rich(keys),
        }
    return {
        "frame_rate": scene.frame_rate,
        "camera": dict(scene.camera),
        "frame_count": last,
        "levels": {name: sorted(drawings) for name, drawings in scene.levels.items()},
        "columns": columns,
        "pegs": sorted(scene.pegs),
        "curves": curves,
    }


def after_reopen(scene: Scene) -> None:
    """Apply what a saved scene loses by native design: trailing columns that hold no cell."""
    while scene.columns and not scene.columns[-1].cells:
        scene.columns.pop()


@dataclass(frozen=True)
class Operation:
    """One operation of the model and the tool call that carries it out."""

    name: str
    weight: int
    available: Callable[[Scene], bool]
    choose: Callable[[random.Random, Scene], Step]
    effect: Callable[[Scene, Step], str | None]
    request: Callable[[Step, Handles], Request]
    learn: Callable[[Step, dict[str, Any], Handles], None] = lambda step, result, handles: None
    reopens: bool = False


def _pick(rng: random.Random, options: Any) -> Any:
    return rng.choice(sorted(options) if isinstance(options, set) else list(options))


def _name(rng: random.Random, scene: Scene, prefix: str) -> str:
    """A fresh, unique name that sometimes carries characters a serializer must escape."""
    tail = "".join(rng.choice(NAME_ALPHABET) for _ in range(rng.randint(0, 6)))
    return f"{scene.fresh_name(prefix)}{tail}"


def _used_names(scene: Scene) -> set[str]:
    return set(scene.levels) | {column.name for column in scene.columns} | set(scene.pegs)


def _settings_choice(rng: random.Random, scene: Scene) -> Step:
    options: dict[str, Any] = {
        "frame_rate": FRAME_RATES,
        "width_px": PIXELS,
        "height_px": PIXELS,
        "width_in": INCHES,
        "height_in": INCHES,
    }
    fields = rng.sample(sorted(options), rng.randint(1, 3))
    return {name: _pick(rng, options[name]) for name in fields}


def _settings_effect(scene: Scene, step: Step) -> str | None:
    for name, value in step.items():
        if name == "frame_rate":
            scene.frame_rate = value
        else:
            scene.camera[name] = value
    return None


def _camera_choice(rng: random.Random, scene: Scene) -> Step:
    step = _settings_choice(rng, scene)
    step.pop("frame_rate", None)
    return step or {"width_px": _pick(rng, PIXELS)}


def _level_effect(scene: Scene, step: Step) -> str | None:
    if step["name"] in scene.levels:
        raise ModelError(f"level {step['name']!r} exists")
    scene.levels[step["name"]] = set()
    return None


def _drawing_choice(rng: random.Random, scene: Scene) -> Step:
    level = _pick(rng, [n for n, d in scene.levels.items() if len(d) < 6])
    taken = scene.levels[level]
    while True:
        drawing = (rng.randint(1, 30), _pick(rng, SUFFIXES))
        if drawing not in taken:
            return {"level": level, "number": drawing[0], "suffix": drawing[1]}


def _drawing_effect(scene: Scene, step: Step) -> str | None:
    drawing = (step["number"], step["suffix"])
    if step["level"] not in scene.levels or drawing in scene.levels[step["level"]]:
        raise ModelError(f"cannot create drawing {drawing} in {step['level']!r}")
    scene.levels[step["level"]].add(drawing)
    return None


def _column_effect(scene: Scene, step: Step) -> str | None:
    if step["name"] in {column.name for column in scene.columns}:
        raise ModelError(f"column {step['name']!r} exists")
    scene.columns.append(Column(step["name"]))
    return None


def _update_choice(rng: random.Random, scene: Scene) -> Step:
    column = _pick(rng, scene.columns)
    if column.locked and rng.random() < 0.6:
        return {"column": column.name, "locked": False}
    fields: dict[str, Any] = {
        "visible": rng.random() < 0.5,
        "locked": rng.random() < 0.2,
        "opacity": rng.randint(0, 20) / 20,
        "name": _name(rng, scene, "C"),
    }
    chosen = rng.sample(sorted(fields), rng.randint(1, 3))
    return {"column": column.name, **{name: fields[name] for name in chosen}}


def _update_effect(scene: Scene, step: Step) -> str | None:
    column = scene.column(step["column"])
    changes = {name: value for name, value in step.items() if name != "column"}
    if column.locked and set(changes) != {"locked"}:
        return "locked"
    renamed = changes.get("name", column.name)
    if renamed != column.name and renamed in {other.name for other in scene.columns}:
        raise ModelError(f"column {renamed!r} exists")
    column.name = renamed
    column.visible = changes.get("visible", column.visible)
    column.locked = changes.get("locked", column.locked)
    if "opacity" in changes:
        column.opacity = quantized_opacity(changes["opacity"])
    return None


def _delete_effect(scene: Scene, step: Step) -> str | None:
    column = scene.column(step["column"])
    if column.locked:
        return "locked"
    scene.columns.remove(column)
    return None


def _drawn_levels(scene: Scene) -> list[str]:
    return [name for name, drawings in scene.levels.items() if drawings]


def _expose_choice(rng: random.Random, scene: Scene) -> Step:
    level = _pick(rng, _drawn_levels(scene))
    number, suffix = _pick(rng, scene.levels[level])
    return {
        "column": _pick(rng, scene.columns).name,
        "level": level,
        "number": number,
        "suffix": suffix,
        "start": rng.randint(0, 30),
        "length": rng.randint(1, 6),
        "overwrite": rng.random() < 0.4,
    }


def _expose_effect(scene: Scene, step: Step) -> str | None:
    column = scene.column(step["column"])
    drawing = (step["number"], step["suffix"])
    if drawing not in scene.levels.get(step["level"], set()):
        raise ModelError(f"no drawing {drawing} in {step['level']!r}")
    frames = range(step["start"], step["start"] + step["length"])
    if column.locked:
        return "locked"
    if not step["overwrite"] and any(frame in column.cells for frame in frames):
        return "occupied_cells"
    for frame in frames:
        column.cells[frame] = (step["level"], drawing)
    return None


def _peg_effect(scene: Scene, step: Step) -> str | None:
    if step["name"] in scene.pegs:
        raise ModelError(f"peg {step['name']!r} exists")
    scene.pegs.append(step["name"])
    return None


def _plain_curves(scene: Scene) -> list[tuple[str, str]]:
    """The (peg, channel) pairs whose curves hold no speed or ease segment."""
    return [
        (peg, channel)
        for peg in scene.pegs
        for channel in CHANNEL_UNITS
        if not is_rich(scene.curves.get((peg, channel), {}))
    ]


def _keys_choice(rng: random.Random, scene: Scene) -> Step:
    peg, channel = _pick(rng, _plain_curves(scene))
    keys = []
    for index in rng.sample(range(8), rng.randint(1, 3)):
        keys.append(
            {
                "frame": index * FRAME_GRID,
                "value": rng.randint(-200, 200) / 4,
                "interpolation": _pick(rng, ("linear", "constant")),
                "step": _pick(rng, STEPS_THAT_FIT),
            }
        )
    return {"peg": peg, "channel": channel, "keys": keys}


def _keys_effect(scene: Scene, step: Step) -> str | None:
    if step["peg"] not in scene.pegs:
        raise ModelError(f"no peg {step['peg']!r}")
    curve = scene.curves.setdefault((step["peg"], step["channel"]), {})
    for key in step["keys"]:
        curve[key["frame"]] = Key(key["frame"], key["value"], key["interpolation"], key["step"])
    return None


def _delete_keys_choice(rng: random.Random, scene: Scene) -> Step:
    plain = [name for name, keys in scene.curves.items() if keys and not is_rich(keys)]
    peg, channel = _pick(rng, plain)
    frames = sorted(scene.curves[(peg, channel)])
    return {
        "peg": peg,
        "channel": channel,
        "frames": rng.sample(frames, rng.randint(1, len(frames))),
    }


def _delete_keys_effect(scene: Scene, step: Step) -> str | None:
    curve = scene.curves.get((step["peg"], step["channel"]), {})
    if not set(step["frames"]) <= set(curve):
        raise ModelError("a frame to delete has no key")
    for frame in step["frames"]:
        del curve[frame]
    return None


def _save_effect(scene: Scene, step: Step) -> str | None:
    if any(not drawings for drawings in scene.levels.values()):
        return "empty_level"
    scene.reopens += 1
    after_reopen(scene)
    return None


def _create_request(tool: str, params: Callable[[Step], dict[str, Any]]) -> Callable[..., Request]:
    return lambda step, handles: (tool, params(step))


def _expose_request(step: Step, handles: Handles) -> Request:
    return "column.expose", {
        "column_id": handles["column"][step["column"]],
        "level_id": handles["level"][step["level"]],
        "drawing_id": {"number": step["number"], "suffix": step["suffix"]},
        "start_frame": step["start"],
        "length": step["length"],
        "overwrite": step["overwrite"],
    }


def _update_request(step: Step, handles: Handles) -> Request:
    changes = {name: value for name, value in step.items() if name != "column"}
    return "column.update", {"column_id": handles["column"][step["column"]], **changes}


def _curve_target(step: Step, handles: Handles) -> dict[str, Any]:
    return {
        "object_id": handles["peg"][step["peg"]],
        "channel": step["channel"],
        "unit": CHANNEL_UNITS[step["channel"]],
    }


def _keys_request(step: Step, handles: Handles) -> Request:
    return "animation.set_keys", {**_curve_target(step, handles), "keys": step["keys"]}


def _delete_keys_request(step: Step, handles: Handles) -> Request:
    return "animation.delete_keys", {**_curve_target(step, handles), "frames": step["frames"]}


def _learn(kind: str, key: str, field_name: str) -> Callable[..., None]:
    def learn(step: Step, result: dict[str, Any], handles: Handles) -> None:
        handles[kind][step[key]] = result[field_name]

    return learn


def _learn_rename(step: Step, result: dict[str, Any], handles: Handles) -> None:
    if "name" in step:
        handles["column"][step["name"]] = handles["column"].pop(step["column"])


def _forget_column(step: Step, result: dict[str, Any], handles: Handles) -> None:
    del handles["column"][step["column"]]


def _chosen_pegs(scene: Scene) -> bool:
    return any(keys and not is_rich(keys) for keys in scene.curves.values())


def _empty_curves(scene: Scene) -> list[tuple[str, str]]:
    return [
        (peg, channel)
        for peg in scene.pegs
        for channel in CHANNEL_UNITS
        if not scene.curves.get((peg, channel))
    ]


def _keyed_curves(scene: Scene, minimum: int = 1) -> list[tuple[str, str]]:
    return [name for name, keys in scene.curves.items() if len(keys) >= minimum]


def _rich_choice(rng: random.Random, scene: Scene) -> Step:
    peg, channel = _pick(rng, _empty_curves(scene))
    return {"peg": peg, "channel": channel, "keys": [key_row(k) for k in random_keys(rng)]}


def _rich_effect(scene: Scene, step: Step) -> str | None:
    if step["peg"] not in scene.pegs or scene.curves.get((step["peg"], step["channel"])):
        raise ModelError("a rich curve is set on a channel that holds no key")
    keys = [key_from_row(row) for row in step["keys"]]
    scene.curves[(step["peg"], step["channel"])] = {key.frame: key for key in keys}
    return None


def _rich_request(step: Step, handles: Handles) -> Request:
    records = [key_record(key_from_row(row)) for row in step["keys"]]
    return "animation.set_keys", {**_curve_target(step, handles), "keys": records}


def _cycle_choice(rng: random.Random, scene: Scene) -> Step:
    peg, channel = _pick(rng, _keyed_curves(scene))
    return {"peg": peg, "channel": channel, "enabled": rng.random() < 0.7}


def _cycle_effect(scene: Scene, step: Step) -> str | None:
    pair = (step["peg"], step["channel"])
    if not scene.curves.get(pair):
        raise ModelError("a curve without keys has no cycle to set")
    (scene.cycles.add if step["enabled"] else scene.cycles.discard)(pair)
    return None


def _cycle_request(step: Step, handles: Handles) -> Request:
    return "animation.set_cycle", {**_curve_target(step, handles), "enabled": step["enabled"]}


def _key_frames(rng: random.Random, keys: dict[int, Key]) -> list[int]:
    frames = sorted(keys)
    chosen = rng.sample(frames, rng.randint(1, len(frames)))
    return sorted(chosen)


def _shift_choice(rng: random.Random, scene: Scene, duplicate: bool) -> Step:
    peg, channel = _pick(rng, _keyed_curves(scene))
    keys = scene.curves[(peg, channel)]
    shift = duplicate_keys if duplicate else move_keys
    step: Step = {"peg": peg, "channel": channel, "frames": [min(keys)]}
    step.update(frame_offset=0, value_offset=0)
    for _ in range(8):
        frames = _key_frames(rng, keys)
        frame_offset = FRAME_GRID * rng.randint(-4, 4)
        value_offset = rng.choice([0, 0, rng.randint(-160, 160) / 4])
        error, edited = shift(keys, frames, frame_offset, value_offset)
        clear = error is None and shape_is_defined(edited)
        refused = error is not None and "controls" not in error
        if clear or refused:
            step.update(frames=frames, frame_offset=frame_offset, value_offset=value_offset)
            break
    return _hostile(rng, step, keys)


def _hostile(rng: random.Random, step: Step, keys: dict[int, Key]) -> Step:
    """Sometimes ask for what no worker may do: an absent key, an offset past a bound."""
    roll = rng.random()
    if roll < 0.04:
        step["frames"] = sorted({*step["frames"], max(keys) + 1})
    elif roll < 0.07:
        step["frame_offset"] = -1000000
    elif roll < 0.1:
        step["value_offset"] = 2000000
    return step


def _shift_effect(scene: Scene, step: Step, duplicate: bool) -> str | None:
    pair = (step["peg"], step["channel"])
    keys = scene.curves.get(pair)
    if not keys:
        raise ModelError("no keys to shift")
    shift = duplicate_keys if duplicate else move_keys
    error, edited = shift(keys, step["frames"], step["frame_offset"], step["value_offset"])
    if error is None:
        scene.curves[pair] = edited
    return error


def _shift_request(tool: str) -> Callable[[Step, Handles], Request]:
    def request(step: Step, handles: Handles) -> Request:
        fields = {key: step[key] for key in ("frames", "frame_offset", "value_offset")}
        return tool, {**_curve_target(step, handles), **fields}

    return request


def _breakdown_choice(rng: random.Random, scene: Scene) -> Step:
    peg, channel = _pick(rng, _keyed_curves(scene, 2))
    keys = scene.curves[(peg, channel)]
    items = ordered(keys)
    index = rng.randrange(len(items) - 1)
    first, last = items[index], items[index + 1]
    stride = 1 if first.interpolation in ("speed_in_out", "ease_in_out") else first.step
    frame = first.frame + stride * rng.randint(1, (last.frame - first.frame) // stride)
    roll = rng.random()
    if roll < 0.06:
        frame = rng.choice(list(keys))
    elif roll < 0.1:
        frame = items[-1].frame + rng.randint(1, 5)
    step: Step = {"peg": peg, "channel": channel, "frame": frame}
    if rng.random() < 0.6:
        step["favour_percent"] = rng.randint(-20, 40) * 5
    else:
        step["value"] = rng.randint(-400, 400) / 4
    return step


def _breakdown_effect(scene: Scene, step: Step) -> str | None:
    pair = (step["peg"], step["channel"])
    keys = scene.curves.get(pair)
    if not keys:
        raise ModelError("no keys to split")
    error, edited = insert_breakdown(
        keys, step["frame"], step.get("favour_percent"), step.get("value")
    )
    if error is None:
        scene.curves[pair] = edited
    return error


def _breakdown_request(step: Step, handles: Handles) -> Request:
    fields = {key: step[key] for key in ("frame", "favour_percent", "value") if key in step}
    return "animation.insert_breakdown", {**_curve_target(step, handles), **fields}


OPERATIONS: dict[str, Operation] = {
    "scene_settings": Operation(
        "scene_settings",
        2,
        lambda scene: True,
        _settings_choice,
        _settings_effect,
        _create_request("scene.settings", dict),
    ),
    "camera_update": Operation(
        "camera_update",
        2,
        lambda scene: True,
        _camera_choice,
        _settings_effect,
        _create_request("camera.update", dict),
    ),
    "level_create": Operation(
        "level_create",
        4,
        lambda scene: len(scene.levels) < 4,
        lambda rng, scene: {"name": _name(rng, scene, "L")},
        _level_effect,
        _create_request("level.create", lambda step: {"name": step["name"], "format": "vector"}),
        _learn("level", "name", "level_id"),
    ),
    "drawing_create": Operation(
        "drawing_create",
        8,
        lambda scene: any(len(d) < 6 for d in scene.levels.values()),
        _drawing_choice,
        _drawing_effect,
        lambda step, handles: (
            "drawing.create",
            {
                "level_id": handles["level"][step["level"]],
                "drawing_id": {"number": step["number"], "suffix": step["suffix"]},
                "format": "vector",
            },
        ),
    ),
    "column_create": Operation(
        "column_create",
        4,
        lambda scene: len(scene.columns) < 4,
        lambda rng, scene: {"name": _name(rng, scene, "C")},
        _column_effect,
        _create_request("column.create", lambda step: {"name": step["name"]}),
        _learn("column", "name", "column_id"),
    ),
    "column_update": Operation(
        "column_update",
        6,
        lambda scene: bool(scene.columns),
        _update_choice,
        _update_effect,
        _update_request,
        _learn_rename,
    ),
    "column_expose": Operation(
        "column_expose",
        14,
        lambda scene: bool(scene.columns) and bool(_drawn_levels(scene)),
        _expose_choice,
        _expose_effect,
        _expose_request,
    ),
    "column_delete": Operation(
        "column_delete",
        2,
        lambda scene: any(not column.cells for column in scene.columns),
        lambda rng, scene: {"column": _pick(rng, [c for c in scene.columns if not c.cells]).name},
        _delete_effect,
        lambda step, handles: ("column.delete", {"column_id": handles["column"][step["column"]]}),
        _forget_column,
    ),
    "peg_create": Operation(
        "peg_create",
        3,
        lambda scene: len(scene.pegs) < 3,
        lambda rng, scene: {"name": _name(rng, scene, "P")},
        _peg_effect,
        _create_request("object.create", lambda step: {"name": step["name"], "kind": "pegbar"}),
        _learn("peg", "name", "object_id"),
    ),
    "keys_set": Operation(
        "keys_set",
        10,
        lambda scene: bool(_plain_curves(scene)),
        _keys_choice,
        _keys_effect,
        _keys_request,
    ),
    "keys_delete": Operation(
        "keys_delete",
        3,
        _chosen_pegs,
        _delete_keys_choice,
        _delete_keys_effect,
        _delete_keys_request,
    ),
    "curve_rich": Operation(
        "curve_rich",
        4,
        lambda scene: bool(scene.pegs) and bool(_empty_curves(scene)),
        _rich_choice,
        _rich_effect,
        _rich_request,
    ),
    "cycle_set": Operation(
        "cycle_set",
        3,
        lambda scene: bool(_keyed_curves(scene)),
        _cycle_choice,
        _cycle_effect,
        _cycle_request,
    ),
    "keys_move": Operation(
        "keys_move",
        4,
        lambda scene: bool(_keyed_curves(scene)),
        lambda rng, scene: _shift_choice(rng, scene, False),
        lambda scene, step: _shift_effect(scene, step, False),
        _shift_request("animation.move_keys"),
    ),
    "keys_duplicate": Operation(
        "keys_duplicate",
        3,
        lambda scene: bool(_keyed_curves(scene)),
        lambda rng, scene: _shift_choice(rng, scene, True),
        lambda scene, step: _shift_effect(scene, step, True),
        _shift_request("animation.duplicate_keys"),
    ),
    "breakdown_insert": Operation(
        "breakdown_insert",
        4,
        lambda scene: bool(_keyed_curves(scene, 2)),
        _breakdown_choice,
        _breakdown_effect,
        _breakdown_request,
    ),
    "save_reopen": Operation(
        "save_reopen",
        3,
        lambda scene: scene.reopens < 3,
        lambda rng, scene: {"directory": f"package-{scene.reopens + 1}"},
        _save_effect,
        _create_request("scene.save_copy", lambda step: {"directory": step["directory"]}),
        reopens=True,
    ),
}


# Writing tools of the modelled families that no operation above carries out yet, each with
# the reason. Generated sequences never call them, so nothing here is checked by the model:
# their own cases are their only evidence until an operation replaces the entry.
NOT_MODELLED: dict[str, str] = {
    "cells.set": "not written yet",
    "cells.clear": "not written yet",
    "cells.remove": "not written yet",
    "cells.insert": "not written yet",
    "cells.copy": "not written yet",
    "cells.retime": "not written yet",
    "cells.replace": "not written yet",
}


def generate(seed: int, length: int) -> list[Step]:
    """Return `length` operations chosen from `seed`, each valid for the state before it.

    The same seed and length always give the same sequence. A step is
    `{"op": name, "args": {...}}`; replaying a prefix needs no random numbers.
    """
    rng = random.Random(seed)
    scene = new_scene()
    steps: list[Step] = []
    for _ in range(length):
        candidates = [op for op in OPERATIONS.values() if op.available(scene)]
        operation = rng.choices(candidates, [op.weight for op in candidates])[0]
        args = operation.choose(rng, scene)
        steps.append({"op": operation.name, "args": args})
        operation.effect(scene, args)
    return steps
