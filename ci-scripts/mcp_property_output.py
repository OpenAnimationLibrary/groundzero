"""Stored output settings and guides for the property model.

The model starts knowing none of these settings: a new scene's defaults are not documented, so
only what a step has set is tracked and compared. `scene.props` holds that, and
`expected_properties` gives the shape the worker's `scene.properties.inspect` is read against.
"""

from __future__ import annotations

import random
from typing import Any

Step = dict[str, Any]
BLOCKS = ("output", "preview")
BLOCK_FIELDS = ("range", "step", "shrink", "bpp", "aspect_export")
GUIDE_LIMIT = 4
RATIOS = (0.25, 0.5, 1.0, 1.5, 1.75, 2.5, 4.0)
COLORS = ({"r": 0, "g": 0, "b": 0, "a": 255}, {"r": 255, "g": 128, "b": 7, "a": 0})


def props(scene: Any) -> dict[str, Any]:
    """The tracked output and preview blocks, created on first use."""
    return scene.props.setdefault("blocks", {"output": {}, "preview": {}})


def apply_aspect(block: dict[str, Any], aspect: dict[str, Any]) -> None:
    """Set the aspect export: none keeps the stored ratio and color, the others replace them."""
    block["aspect_mode"] = aspect["mode"]
    if aspect["mode"] != "none":
        block["aspect_ratio"] = aspect["ratio"]
        if "color" in aspect:
            block["aspect_color"] = dict(aspect["color"])


def apply_block(block: dict[str, Any], settings: dict[str, Any]) -> None:
    """Change one block by the settings of a step."""
    if "range" in settings:
        block["range"] = dict(settings["range"])
    for name in ("step", "shrink", "bpp"):
        if name in settings:
            block[name] = settings[name]
    if "aspect_export" in settings:
        apply_aspect(block, settings["aspect_export"])


def set_refusal(step: Step) -> str | None:
    """The refusal of a scene.output.set, in the order the worker checks."""
    settings = step.get("settings", {})
    if not settings and "sync" not in step and "filters" not in step:
        return "invalid_params/nothing_to_set"
    if len(set(step["apply_to"])) != len(step["apply_to"]):
        return "invalid_params/apply_to"
    if "sync" in step and "preview" not in step["apply_to"]:
        return "invalid_params/sync_color_settings"
    ranged = settings.get("range", {})
    if ranged.get("kind") == "frames" and ranged["start_frame"] > ranged["end_frame"]:
        return "invalid_params/range"
    return None


def set_effect(scene: Any, step: Step) -> str | None:
    """scene.output.set."""
    refusal = set_refusal(step)
    if refusal:
        return refusal
    for name in step["apply_to"]:
        apply_block(props(scene)[name], step.get("settings", {}))
    if "sync" in step:
        scene.props["sync"] = step["sync"]
    if "filters" in step:
        scene.props["filters"] = step["filters"]
    return None


def guides_effect(scene: Any, step: Step) -> str | None:
    """scene.guides.set."""
    field, markers, ruler = step.get("field", {}), step.get("markers", {}), step.get("ruler")
    if not field and not markers and ruler is None:
        return "invalid_params/nothing_to_set"
    guides = scene.props.setdefault("guides", {})
    guides.update({f"field_{key}": value for key, value in field.items()})
    guides.update({f"markers_{key}": value for key, value in markers.items()})
    if ruler is not None:
        guides["ruler"] = {key: list(values) for key, values in ruler.items()}
    return None


def effective_range(block: dict[str, Any], frames: int) -> dict[str, int] | None:
    """The frames a block renders: its range cut to the scene's length, or None when empty."""
    ranged = block["range"]
    first = 0 if ranged["kind"] == "all" else ranged["start_frame"]
    last = frames - 1 if ranged["kind"] == "all" else min(ranged["end_frame"], frames - 1)
    return {"start_frame": first, "end_frame": last} if last >= first else None


def expected_block(block: dict[str, Any], frames: int) -> dict[str, Any]:
    """What the worker reads of one block, for the settings that are known."""
    record: dict[str, Any] = {}
    if "range" in block:
        record["range"] = dict(block["range"])
        record["effective_range"] = effective_range(block, frames)
    record.update({name: block[name] for name in ("step", "shrink", "bpp") if name in block})
    aspect: dict[str, Any] = {}
    if "aspect_mode" in block:
        aspect["mode"] = block["aspect_mode"]
    if "aspect_ratio" in block:
        aspect["ratio"] = block["aspect_ratio"]
    if "aspect_color" in block:
        aspect["color"] = dict(block["aspect_color"])
    if aspect:
        record["aspect_export"] = aspect
    return record


def expected_properties(scene: Any, frames: int) -> dict[str, Any]:
    """The part of `scene.properties.inspect` the model knows, by the worker's own names."""
    record: dict[str, Any] = {
        name: expected_block(block, frames)
        for name, block in scene.props.get("blocks", {"output": {}, "preview": {}}).items()
    }
    if "sync" in scene.props:
        record["sync_color_settings"] = scene.props["sync"]
    if "filters" in scene.props:
        record["column_filters_on_render"] = scene.props["filters"]
    guides = scene.props.get("guides", {})
    pairs = (
        ("field_guide", "field_", ("size", "aspect_ratio")),
        ("markers", "markers_", ("interval", "start_frame")),
    )
    for name, prefix, fields in pairs:
        known = {key: guides[prefix + key] for key in fields if prefix + key in guides}
        if known:
            record[name] = known
    if "ruler" in guides:
        record["ruler_guides"] = {key: list(values) for key, values in guides["ruler"].items()}
    return record


def project(observed: Any, shape: Any) -> Any:
    """Keep of what the worker read only the entries that the model expects."""
    if isinstance(shape, dict):
        return {key: project(observed[key], value) for key, value in shape.items()}
    return observed


def choose_range(rng: random.Random) -> dict[str, Any]:
    """A range of all frames, or of some, rarely with its end before its start."""
    if rng.random() < 0.3:
        return {"kind": "all"}
    first = rng.randint(0, 20)
    last = first + rng.randint(0, 20)
    if rng.random() < 0.08 and last > first:
        first, last = last, first
    return {"kind": "frames", "start_frame": first, "end_frame": last}


def choose_aspect(rng: random.Random) -> dict[str, Any]:
    """An aspect export: none, or letterbox or crop with a ratio and perhaps a color."""
    if rng.random() < 0.3:
        return {"mode": "none"}
    aspect: dict[str, Any] = {
        "mode": rng.choice(("letterbox", "crop")),
        "ratio": rng.choice(RATIOS),
    }
    if rng.random() < 0.5:
        aspect["color"] = dict(rng.choice(COLORS))
    return aspect


def choose_set(rng: random.Random, scene: Any) -> Step:
    """A change of the output and preview blocks, now and then an empty or inconsistent one."""
    apply_to = rng.choice((["output"], ["preview"], ["output", "preview"]))
    options = {
        "range": choose_range,
        "step": lambda r: r.randint(1, 8),
        "shrink": lambda r: r.randint(1, 8),
        "bpp": lambda r: r.choice((32, 64)),
        "aspect_export": choose_aspect,
    }
    chosen = rng.sample(BLOCK_FIELDS, rng.randint(1, 3))
    step: Step = {"apply_to": apply_to, "settings": {n: options[n](rng) for n in chosen}}
    if "preview" in apply_to and rng.random() < 0.3:
        step["sync"] = rng.random() < 0.5
    if rng.random() < 0.2:
        step["filters"] = rng.random() < 0.5
    roll = rng.random()
    if roll < 0.04:
        step = {"apply_to": apply_to}
    elif roll < 0.08:
        step["apply_to"] = [apply_to[0], apply_to[0]]
    elif roll < 0.12:
        step["apply_to"] = ["output"]
        step["sync"] = True
    return step


def choose_positions(rng: random.Random) -> list[float]:
    """Guide positions in inches, multiples of a quarter so that they read back exactly."""
    return [rng.randint(-40, 40) / 4 for _ in range(rng.randint(0, GUIDE_LIMIT))]


def choose_guides(rng: random.Random, scene: Any) -> Step:
    """A change of the field guide, the markers or the ruler guides, now and then none."""
    step: Step = {}
    if rng.random() < 0.5:
        step["field"] = {"size": rng.randint(1, 100), "aspect_ratio": rng.choice(RATIOS)}
        if rng.random() < 0.5:
            del step["field"][rng.choice(("size", "aspect_ratio"))]
    if rng.random() < 0.5:
        step["markers"] = {"interval": rng.randint(0, 100), "start_frame": rng.randint(0, 50)}
        if rng.random() < 0.5:
            del step["markers"][rng.choice(("interval", "start_frame"))]
    if rng.random() < 0.5:
        step["ruler"] = {
            "vertical_lines_x_in": choose_positions(rng),
            "horizontal_lines_y_in": choose_positions(rng),
        }
    return step


def _set_request(step: Step, handles: dict[str, Any]) -> tuple[str, dict[str, Any]]:
    params: dict[str, Any] = {"apply_to": step["apply_to"]}
    if step.get("settings"):
        params["settings"] = step["settings"]
    if "sync" in step:
        params["sync_color_settings"] = step["sync"]
    if "filters" in step:
        params["column_filters_on_render"] = step["filters"]
    return "scene.output.set", params


def _guides_request(step: Step, handles: dict[str, Any]) -> tuple[str, dict[str, Any]]:
    params: dict[str, Any] = {}
    if "field" in step:
        params["field_guide"] = step["field"]
    if "markers" in step:
        params["markers"] = step["markers"]
    if "ruler" in step:
        params["ruler_guides"] = step["ruler"]
    return "scene.guides.set", params


def build(model: Any) -> dict[str, tuple[Any, tuple[str, ...]]]:
    """Return each operation with the tools it needs, for the model to register."""
    op = model.Operation
    return {
        "output_set": (
            op("output_set", 3, lambda scene: True, choose_set, set_effect, _set_request),
            ("scene.output.set",),
        ),
        "guides_set": (
            op("guides_set", 3, lambda scene: True, choose_guides, guides_effect, _guides_request),
            ("scene.guides.set",),
        ),
    }
