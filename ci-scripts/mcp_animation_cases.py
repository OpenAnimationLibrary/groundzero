"""Qualify detached rich stage animation against the actual native worker."""

from __future__ import annotations

import base64
from collections.abc import Sequence
from copy import deepcopy
from typing import TYPE_CHECKING, Any

from mcp_png import Pixel, decode_png

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


def _target(object_id: str) -> dict[str, str]:
    return {"object_id": object_id, "channel": "x", "unit": "inch"}


def _simple(frame: int, value: float) -> dict[str, Any]:
    return {"frame": frame, "value": value, "interpolation": "linear"}


def _rich_keys(scale: float = 1) -> list[dict[str, Any]]:
    return [
        {
            "frame": 0,
            "value": 0,
            "interpolation": "speed_in_out",
            "outgoing": {"frame_delta": 4, "value_delta": 0},
        },
        {
            "frame": 12,
            "value": 120 * scale,
            "interpolation": "ease_in_out",
            "incoming": {
                "kind": "speed",
                "frame_delta": -4,
                "value_delta": -24 * scale,
            },
            "outgoing": {"frames": 3},
        },
        {
            "frame": 24,
            "value": 240 * scale,
            "interpolation": "linear",
            "incoming": {"kind": "ease", "frames": 3},
        },
        {"frame": 36, "value": 360 * scale, "interpolation": "constant"},
    ]


def _inspect(case: NativeAcceptance, document: str, target: dict[str, str]) -> dict[str, Any]:
    return case.success("animation.keys", {"document_id": document, **target})


def _sample(
    case: NativeAcceptance, document: str, target: dict[str, str], frames: Sequence[float]
) -> list[float]:
    result = case.success("animation.sample", {"document_id": document, **target, "frames": frames})
    case.assertEqual(result["value_space"], "effective_local_channel")
    case.assertEqual([value["frame"] for value in result["values"]], frames)
    return [value["value"] for value in result["values"]]


def _peg(case: NativeAcceptance, document: str) -> dict[str, str]:
    created = case.mutate("object.create", document, kind="pegbar", name="Animation peg")
    return _target(created["object_id"])


def _render(case: NativeAcceptance, document: str, frame: int) -> list[Pixel]:
    job = case.mutate("render.preview", document, frame=frame, width=32, height=32)
    status = case.wait_job(job["job_id"])
    case.assertEqual(status["status"], "completed", status)
    artifacts = case.success("render.artifacts", {"job_id": job["job_id"]})["artifacts"]
    case.assertEqual(len(artifacts), 1)
    artifact = case.success("artifact.read", {"artifact_id": artifacts[0]["artifact_id"]})
    width, height, pixels = decode_png(base64.b64decode(artifact["image"]["data"], validate=True))
    case.assertEqual((width, height), (32, 32))
    case.assertTrue(any(pixel[3] for pixel in pixels))
    return pixels


def _pixels_match(case: NativeAcceptance, actual: list[Pixel], expected: list[Pixel]) -> None:
    case.assertEqual(len(actual), len(expected))
    difference = max(
        abs(a - b)
        for left, right in zip(actual, expected, strict=True)
        for a, b in zip(left, right, strict=True)
    )
    # Native TNZ stores decimal transforms; subpixel rounding may change one output code value.
    case.assertLessEqual(difference, 2)


def _reopen(case: NativeAcceptance, document: str, name: str) -> tuple[str, dict[str, str]]:
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / name))
    reopened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    objects = case.success("scene.summary", {"document_id": reopened})["objects"]
    peg = next(item for item in objects if item["name"] == "Animation peg")
    return reopened, _target(peg["object_id"])


def animation_channel_descriptors_and_fractional_sampling(
    case: NativeAcceptance,
) -> None:
    """Discover real channel units and verify native stepped subframe semantics."""
    document = case.create()
    target = _peg(case, document)
    descriptor = case.success(
        "animation.channels",
        {"document_id": document, "object_id": target["object_id"]},
    )
    channels = {entry["channel"]: entry for entry in descriptor["channels"]}
    case.assertEqual(set(channels), {"x", "y", "z", "rotation", "scale_x", "scale_y", "scale"})
    case.assertEqual(channels["x"]["unit"], "inch")
    case.assertEqual(channels["rotation"]["unit"], "degree")
    case.assertEqual(channels["z"]["unit"], "native_z")
    for channel in channels.values():
        case.assertTrue(channel["writable"])
        case.assertIsNone(channel["unsupported_reason"])
        case.assertEqual(channel["key_time"], "integer")
        case.assertEqual(channel["sample_time"], "fractional")
        case.assertEqual(channel["step_bounds"], [1, 1000])
    case.mutate(
        "animation.set_keys",
        document,
        **target,
        keys=[{**_simple(0, 0), "step": 3}, _simple(10, 90)],
    )
    frames = [0, 2.99, 3, 3.01, 5.99, 6, 8.99, 9, 9.99, 10]
    case.assertEqual(
        _sample(case, document, target, frames), [0, 0, 30, 30, 30, 60, 60, 90, 90, 90]
    )
    inspected = _inspect(case, document, target)
    case.assertEqual(inspected["value_space"], "base_channel")
    case.assertEqual(inspected["keys"][0]["step"], 3)
    case.assertEqual(inspected["keys"][1]["step"], 1)
    revision = case.revisions[document]
    request = case.write_params(document, {**target, "keys": [_simple(10, 90)]})
    no_op = case.success("animation.set_keys", request)
    case.assertEqual(no_op["revision"], revision)
    case.assertEqual(no_op["operation_id"], request["operation_id"])
    case.assertEqual(case.success("animation.set_keys", request), no_op)
    case.assertEqual(_inspect(case, document, target), inspected)


def _animated_scene(case: NativeAcceptance) -> tuple[str, dict[str, str]]:
    document, level, column = case.populated_scene()
    case.mutate(
        "column.expose",
        document,
        column_id=column["column_id"],
        level_id=level["level_id"],
        drawing_id={"number": 1, "suffix": ""},
        start_frame=0,
        length=37,
        overwrite=True,
    )
    target = _peg(case, document)
    case.mutate(
        "object.parent",
        document,
        object_id=column["object_id"],
        parent_id=target["object_id"],
    )
    case.mutate("animation.set_keys", document, **target, keys=_rich_keys(1 / 2400))
    camera = case.success("camera.inspect", {"document_id": document})
    case.mutate(
        "animation.set_keys",
        document,
        **_target(camera["object_id"]),
        keys=[_simple(0, 0), _simple(36, 6 / 120)],
    )
    return document, target


def _reference_frames(case: NativeAcceptance, frames: list[int]) -> list[list[Pixel]]:
    reference, _level, column = case.populated_scene()
    # Independent analytic samples of speed/ease/linear segments, in channel units.
    expected = {0: 0, 6: 51, 12: 120, 15: 140, 18: 180, 24: 240, 30: 300, 36: 360}
    images = []
    for frame in frames:
        displacement = expected[frame] / 2400 - frame / 720
        case.mutate(
            "animation.set_keys",
            reference,
            **_target(column["object_id"]),
            keys=[_simple(0, displacement)],
        )
        images.append(_render(case, reference, 0))
    return images


def rich_animation_round_trips_and_renders(case: NativeAcceptance) -> None:
    """Compare rich peg/camera motion with static native references and two saved generations."""
    document, target = _animated_scene(case)
    frames = [0, 6, 12, 15, 18, 24, 30, 36]
    samples = _sample(case, document, target, frames)
    expected = [value / 2400 for value in (0, 51, 120, 140, 180, 240, 300, 360)]
    for actual, value in zip(samples, expected, strict=True):
        case.assertAlmostEqual(actual, value, delta=1e-8)
    images = [_render(case, document, frame) for frame in frames]
    case.assertGreaterEqual(len({tuple(image) for image in images}), 6)
    for actual, reference in zip(images, _reference_frames(case, frames), strict=True):
        _pixels_match(case, actual, reference)
    reopened, reopened_target = _reopen(case, document, "rich-first")
    keys = _inspect(case, reopened, reopened_target)["keys"]
    case.assertEqual(keys[1]["incoming"]["kind"], "speed")
    case.assertAlmostEqual(keys[1]["incoming"]["value_delta"], -24 / 2400, delta=1e-8)
    case.assertEqual(keys[1]["outgoing"], {"frames": 3})
    case.assertEqual(keys[2]["incoming"], {"kind": "ease", "frames": 3})
    for frame, image in zip(frames, images, strict=True):
        _pixels_match(case, _render(case, reopened, frame), image)
    patch = _rich_keys(1 / 2400)[1]
    patch["outgoing"] = {"frames": 1}
    case.mutate("animation.set_keys", reopened, **reopened_target, keys=[patch])
    edited = _render(case, reopened, 15)
    case.assertNotEqual(edited, images[3])
    second, second_target = _reopen(case, reopened, "rich-second")
    case.assertEqual(_inspect(case, second, second_target)["keys"][1]["outgoing"], {"frames": 1})
    _pixels_match(case, _render(case, second, 15), edited)
    _pixels_match(case, _render(case, document, 15), images[3])


def _invalid_batches() -> list[list[dict[str, Any]]]:
    keys = _rich_keys()
    batches = [
        [keys[0], keys[0]],
        [_simple(0, 0)],
        [{**keys[1], "step": 0}],
        [{**keys[1], "step": 1001}],
        [{**keys[1], "step": 1.5}],
        [{**keys[1], "frame": 12.5}],
        [{**keys[1], "outgoing": {"frames": 10}}],
    ]
    missing = deepcopy(keys[1])
    del missing["incoming"]
    batches.append([keys[0], missing])
    batches.append([{**keys[0], "outgoing": {"frame_delta": 13, "value_delta": 0}}])
    batches.append([{**keys[1], "incoming": {"kind": "ease", "frames": 2}}])
    return batches


def rich_animation_rejections_preserve_scene(case: NativeAcceptance) -> None:
    """Reject malformed and incomplete segments before any key or revision changes."""
    document, _level, column = case.populated_scene()
    target = _target(column["object_id"])
    case.mutate("animation.set_keys", document, **target, keys=_rich_keys())
    before = _inspect(case, document, target)
    revision = case.revisions[document]
    for keys in _invalid_batches():
        case.failure("animation.set_keys", case.write_params(document, {**target, "keys": keys}))
        case.assertEqual(_inspect(case, document, target), before)
        case.assertEqual(
            case.success("scene.summary", {"document_id": document})["revision"],
            revision,
        )
    case.failure(
        "animation.delete_keys",
        case.write_params(document, {**target, "frames": [12.5]}),
    )
    case.failure("animation.sample", {"document_id": document, **target, "frames": [1.5, 1.5]})
    case.mutate("column.update", document, column_id=column["column_id"], locked=True)
    case.failure(
        "animation.set_keys",
        case.write_params(document, {**target, "keys": _rich_keys()}),
    )
    descriptor = case.success(
        "animation.channels",
        {"document_id": document, "object_id": column["object_id"]},
    )
    case.assertTrue(all(not channel["writable"] for channel in descriptor["channels"]))
    case.assertEqual(_inspect(case, document, target), before)


def rich_animation_retries_and_noops(case: NativeAcceptance) -> None:
    """Preserve revisions on identical keys and replay the original authoritative receipt."""
    document = case.create()
    target = _peg(case, document)
    request = case.write_params(document, {**target, "keys": _rich_keys()})
    first = case.success("animation.set_keys", request)
    case.revisions[document] = first["revision"]
    before = _inspect(case, document, target)
    case.assertEqual(case.success("animation.set_keys", request), first)
    no_op = case.mutate("animation.set_keys", document, **target, keys=_rich_keys())
    case.assertEqual(no_op["revision"], first["revision"])
    case.assertEqual(_inspect(case, document, target), before)
    conflicting = {**request, "keys": [{**_rich_keys()[1], "outgoing": {"frames": 1}}]}
    case.assertEqual(case.failure("animation.set_keys", conflicting)["code"], "operation_conflict")
    changed = case.mutate(
        "animation.set_keys",
        document,
        **target,
        keys=[{**_rich_keys()[1], "outgoing": {"frames": 1}}],
    )
    case.assertNotEqual(changed["revision"], first["revision"])
    case.assertEqual(case.success("animation.set_keys", request), first)
    stale = case.write_params(document, {**target, "keys": _rich_keys()})
    stale["expected_revision"] = first["revision"]
    case.failure("animation.set_keys", stale)
    case.assertNotEqual(_inspect(case, document, target), before)
    case.assertEqual(_inspect(case, document, target)["keys"][1]["outgoing"], {"frames": 1})
    endpoint = {**_rich_keys()[1], "incoming": {"kind": "none"}}
    case.mutate("animation.set_keys", document, **target, keys=[endpoint, _simple(0, 0)])
    case.assertEqual(_sample(case, document, target, [6, 15]), [60, 140])
    case.mutate("animation.set_keys", document, **target, keys=_rich_keys()[:2][::-1])
    case.assertEqual(_sample(case, document, target, [6, 15]), [51, 140])


def rich_animation_delete_keys_obeys_control_kinds(case: NativeAcceptance) -> None:
    """Reject control reinterpretation and atomically handle first/last key deletion."""
    document = case.create()
    target = _peg(case, document)
    case.mutate("animation.set_keys", document, **target, keys=_rich_keys())
    before = _inspect(case, document, target)
    for frames in ([12], [24], [0, 999]):
        case.failure(
            "animation.delete_keys",
            case.write_params(document, {**target, "frames": frames}),
        )
        case.assertEqual(_inspect(case, document, target), before)
    case.mutate("animation.delete_keys", document, **target, frames=[0])
    keys = _inspect(case, document, target)["keys"]
    case.assertEqual([key["frame"] for key in keys], [12, 24, 36])
    case.assertEqual(keys[0]["incoming"], {"kind": "none"})
    case.assertEqual(_sample(case, document, target, [0, 6, 12, 18]), [120, 120, 120, 180])
    case.mutate("animation.delete_keys", document, **target, frames=[36])
    case.assertEqual([key["frame"] for key in _inspect(case, document, target)["keys"]], [12, 24])
