"""Effect channels for the property model: key edits on a blur's value within its bounds.

An effect parameter channel takes the same key edits as a stage channel, through the same
tools with `fx_id` in place of `object_id`, and so follows the same curve model
(`mcp_property_curves`). What differs is the admitted range: every key value, and both handle
points of every speed segment, must lie in the parameter's bounds, or the write is refused as
`OUT_OF_RANGE` and nothing changes. An ease or constant segment stays between its key values and
a speed segment inside the hull of its four points, so no sample is needed to decide.

The operations here extend `mcp_property_model.OPERATIONS`: `generate_effects` chooses from both
the shared save-and-reopen operation and these, so effect sequences are seeded on their own and
the stage sequences keep their seeds.
"""

from __future__ import annotations

import random
from typing import Any

from mcp_property_curves import (
    FRAME_GRID,
    STEPS_THAT_FIT,
    Key,
    duplicate_keys,
    incoming_of,
    insert_breakdown,
    is_rich,
    key_from_row,
    key_record,
    key_row,
    move_keys,
    ordered,
    random_keys,
    shape_is_defined,
)
from mcp_property_model import (
    OPERATIONS,
    Handles,
    ModelError,
    Operation,
    Request,
    Scene,
    Step,
    new_scene,
)

FX_TYPE = "STD_blurFx"
FX_CHANNEL = "value"
FX_UNIT = "native_fxLength"
LOW, HIGH = 0.0, 200.0
OUT_OF_RANGE = "OUT_OF_RANGE"
MOST_EFFECTS = 2


def in_bounds(keys: dict[int, Key]) -> bool:
    """Whether every key value and every speed handle point lies in the admitted bounds."""
    items = ordered(keys)
    points = [key.value for key in items]
    for first, last in zip(items, items[1:], strict=False):
        if first.interpolation == "speed_in_out" and first.outgoing is not None:
            points.append(first.value + first.outgoing[2])
            points.append(last.value + incoming_of(first, last)[2])
    return all(LOW <= point <= HIGH for point in points)


def _settle(scene: Scene, owner: str, error: str | None, edited: dict[int, Key]) -> str | None:
    """Apply an edit the curve rules allow, unless it leaves the bounds."""
    if error is not None:
        return error
    if not in_bounds(edited):
        return OUT_OF_RANGE
    scene.curves[(owner, FX_CHANNEL)] = edited
    return None


def _curves(scene: Scene, minimum: int = 1, rich: bool | None = None) -> list[str]:
    """The effects whose curve holds at least `minimum` keys (and is or is not rich)."""
    found = []
    for name in scene.effects:
        keys = scene.curves.get((name, FX_CHANNEL), {})
        if len(keys) >= minimum and (rich is None or is_rich(keys) == rich):
            found.append(name)
    return found


def _value(rng: random.Random) -> float:
    """A key value: inside the bounds, sometimes just outside or far beyond them."""
    roll = rng.random()
    if roll < 0.04:
        return rng.choice((LOW - 0.25, HIGH + 0.25, HIGH + 60))
    return rng.randint(int(LOW) * 4, int(HIGH) * 4) / 4


def _target(step: Step, handles: Handles) -> dict[str, Any]:
    return {"fx_id": handles["fx"][step["fx"]], "channel": FX_CHANNEL, "unit": FX_UNIT}


def _create_effect(scene: Scene, step: Step) -> str | None:
    if step["name"] in scene.effects:
        raise ModelError(f"effect {step['name']!r} exists")
    scene.effects.append(step["name"])
    return None


def _learn_effect(step: Step, result: dict[str, Any], handles: Handles) -> None:
    handles["fx"][step["name"]] = result["fx_id"]


def _keys_choice(rng: random.Random, scene: Scene) -> Step:
    keys = []
    for index in rng.sample(range(8), rng.randint(1, 3)):
        keys.append(
            {
                "frame": index * FRAME_GRID,
                "value": _value(rng),
                "interpolation": rng.choice(("linear", "constant")),
                "step": rng.choice(STEPS_THAT_FIT),
            }
        )
    return {"fx": rng.choice(_curves(scene, 0, rich=False)), "keys": keys}


def _keys_effect(scene: Scene, step: Step) -> str | None:
    if step["fx"] not in scene.effects:
        raise ModelError(f"no effect {step['fx']!r}")
    edited = dict(scene.curves.get((step["fx"], FX_CHANNEL), {}))
    for key in step["keys"]:
        edited[key["frame"]] = Key(key["frame"], key["value"], key["interpolation"], key["step"])
    return _settle(scene, step["fx"], None, edited)


def _keys_request(step: Step, handles: Handles) -> Request:
    return "animation.set_keys", {**_target(step, handles), "keys": step["keys"]}


def _rich_choice(rng: random.Random, scene: Scene) -> Step:
    empty = [name for name in scene.effects if not scene.curves.get((name, FX_CHANNEL))]
    keys = random_keys(rng)
    for key in keys:
        key.value += (LOW + HIGH) / 2
    return {"fx": rng.choice(empty), "keys": [key_row(key) for key in keys]}


def _rich_effect(scene: Scene, step: Step) -> str | None:
    if step["fx"] not in scene.effects or scene.curves.get((step["fx"], FX_CHANNEL)):
        raise ModelError("a rich curve is set on an effect channel that holds no key")
    keys = [key_from_row(row) for row in step["keys"]]
    return _settle(scene, step["fx"], None, {key.frame: key for key in keys})


def _rich_request(step: Step, handles: Handles) -> Request:
    records = [key_record(key_from_row(row)) for row in step["keys"]]
    return "animation.set_keys", {**_target(step, handles), "keys": records}


def _delete_choice(rng: random.Random, scene: Scene) -> Step:
    name = rng.choice(_curves(scene, rich=False))
    frames = sorted(scene.curves[(name, FX_CHANNEL)])
    return {"fx": name, "frames": rng.sample(frames, rng.randint(1, len(frames)))}


def _delete_effect(scene: Scene, step: Step) -> str | None:
    curve = scene.curves.get((step["fx"], FX_CHANNEL), {})
    if not set(step["frames"]) <= set(curve):
        raise ModelError("a frame to delete has no key")
    for frame in step["frames"]:
        del curve[frame]
    return None


def _delete_request(step: Step, handles: Handles) -> Request:
    return "animation.delete_keys", {**_target(step, handles), "frames": step["frames"]}


def _shift_choice(rng: random.Random, scene: Scene, duplicate: bool) -> Step:
    name = rng.choice(_curves(scene))
    keys = scene.curves[(name, FX_CHANNEL)]
    shift = duplicate_keys if duplicate else move_keys
    step: Step = {"fx": name, "frames": [min(keys)], "frame_offset": 0, "value_offset": 0}
    for _ in range(8):
        frames = sorted(rng.sample(sorted(keys), rng.randint(1, len(keys))))
        frame_offset = FRAME_GRID * rng.randint(-4, 4)
        value_offset = rng.choice([0, 0, rng.randint(-160, 160) / 4])
        error, edited = shift(keys, frames, frame_offset, value_offset)
        if (error is None and shape_is_defined(edited)) or (
            error is not None and "controls" not in error
        ):
            step.update(frames=frames, frame_offset=frame_offset, value_offset=value_offset)
            break
    if rng.random() < 0.05:
        step["value_offset"] = 2000000
    return step


def _shift_effect(scene: Scene, step: Step, duplicate: bool) -> str | None:
    keys = scene.curves.get((step["fx"], FX_CHANNEL))
    if not keys:
        raise ModelError("no effect keys to shift")
    shift = duplicate_keys if duplicate else move_keys
    error, edited = shift(keys, step["frames"], step["frame_offset"], step["value_offset"])
    return _settle(scene, step["fx"], error, edited)


def _shift_request(tool: str) -> Any:
    def request(step: Step, handles: Handles) -> Request:
        fields = {key: step[key] for key in ("frames", "frame_offset", "value_offset")}
        return tool, {**_target(step, handles), **fields}

    return request


def _breakdown_choice(rng: random.Random, scene: Scene) -> Step:
    name = rng.choice(_curves(scene, 2))
    items = ordered(scene.curves[(name, FX_CHANNEL)])
    index = rng.randrange(len(items) - 1)
    first, last = items[index], items[index + 1]
    stride = 1 if first.interpolation in ("speed_in_out", "ease_in_out") else first.step
    if last.frame - first.frame < stride:
        stride = 1
    frame = first.frame + stride * rng.randint(1, (last.frame - first.frame) // stride)
    if rng.random() < 0.06:
        frame = first.frame
    step: Step = {"fx": name, "frame": frame}
    if rng.random() < 0.6:
        step["favour_percent"] = rng.randint(-20, 40) * 5
    else:
        step["value"] = _value(rng)
    return step


def _breakdown_effect(scene: Scene, step: Step) -> str | None:
    keys = scene.curves.get((step["fx"], FX_CHANNEL))
    if not keys:
        raise ModelError("no effect keys to split")
    error, edited = insert_breakdown(
        keys, step["frame"], step.get("favour_percent"), step.get("value")
    )
    return _settle(scene, step["fx"], error, edited)


def _breakdown_request(step: Step, handles: Handles) -> Request:
    fields = {key: step[key] for key in ("frame", "favour_percent", "value") if key in step}
    return "animation.insert_breakdown", {**_target(step, handles), **fields}


EFFECT_OPERATIONS: dict[str, Operation] = {
    "fx_create": Operation(
        "fx_create",
        3,
        lambda scene: len(scene.effects) < MOST_EFFECTS,
        lambda rng, scene: {"name": scene.fresh_name("F")},
        _create_effect,
        lambda step, handles: ("fx.create", {"type": FX_TYPE, "name": step["name"]}),
        _learn_effect,
    ),
    "fx_keys_set": Operation(
        "fx_keys_set",
        8,
        lambda scene: bool(_curves(scene, 0, rich=False)),
        _keys_choice,
        _keys_effect,
        _keys_request,
    ),
    "fx_curve_rich": Operation(
        "fx_curve_rich",
        5,
        lambda scene: any(not scene.curves.get((n, FX_CHANNEL)) for n in scene.effects),
        _rich_choice,
        _rich_effect,
        _rich_request,
    ),
    "fx_keys_delete": Operation(
        "fx_keys_delete",
        3,
        lambda scene: bool(_curves(scene, rich=False)),
        _delete_choice,
        _delete_effect,
        _delete_request,
    ),
    "fx_keys_move": Operation(
        "fx_keys_move",
        5,
        lambda scene: bool(_curves(scene)),
        lambda rng, scene: _shift_choice(rng, scene, False),
        lambda scene, step: _shift_effect(scene, step, False),
        _shift_request("animation.move_keys"),
    ),
    "fx_keys_duplicate": Operation(
        "fx_keys_duplicate",
        4,
        lambda scene: bool(_curves(scene)),
        lambda rng, scene: _shift_choice(rng, scene, True),
        lambda scene, step: _shift_effect(scene, step, True),
        _shift_request("animation.duplicate_keys"),
    ),
    "fx_breakdown_insert": Operation(
        "fx_breakdown_insert",
        5,
        lambda scene: bool(_curves(scene, 2)),
        _breakdown_choice,
        _breakdown_effect,
        _breakdown_request,
    ),
    "save_reopen": OPERATIONS["save_reopen"],
}


def generate_effects(seed: int, length: int) -> list[Step]:
    """Return `length` effect-channel operations chosen from `seed`, as `generate` does."""
    rng = random.Random(seed)
    scene = new_scene()
    steps: list[Step] = []
    for _ in range(length):
        candidates = [op for op in EFFECT_OPERATIONS.values() if op.available(scene)]
        operation = rng.choices(candidates, [op.weight for op in candidates])[0]
        args = operation.choose(rng, scene)
        steps.append({"op": operation.name, "args": args})
        operation.effect(scene, args)
    return steps
