"""Cameras for the property model: create, copy, select roles and remove, with packed numbers.

A sheet has at least one camera. The first is unnamed and shows as Camera1; every camera made
since has an explicit name that is neither `Camera<digits>` nor another camera's. One camera is
the output (render) camera and one the preview camera, perhaps the same. The scene's own size
settings are the output camera's, so `scene.camera` always names that camera's size.
"""

from __future__ import annotations

import random
import re
from collections.abc import Callable
from typing import Any

from mcp_property_state import Camera, ModelError

Step = dict[str, Any]
ROLES = ("output", "preview")
RESERVED = re.compile(r"^Camera[0-9]+$")
MAX_CAMERAS = 6


def camera_named(scene: Any, name: str) -> Camera:
    """The camera called `name`."""
    for camera in scene.cameras:
        if camera.name == name:
            return camera
    raise ModelError(f"no camera named {name!r}")


def name_refusal(scene: Any, name: str) -> str | None:
    """What a camera name is refused for: the reserved pattern or another camera's name."""
    if RESERVED.match(name):
        return "invalid_params/reserved_name"
    if any(camera.name == name for camera in scene.cameras):
        return "name_conflict"
    return None


def create_effect(scene: Any, step: Step) -> str | None:
    """camera.create."""
    refusal = name_refusal(scene, step["name"])
    if refusal:
        return refusal
    sizes = {key: step[key] for key in ("width_px", "height_px", "width_in", "height_in")}
    scene.cameras.append(Camera(step["name"], sizes))
    return None


def duplicate_effect(scene: Any, step: Step) -> str | None:
    """camera.duplicate: a copy with the source's resolution and size, and no role."""
    source = camera_named(scene, step["camera"])
    refusal = name_refusal(scene, step["name"])
    if refusal:
        return refusal
    scene.cameras.append(Camera(step["name"], dict(source.size)))
    return None


def select_effect(scene: Any, step: Step) -> str | None:
    """camera.select."""
    camera_named(scene, step["camera"])
    roles = step["roles"]
    if len(set(roles)) != len(roles):
        return "invalid_params/roles"
    camera = camera_named(scene, step["camera"])
    for role in roles:
        scene.roles[role] = camera
    scene.camera = scene.roles["output"].size
    return None


def held_roles(scene: Any, camera: Camera) -> list[str]:
    """The roles a camera holds, output first."""
    return [role for role in ROLES if scene.roles[role] is camera]


def replacement_refusal(scene: Any, camera: Camera, replacement: dict[str, str]) -> str | None:
    """The refusal a held role without a fitting replacement gets, in the worker's order."""
    held = held_roles(scene, camera)
    for role in ROLES:
        if role not in replacement:
            if role in held:
                return "in_use/role"
            continue
        if role not in held or replacement[role] == camera.name:
            return "invalid_params/replacement"
    return None


def remove_effect(scene: Any, step: Step) -> str | None:
    """camera.remove, without stage children."""
    for name in (step["camera"], *step["replacement"].values()):
        camera_named(scene, name)
    if len(scene.cameras) <= 1:
        return "in_use/last_camera"
    camera = camera_named(scene, step["camera"])
    refusal = replacement_refusal(scene, camera, step["replacement"])
    if refusal:
        return refusal
    scene.cameras.remove(camera)
    for role, name in step["replacement"].items():
        scene.roles[role] = camera_named(scene, name)
    scene.camera = scene.roles["output"].size
    return None


def expected_cameras(scene: Any) -> list[dict[str, Any]]:
    """The camera list as the model expects the worker to report it, in sheet order."""
    return [
        {"name": camera.name, **camera.size, "roles": held_roles(scene, camera)}
        for camera in scene.cameras
    ]


def choose_create(rng: random.Random, scene: Any, fresh: Callable[..., str], sizes: Any) -> Step:
    """A camera with a fresh name, now and then a reserved or a taken one."""
    step = {"name": fresh(rng, scene, "K"), **sizes(rng)}
    roll = rng.random()
    if roll < 0.05:
        step["name"] = f"Camera{rng.randint(1, 99)}"
    elif roll < 0.1 and len(scene.cameras) > 1:
        step["name"] = rng.choice(scene.cameras[1:]).name
    return step


def choose_duplicate(rng: random.Random, scene: Any, fresh: Callable[..., str]) -> Step:
    """A copy of a camera under a fresh name."""
    return {
        "camera": rng.choice(scene.cameras).name,
        "name": fresh(rng, scene, "K"),
        "parent_policy": rng.choice(("same_parent", "none")),
    }


def choose_select(rng: random.Random, scene: Any) -> Step:
    """A camera made the output camera, the preview camera or both."""
    roles = rng.choice((["output"], ["preview"], ["output", "preview"], ["preview", "output"]))
    if rng.random() < 0.05:
        roles = [roles[0], roles[0]]
    return {"camera": rng.choice(scene.cameras).name, "roles": roles}


def choose_remove(rng: random.Random, scene: Any) -> Step:
    """A camera removed, with replacements that are right for the roles it holds, mostly."""
    camera = rng.choice(scene.cameras)
    others = [other.name for other in scene.cameras if other is not camera] or [camera.name]
    replacement = {role: rng.choice(others) for role in held_roles(scene, camera)}
    roll = rng.random()
    if roll < 0.1 and replacement:
        del replacement[rng.choice(sorted(replacement))]
    elif roll < 0.2:
        free = [role for role in ROLES if role not in replacement]
        if free:
            replacement[free[0]] = rng.choice(others)
    elif roll < 0.25 and replacement:
        replacement[sorted(replacement)[0]] = camera.name
    return {"camera": camera.name, "replacement": replacement}


def _camera_id(handles: dict[str, dict[str, str]], name: str) -> str:
    return handles["camera"][name]


def _create_request(step: Step, handles: dict[str, dict[str, str]]) -> tuple[str, dict[str, Any]]:
    keys = ("name", "width_px", "height_px", "width_in", "height_in")
    return "camera.create", {key: step[key] for key in keys}


def _duplicate_request(
    step: Step, handles: dict[str, dict[str, str]]
) -> tuple[str, dict[str, Any]]:
    return "camera.duplicate", {
        "camera_id": _camera_id(handles, step["camera"]),
        "name": step["name"],
        "parent_policy": step["parent_policy"],
    }


def _select_request(step: Step, handles: dict[str, dict[str, str]]) -> tuple[str, dict[str, Any]]:
    return "camera.select", {
        "camera_id": _camera_id(handles, step["camera"]),
        "roles": step["roles"],
    }


def _remove_request(step: Step, handles: dict[str, dict[str, str]]) -> tuple[str, dict[str, Any]]:
    params: dict[str, Any] = {
        "camera_id": _camera_id(handles, step["camera"]),
        "children_policy": "refuse",
    }
    if step["replacement"]:
        params["replacement"] = {
            role: _camera_id(handles, name) for role, name in step["replacement"].items()
        }
    return "camera.remove", params


def _learn_camera(step: Step, result: dict[str, Any], handles: dict[str, Any]) -> None:
    handles["camera"][step["name"]] = result["camera"]["camera_id"]


def _forget_camera(step: Step, result: dict[str, Any], handles: dict[str, Any]) -> None:
    del handles["camera"][step["camera"]]


def _nothing(step: Step, result: dict[str, Any], handles: dict[str, Any]) -> None:
    return None


def build(model: Any) -> dict[str, tuple[Any, tuple[str, ...]]]:
    """Return each operation with the tools it needs, for the model to register."""
    op, fresh = model.Operation, model._name

    def sizes(rng: random.Random) -> Step:
        return {
            "width_px": rng.choice(model.PIXELS),
            "height_px": rng.choice(model.PIXELS),
            "width_in": rng.choice(model.INCHES),
            "height_in": rng.choice(model.INCHES),
        }

    room = lambda scene: len(scene.cameras) < MAX_CAMERAS  # noqa: E731
    return {
        "camera_create": (
            op(
                "camera_create",
                2,
                room,
                lambda rng, scene: choose_create(rng, scene, fresh, sizes),
                create_effect,
                _create_request,
                _learn_camera,
            ),
            ("camera.create",),
        ),
        "camera_duplicate": (
            op(
                "camera_duplicate",
                2,
                room,
                lambda rng, scene: choose_duplicate(rng, scene, fresh),
                duplicate_effect,
                _duplicate_request,
                _learn_camera,
            ),
            ("camera.duplicate",),
        ),
        "camera_select": (
            op(
                "camera_select",
                3,
                lambda scene: True,
                choose_select,
                select_effect,
                _select_request,
                _nothing,
            ),
            ("camera.select",),
        ),
        "camera_remove": (
            op(
                "camera_remove",
                2,
                lambda scene: True,
                choose_remove,
                remove_effect,
                _remove_request,
                _forget_camera,
            ),
            ("camera.remove",),
        ),
    }
