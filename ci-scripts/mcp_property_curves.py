"""Curves for the property model: keys with handles, segment kinds, the cyclic fold, key edits.

A curve is a set of keys. A key has a frame, a value, the interpolation of the segment that starts
at it (`linear`, `constant`, `speed_in_out` or `ease_in_out`) and its step; a key that ends a
speed or ease segment carries that segment's incoming control, and a key that starts one carries
its outgoing control. Everything here is computed from those definitions alone:

- a speed segment is the cubic Bezier through its two keys whose other control points are the
  outgoing and incoming speed handles, read as offsets in (frame, value);
- an ease segment moves with a trapezoid of speed: it rises from rest over the outgoing ease
  length, holds, and falls to rest over the incoming ease length, covering the value difference;
- a linear segment is the straight line, held for `step` frames at a time when the step is above
  one; a constant segment holds its first key's value;
- a cyclic curve repeats its key span after the last key, adding the last-minus-first value each
  time.

The key edits (breakdown, move, duplicate) return the error a correct worker answers, as
`invalid_argument/<reason>` or a plain code, or the edited keys.
"""

from __future__ import annotations

import dataclasses
import math
import random
from dataclasses import dataclass
from typing import Any

MAX_FRAME = 1000000
MAX_VALUE = 1000000
RICH = ("speed_in_out", "ease_in_out")
INTERPOLATIONS = ("linear", "constant", *RICH)
FRAME_GRID = 6
STEPS_THAT_FIT = (1, 2, 3, 6)
Point = tuple[float, float]
Control = tuple[Any, ...]


@dataclass
class Key:
    """One key of a curve and the controls it carries."""

    frame: int
    value: float
    interpolation: str
    step: int = 1
    incoming: Control = ("none",)
    outgoing: Control | None = None
    linked: bool = True

    def kind(self) -> str:
        """The kind of control that a segment starting at this key gives the key after it."""
        return {"speed_in_out": "speed", "ease_in_out": "ease"}.get(self.interpolation, "none")


def ordered(keys: dict[int, Key]) -> list[Key]:
    """The keys by frame."""
    return [keys[frame] for frame in sorted(keys)]


def incoming_of(first: Key, last: Key) -> Control:
    """The control a segment's last key gives it: its own, or none (zero) of another kind."""
    if last.incoming[0] == first.kind():
        return last.incoming
    return {"speed": ("speed", 0, 0), "ease": ("ease", 0)}.get(first.kind(), ("none",))


def control_points(first: Key, last: Key) -> list[Point]:
    """The four Bezier points of a speed or ease segment."""
    start, end = (first.frame, first.value), (last.frame, last.value)
    into = incoming_of(first, last)
    if first.interpolation == "speed_in_out":
        out = first.outgoing
        assert out is not None
        return [
            start,
            (first.frame + out[1], first.value + out[2]),
            (last.frame + into[1], last.value + into[2]),
            end,
        ]
    assert first.outgoing is not None
    return [
        start,
        (first.frame + first.outgoing[1], first.value),
        (last.frame - into[1], last.value),
        end,
    ]


def bezier_value(points: list[Point], frame: float) -> float:
    """The value of a Bezier segment at a frame, found by bisection on its frame coordinate."""
    (x0, y0), (x1, y1), (x2, y2), (x3, y3) = points

    def at(t: float, a: float, b: float, c: float, d: float) -> float:
        u = 1 - t
        return u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d

    low, high = 0.0, 1.0
    for _ in range(100):
        middle = (low + high) / 2
        if at(middle, x0, x1, x2, x3) < frame:
            low = middle
        else:
            high = middle
    return at((low + high) / 2, y0, y1, y2, y3)


def ease_value(first: Key, last: Key, frame: float) -> float:
    """The value of an ease segment: position under a speed that ramps up, holds and ramps down."""
    assert first.outgoing is not None
    out, into = float(first.outgoing[1]), float(incoming_of(first, last)[1])
    span = last.frame - first.frame
    distance = last.value - first.value
    peak = distance / (out / 2 + (span - out - into) + into / 2)
    elapsed = frame - first.frame
    if out > 0 and elapsed < out:
        travelled = peak * elapsed * elapsed / (2 * out)
    elif into > 0 and elapsed > span - into:
        travelled = distance - peak * (span - elapsed) ** 2 / (2 * into)
    else:
        travelled = peak * (out / 2 + elapsed - out)
    return first.value + travelled


def _segment(first: Key, last: Key, frame: float) -> float:
    if first.interpolation == "constant":
        return first.value
    held = frame
    if first.step > 1:
        held = first.frame + first.step * math.floor((frame - first.frame) / first.step)
    if first.interpolation == "linear":
        fraction = (held - first.frame) / (last.frame - first.frame)
        return first.value + (last.value - first.value) * fraction
    if first.interpolation == "ease_in_out":
        return ease_value(first, last, held)
    return bezier_value(control_points(first, last), held)


def _plain(keys: list[Key], frame: float) -> float:
    if frame <= keys[0].frame:
        return keys[0].value
    if frame >= keys[-1].frame:
        return keys[-1].value
    index = max(i for i, key in enumerate(keys) if key.frame <= frame)
    return _segment(keys[index], keys[index + 1], frame)


def sample(keys: list[Key], frame: float, cycle: bool = False) -> float:
    """The value of a curve at a frame, from its keys alone.

    Before the first key the curve holds the first value. After the last it holds the last, or,
    for a cyclic curve, repeats the span from the first key to the last, drifting by the last
    value minus the first for each repeat.
    """
    keys = sorted(keys, key=lambda key: key.frame)
    if cycle and len(keys) > 1 and frame >= keys[-1].frame:
        span = keys[-1].frame - keys[0].frame
        repeats = math.floor((frame - keys[0].frame) / span)
        drift = (keys[-1].value - keys[0].value) * repeats
        return _plain(keys, frame - repeats * span) + drift
    return _plain(keys, frame)


def is_rich(keys: dict[int, Key]) -> bool:
    """Whether any segment of the curve is a speed or an ease segment."""
    return any(key.interpolation in RICH for key in keys.values())


def shape_is_defined(keys: dict[int, Key]) -> bool:
    """Whether the model gives every speed segment a shape to compare.

    Its two handles must not cross, and its frame axis must not lose the cubic term of the
    Bezier (three times the outgoing length minus three times the incoming one is not twice the
    span), where the worker samples a stepped curve: the known violation KV-9.
    """
    items = ordered(keys)
    for first, last in zip(items, items[1:], strict=False):
        if first.interpolation != "speed_in_out" or first.outgoing is None:
            continue
        span = last.frame - first.frame
        outgoing, incoming = first.outgoing[1], incoming_of(first, last)[1]
        if outgoing > span + incoming or 3 * outgoing - 3 * incoming == 2 * span:
            return False
    return True


def native_refusal(keys: dict[int, Key]) -> str | None:
    """What validating a whole curve refuses, key by key in order, or None.

    A key with a speed or ease segment on either side that is still linked is refused as
    unsupported; a handle outside its segment, or two eases that overlap, is refused as controls.
    """
    items = ordered(keys)
    kinds = incoming_kinds(keys)
    for index, key in enumerate(items):
        if (key.kind() != "none" or kinds[key.frame] != "none") and key.linked:
            return "unsupported_interpolation"
        if index and not _fits(items[index - 1], key):
            return "invalid_argument/controls"
    return None


def _fits(first: Key, last: Key) -> bool:
    span = last.frame - first.frame
    out, into = first.outgoing, incoming_of(first, last)
    if first.interpolation == "speed_in_out":
        return out is not None and 0 <= out[1] <= span and -span <= into[1] <= 0
    if first.interpolation == "ease_in_out":
        return (
            out is not None
            and 0 <= out[1] <= span
            and 0 <= into[1] <= span
            and (out[1] + into[1] <= span)
        )
    return True


def controls_fit(keys: dict[int, Key]) -> bool:
    """Whether every handle lies inside its segment, and two ease lengths do not overlap."""
    items = ordered(keys)
    return all(_fits(a, b) for a, b in zip(items, items[1:], strict=False))


def incoming_kinds(keys: dict[int, Key]) -> dict[int, str]:
    """The kind of control that each key's preceding segment gives it."""
    items = ordered(keys)
    kinds = {items[0].frame: "none"} if items else {}
    for before, after in zip(items, items[1:], strict=False):
        kinds[after.frame] = before.kind()
    return kinds


def key_row(key: Key) -> list[Any]:
    """A key as the property model's observation lists it."""
    outgoing = None if key.outgoing is None else list(key.outgoing)
    return [key.frame, key.value, key.interpolation, key.step, list(key.incoming), outgoing]


def key_from_row(row: list[Any]) -> Key:
    """A key from the row that `key_row` wrote."""
    frame, value, interpolation, step, incoming, outgoing = row
    key = Key(frame, value, interpolation, step, tuple(incoming))
    key.outgoing = None if outgoing is None else tuple(outgoing)
    key.linked = key.kind() == "none" and key.incoming[0] == "none"
    return key


def key_record(key: Key) -> dict[str, Any]:
    """A key as the animation.set_keys call states it."""
    record: dict[str, Any] = {
        "frame": key.frame,
        "value": key.value,
        "interpolation": key.interpolation,
    }
    if key.step != 1:
        record["step"] = key.step
    if key.incoming[0] == "speed":
        record["incoming"] = {
            "kind": "speed",
            "frame_delta": key.incoming[1],
            "value_delta": key.incoming[2],
        }
    elif key.incoming[0] == "ease":
        record["incoming"] = {"kind": "ease", "frames": key.incoming[1]}
    if key.outgoing is not None and key.outgoing[0] == "speed":
        record["outgoing"] = {"frame_delta": key.outgoing[1], "value_delta": key.outgoing[2]}
    elif key.outgoing is not None:
        record["outgoing"] = {"frames": key.outgoing[1]}
    return record


def _quarter(rng: random.Random, limit: int) -> float:
    return rng.randint(-4 * limit, 4 * limit) / 4


def _handles(rng: random.Random, first: Key, last: Key) -> None:
    """Give a rich segment handles that fit it, do not cross and keep the frame axis cubic."""
    span = last.frame - first.frame
    if first.interpolation == "speed_in_out":
        near = rng.randint(0, span - 1)
        far = rng.randint(near, span)
        if 3 * (far - near) == span:
            far += 1 if far < span else -1
        first.outgoing = ("speed", near, _quarter(rng, 40))
        last.incoming = ("speed", far - span, _quarter(rng, 40))
    else:
        out = rng.randint(0, span)
        first.outgoing = ("ease", out)
        last.incoming = ("ease", rng.randint(0, span - out))


def random_keys(rng: random.Random) -> list[Key]:
    """Two to four keys on the frame grid, joined by every kind of segment, handles that fit."""
    count = rng.randint(2, 4)
    frames = sorted(rng.sample(range(0, 9), count))
    keys = [Key(f * FRAME_GRID, _quarter(rng, 100), "linear") for f in frames]
    for first, last in zip(keys, keys[1:], strict=False):
        first.interpolation = rng.choice(INTERPOLATIONS)
        if first.interpolation == "linear":
            first.step = rng.choice(STEPS_THAT_FIT)
        if first.interpolation in RICH:
            _handles(rng, first, last)
    for key in keys:
        key.linked = key.kind() == "none" and key.incoming[0] == "none"
    return keys


def _moved(key: Key, frame_offset: int, value_offset: float) -> Key | str:
    frame = key.frame + frame_offset
    if frame < 0 or frame > MAX_FRAME:
        return "invalid_argument/frame_out_of_range"
    value = key.value + value_offset
    if abs(value) > MAX_VALUE:
        return "invalid_argument/value_out_of_bounds"
    return dataclasses.replace(key, frame=frame, value=value)


def move_keys(
    keys: dict[int, Key], frames: list[int], frame_offset: int, value_offset: float
) -> tuple[str | None, dict[int, Key]]:
    """Shift the keys at `frames`; the error a correct worker answers, or the edited keys."""
    if any(frame not in keys for frame in frames):
        return "missing_key", keys
    if frame_offset == 0 and value_offset == 0:
        return None, keys
    moved: list[Key] = []
    for key in ordered(keys):
        if key.frame in frames:
            shifted = _moved(key, frame_offset, value_offset)
            if isinstance(shifted, str):
                return shifted, keys
            key = shifted
        if moved and key.frame <= moved[-1].frame:
            reason = "key_exists" if key.frame == moved[-1].frame else "order_changed"
            return f"invalid_argument/{reason}", keys
        moved.append(key)
    result = {key.frame: key for key in moved}
    refusal = native_refusal(result)
    return (refusal, keys) if refusal else (None, result)


def duplicate_keys(
    keys: dict[int, Key], frames: list[int], frame_offset: int, value_offset: float
) -> tuple[str | None, dict[int, Key]]:
    """Copy the keys at `frames` verbatim, shifted: the worker's error, or the edited keys."""
    if any(frame not in keys for frame in frames):
        return "missing_key", keys
    before = incoming_kinds(keys)
    copies: dict[int, Key] = {}
    for frame in sorted(frames):
        copy = _moved(keys[frame], frame_offset, value_offset)
        if isinstance(copy, str):
            return copy, keys
        if copy.frame in keys:
            return "invalid_argument/key_exists", keys
        copies[copy.frame] = copy
    result = {**keys, **copies}
    refusal = native_refusal(result)
    if refusal:
        return refusal, keys
    after = incoming_kinds(result)
    if any(after[frame] != before[frame] for frame in keys):
        return "invalid_argument/incoming_kind_changed", keys
    if any(after[frame + frame_offset] != before[frame] for frame in frames):
        return "invalid_argument/incoming_kind_changed", keys
    return None, result


def insert_breakdown(
    keys: dict[int, Key], frame: int, favour: float | None, value: float | None
) -> tuple[str | None, dict[int, Key]]:
    """Insert a key inside a segment as the editor's Set Breakdown Key does."""
    if frame in keys:
        return "invalid_argument/key_exists", keys
    items = ordered(keys)
    before = [key for key in items if key.frame < frame]
    after = [key for key in items if key.frame > frame]
    if not before or not after:
        return "invalid_argument/outside_segment", keys
    first, last = before[-1], after[0]
    chosen = (
        value
        if value is not None
        else first.value + (last.value - first.value) * ((favour or 0) / 100)
    )
    if abs(chosen) > MAX_VALUE:
        return "invalid_argument/value_out_of_bounds", keys
    new_first, new_key, new_last = _split(first, last, frame)
    new_key.value = chosen
    result = {**keys, new_first.frame: new_first, new_key.frame: new_key, new_last.frame: new_last}
    refusal = native_refusal(result)
    return (refusal, keys) if refusal else (None, result)


def _split(first: Key, last: Key, frame: int) -> tuple[Key, Key, Key]:
    """The segment's two keys and the new key between them, as the editor cuts the segment."""
    key = Key(frame, 0.0, first.interpolation, first.step, linked=first.kind() == "none")
    first, last = dataclasses.replace(first), dataclasses.replace(last)
    if first.interpolation == "ease_in_out":
        assert first.outgoing is not None
        key.incoming, key.outgoing = ("ease", 0), ("ease", 0)
        first.outgoing = ("ease", min(first.outgoing[1], frame - first.frame))
        last.incoming = ("ease", min(incoming_of(first, last)[1], last.frame - frame))
    elif first.interpolation == "speed_in_out":
        _cut_speed(first, key, last)
    return first, key, last


def _cut_speed(first: Key, key: Key, last: Key) -> None:
    """Cut a speed segment at a frame with de Casteljau, keeping its shape."""
    p0, p1, p2, p3 = control_points(first, last)
    low, high = 0.0, 1.0
    for _ in range(100):
        middle = (low + high) / 2
        x = ((1 - middle) ** 3) * p0[0] + 3 * ((1 - middle) ** 2) * middle * p1[0]
        x += 3 * (1 - middle) * middle * middle * p2[0] + middle**3 * p3[0]
        low, high = (middle, high) if x < key.frame else (low, middle)
    t = (low + high) / 2

    def lerp(a: Point, b: Point) -> Point:
        return (a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]))

    q0, q1, q2 = lerp(p0, p1), lerp(p1, p2), lerp(p2, p3)
    r0, r1 = lerp(q0, q1), lerp(q1, q2)
    s = lerp(r0, r1)
    first.outgoing = ("speed", q0[0] - p0[0], q0[1] - p0[1])
    key.incoming = ("speed", r0[0] - s[0], r0[1] - s[1])
    key.outgoing = ("speed", r1[0] - s[0], r1[1] - s[1])
    last.incoming = ("speed", q2[0] - p3[0], q2[1] - p3[1])
