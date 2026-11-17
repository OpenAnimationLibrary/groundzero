"""Qualify native FX copies, bypass state, and published presets through real pixels."""

from __future__ import annotations

import base64
import hashlib
import json
from collections.abc import Sequence
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_png import Pixel, decode_png, fixture_pixel

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


COLORS: dict[int, Pixel] = {
    0: (24, 72, 192, 255),
    6: (112, 56, 104, 255),
    12: (200, 40, 16, 255),
}

NATIVE_PRESETS: dict[str, dict[str, Any]] = {
    "colorCardFx": {"color": [24, 72, 192, 255]},
    "STD_iwa_LinearGradientFx": {
        "startPoint": [-4, 0],
        "endPoint": [4, 0],
        "startColor": [24, 72, 192, 255],
        "endColor": [200, 40, 16, 255],
    },
    "STD_radialGradientFx": {
        "period": 5,
        "innerperiod": 1,
        "color1": [24, 72, 192, 255],
        "color2": [200, 40, 16, 255],
    },
    "overFx": {},
    "STD_blurFx": {"value": 1},
    "STD_glowFx": {"value": 1, "brightness": 25, "fade": 100, "color": [40, 80, 160, 255]},
    "STD_brightContFx": {"brightness": 40, "contrast": 30},
    "STD_hsvScaleFx": {"hue": 90, "value_scale": 80},
    "multFx": {"value": -64},
    "screenFx": {},
    "inFx": {},
    "outFx": {},
    "invertFx": {},
    "blendFx": {},
}

ORIGINAL_TYPES = (
    "colorCardFx",
    "STD_iwa_LinearGradientFx",
    "STD_radialGradientFx",
    "overFx",
    "STD_blurFx",
    "STD_glowFx",
)


def _close_worker(case: NativeAcceptance) -> None:
    case.worker.close()
    diagnostics = case.worker.log_path.read_text(encoding="utf-8", errors="replace")
    case.assertEqual(case.worker.process.returncode, 0, diagnostics)


def _scene(case: NativeAcceptance) -> str:
    return case.mutate(
        "scene.create",
        None,
        name="FX authoring",
        width_px=32,
        height_px=32,
        width_in=0.25,
        height_in=0.25,
        frame_rate=24,
    )["document_id"]


def _graph(case: NativeAcceptance, document: str) -> dict[str, Any]:
    return case.success("fx.graph", {"document_id": document})


def _node(case: NativeAcceptance, document: str, effect: str) -> dict[str, Any]:
    return next(node for node in _graph(case, document)["effects"] if node["fx_id"] == effect)


def _output(case: NativeAcceptance, document: str, effect: str) -> None:
    case.mutate(
        "fx.connect",
        document,
        fx_id=_graph(case, document)["current_output_fx_id"],
        port="source",
        source_fx_id=effect,
    )


def _pixels(case: NativeAcceptance, document: str, frame: int) -> list[Pixel]:
    job = case.mutate("render.preview", document, frame=frame, width=32, height=32)
    status = case.wait_job(job["job_id"])
    case.assertEqual(status["status"], "completed", status)
    case.assertTrue(status["writers_closed"])
    artifacts = case.success("render.artifacts", {"job_id": job["job_id"]})["artifacts"]
    case.assertEqual(len(artifacts), 1)
    image = case.success("artifact.read", {"artifact_id": artifacts[0]["artifact_id"]})
    data = base64.b64decode(image["image"]["data"], validate=True)
    case.assertEqual(hashlib.sha256(data).hexdigest(), artifacts[0]["sha256"])
    width, height, pixels = decode_png(data)
    case.assertEqual((width, height), (32, 32))
    case.assertEqual(image["source_revision"], case.revisions[document])
    return pixels


def _color_keys(case: NativeAcceptance, document: str, effect: str) -> None:
    for frame in (0, 12):
        case.mutate(
            "fx.parameter.set_key",
            document,
            fx_id=effect,
            parameter="color",
            frame=frame,
            value=list(COLORS[frame]),
        )


def _assert_colors(case: NativeAcceptance, document: str) -> None:
    for frame, color in COLORS.items():
        case.assertEqual(set(_pixels(case, document, frame)), {color}, frame)


def _reopen(case: NativeAcceptance, document: str) -> tuple[str, dict[str, str]]:
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "fx-scene"))
    case.mutate("scene.discard", document)
    restored = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    names = {node["name"]: node["fx_id"] for node in _graph(case, restored)["effects"]}
    return restored, names


def fx_duplicate_and_enable_preserve_native_roundtrip(case: NativeAcceptance) -> None:
    """Keep copied animation independent, generator exposure ownership, and native bypass."""
    document = _scene(case)
    source = case.mutate("fx.create", document, type="colorCardFx", name="Source", frame_count=13)
    _color_keys(case, document, source["fx_id"])
    copy = case.mutate(
        "fx.duplicate",
        document,
        fx_id=source["fx_id"],
        name="独立 copy",
        frame_count=13,
    )
    case.assertNotEqual(copy["fx_id"], source["fx_id"])
    case.assertNotEqual(copy["column_id"], source["column_id"])
    case.assertEqual(copy["native_fx_version"], source["native_fx_version"])
    case.assertEqual(copy["ports"], [])
    case.assertFalse(copy["terminal"])
    cells = case.success(
        "column.cells",
        {
            "document_id": document,
            "column_id": copy["column_id"],
            "start_frame": 0,
            "length": 14,
        },
    )["cells"]
    case.assertEqual([cell["empty"] for cell in cells], [False] * 13 + [True])
    _output(case, document, copy["fx_id"])
    _assert_colors(case, document)
    case.mutate("fx.rename", document, fx_id=copy["fx_id"], name="Independent")
    revision = case.revisions[document]
    case.assertFalse(
        case.mutate("fx.rename", document, fx_id=copy["fx_id"], name="Independent")["changed"]
    )
    case.assertEqual(case.revisions[document], revision)
    case.mutate(
        "fx.parameter.set_key",
        document,
        fx_id=copy["fx_id"],
        parameter="color",
        frame=0,
        value=[240, 16, 80, 255],
    )
    case.assertEqual(set(_pixels(case, document, 0)), {(240, 16, 80, 255)})
    _output(case, document, source["fx_id"])
    _assert_colors(case, document)
    case.mutate("fx.enabled.set", document, fx_id=source["fx_id"], enabled=False)
    case.assertEqual(set(_pixels(case, document, 6)), {(0, 0, 0, 0)})
    revision = case.revisions[document]
    case.assertFalse(
        case.mutate("fx.enabled.set", document, fx_id=source["fx_id"], enabled=False)["changed"]
    )
    case.assertEqual(case.revisions[document], revision)
    case.mutate("fx.enabled.set", document, fx_id=source["fx_id"], enabled=True)
    restored, names = _reopen(case, document)
    _assert_colors(case, restored)
    _output(case, restored, names["Independent"])
    case.assertEqual(set(_pixels(case, restored, 0)), {(240, 16, 80, 255)})
    _close_worker(case)


def _save(case: NativeAcceptance, document: str, effect: str, name: str) -> dict[str, Any]:
    revision = case.revisions[document]
    result = case.mutate(
        "fx.preset.save", document, fx_id=effect, directory=str(case.workspace / name)
    )
    case.assertEqual(case.revisions[document], revision)
    manifest = result["manifest"]
    path = Path(result["preset_directory"])
    case.assertEqual(json.loads((path / "manifest.json").read_text()), manifest)
    data = (path / "effect.preset").read_bytes()
    case.assertEqual(manifest["sha256"], hashlib.sha256(data).hexdigest())
    case.assertEqual(manifest["size_bytes"], len(data))
    case.assertEqual(manifest["source_revision"], revision)
    return result


def _artwork_fixture(case: NativeAcceptance) -> tuple[str, str]:
    document = _scene(case)
    level = case.mutate("level.import", document, path=str(case.fixture), name="Quadrants", dpi=128)
    column = case.mutate("column.create", document, name="Artwork")
    case.mutate(
        "column.expose",
        document,
        column_id=column["column_id"],
        level_id=level["level_id"],
        drawing_id={"number": 1, "suffix": ""},
        start_frame=0,
        length=13,
    )
    source = next(
        node["fx_id"]
        for node in _graph(case, document)["effects"]
        if node.get("column_id") == column["column_id"]
    )
    return document, source


def _blur_fixture(case: NativeAcceptance) -> tuple[str, str, str]:
    document, source = _artwork_fixture(case)
    blur = case.mutate("fx.create", document, type="STD_blurFx", name="Original blur")["fx_id"]
    case.mutate("fx.connect", document, fx_id=blur, port="Source", source_fx_id=source)
    _output(case, document, blur)
    return document, source, blur


def fx_presets_preserve_keys_and_target_graph(case: NativeAcceptance) -> None:
    """Transfer a keyed native blur while retaining target routing and exact bypass pixels."""
    document, artwork, blur = _blur_fixture(case)
    for frame, value in ((0, 0), (12, 1)):
        case.mutate(
            "fx.parameter.set_key",
            document,
            fx_id=blur,
            parameter="value",
            frame=frame,
            value=value,
        )
    original = {frame: _pixels(case, document, frame) for frame in COLORS}
    expected = [fixture_pixel(x, y, 32) for y in range(32) for x in range(32)]
    case.assertEqual(original[0], expected)
    case.assertNotEqual(original[6], original[0])
    case.assertNotEqual(original[12], original[6])
    preset = _save(case, document, blur, "blur-look")
    duplicate = case.mutate("fx.duplicate", document, fx_id=blur, name="Disconnected blur")
    case.assertTrue(all(not port["connected"] for port in duplicate["ports"]))
    case.assertFalse(duplicate["terminal"])
    target = case.mutate("fx.create", document, type="STD_blurFx", name="Target blur")["fx_id"]
    case.mutate("fx.connect", document, fx_id=target, port="Source", source_fx_id=artwork)
    _output(case, document, target)
    before = _node(case, document, target)
    applied = case.mutate(
        "fx.preset.apply", document, fx_id=target, directory=preset["preset_directory"]
    )
    case.assertTrue(applied["changed"])
    case.assertEqual(applied["affected_fx_ids"], [target])
    case.assertEqual(_node(case, document, target), before)
    for frame in COLORS:
        case.assertEqual(_pixels(case, document, frame), original[frame], frame)
    revision = case.revisions[document]
    case.assertFalse(
        case.mutate(
            "fx.preset.apply", document, fx_id=target, directory=preset["preset_directory"]
        )["changed"]
    )
    case.assertEqual(case.revisions[document], revision)
    case.mutate("fx.enabled.set", document, fx_id=target, enabled=False)
    case.assertEqual(_pixels(case, document, 12), expected)
    restored, names = _reopen(case, document)
    case.assertFalse(_node(case, restored, names["Target blur"])["enabled"])
    case.assertEqual(_pixels(case, restored, 12), expected)
    case.mutate("fx.enabled.set", restored, fx_id=names["Target blur"], enabled=True)
    for frame in COLORS:
        case.assertEqual(_pixels(case, restored, frame), original[frame], frame)
    _close_worker(case)


def _reject_unchanged(
    case: NativeAcceptance, document: str, method: str, params: dict[str, Any], code: str
) -> None:
    before = _graph(case, document)
    revision = case.revisions[document]
    target = {"document_id": document, "fx_id": params.get("fx_id"), "frame": 6}
    owned = any(node["fx_id"] == target["fx_id"] for node in before["effects"])
    parameters = case.success("fx.parameter.get", target) if owned else None
    error = case.failure(method, case.write_params(document, params))
    case.assertEqual(error["code"], code, error)
    case.assertEqual(_graph(case, document), before)
    if owned:
        case.assertEqual(case.success("fx.parameter.get", target), parameters)
    case.assertEqual(case.success("scene.summary", {"document_id": document})["revision"], revision)


def fx_authoring_rejections_preserve_document(case: NativeAcceptance) -> None:
    """Reject invalid names, foreign handles, structural copies, stale revisions and locks."""
    document = _scene(case)
    card = case.mutate("fx.create", document, type="colorCardFx", frame_count=2)
    for name in ("", "x" * 257, "control\nname"):
        code = "INVALID_PARAMS" if "\n" in name else "invalid_params"
        _reject_unchanged(case, document, "fx.rename", {"fx_id": card["fx_id"], "name": name}, code)
    structural = _graph(case, document)["current_output_fx_id"]
    _reject_unchanged(case, document, "fx.duplicate", {"fx_id": structural}, "UNSUPPORTED_FX")
    blur = case.mutate("fx.create", document, type="STD_blurFx")["fx_id"]
    _reject_unchanged(
        case, document, "fx.duplicate", {"fx_id": blur, "frame_count": 2}, "INVALID_PARAMS"
    )
    other = _scene(case)
    _reject_unchanged(
        case, other, "fx.rename", {"fx_id": card["fx_id"], "name": "Foreign"}, "invalid_handle"
    )
    stale = case.write_params(document, {"fx_id": card["fx_id"], "enabled": False})
    case.mutate("fx.rename", document, fx_id=card["fx_id"], name="Renamed")
    case.assertEqual(case.failure("fx.enabled.set", stale)["code"], "revision_conflict")
    case.mutate("column.update", document, column_id=card["column_id"], locked=True)
    for method, values in (
        ("fx.rename", {"name": "Locked"}),
        ("fx.enabled.set", {"enabled": False}),
        ("fx.duplicate", {}),
    ):
        _reject_unchanged(case, document, method, {"fx_id": card["fx_id"], **values}, "locked")
    _close_worker(case)


def fx_preset_integrity_and_file_receipts(case: NativeAcceptance) -> None:
    """Reject tampering before mutation and replay exact file receipts after source edits."""
    document = _scene(case)
    card = case.mutate("fx.create", document, type="colorCardFx", frame_count=13)["fx_id"]
    _color_keys(case, document, card)
    target = case.mutate("fx.create", document, type="colorCardFx", frame_count=13)["fx_id"]
    params = case.write_params(
        document, {"fx_id": card, "directory": str(case.workspace / "color-look")}
    )
    saved = case.success("fx.preset.save", params)
    case.assertEqual(saved["revision"], case.revisions[document])
    case.mutate("fx.rename", document, fx_id=card, name="Edited after file publication")
    case.assertEqual(case.success("fx.preset.save", params), saved)
    directory = Path(saved["preset_directory"])
    _output(case, document, target)
    before = _node(case, document, target)
    case.mutate("fx.preset.apply", document, fx_id=target, directory=str(directory))
    case.assertEqual(_node(case, document, target), before)
    _assert_colors(case, document)
    case.mutate(
        "fx.parameter.set_key",
        document,
        fx_id=card,
        parameter="color",
        frame=0,
        value=[7, 9, 11, 255],
    )
    case.assertEqual(set(_pixels(case, document, 0)), {COLORS[0]})
    _preset_path_rejections(case, document, target, directory)
    for filename, replacement, code in (
        ("manifest.json", b" ", "INVALID_PRESET"),
        ("effect.preset", b"altered", "INVALID_PRESET"),
        ("effect.preset", b"x" * (1024 * 1024 + 1), "RESOURCE_LIMIT"),
    ):
        path = directory / filename
        previous = path.read_bytes()
        path.write_bytes(replacement)
        _reject_unchanged(
            case, document, "fx.preset.apply", {"fx_id": target, "directory": str(directory)}, code
        )
        path.write_bytes(previous)
    restored, names = _reopen(case, document)
    _assert_colors(case, restored)
    case.assertIn("Edited after file publication", names)
    other = _scene(case)
    independent = case.mutate("fx.create", other, type="colorCardFx", frame_count=13)["fx_id"]
    case.mutate("fx.preset.apply", other, fx_id=independent, directory=str(directory))
    _output(case, other, independent)
    _assert_colors(case, other)
    _close_worker(case)


def _preset_path_rejections(
    case: NativeAcceptance, document: str, effect: str, directory: Path
) -> None:
    _reject_unchanged(
        case,
        document,
        "fx.preset.save",
        {"fx_id": effect, "directory": str(directory)},
        "ALREADY_EXISTS",
    )
    _reject_unchanged(
        case,
        document,
        "fx.preset.save",
        {"fx_id": effect, "directory": "relative-preset"},
        "INVALID_PARAMS",
    )
    _reject_unchanged(
        case,
        document,
        "fx.preset.save",
        {"fx_id": effect, "directory": str(case.inputs / "denied")},
        "path_denied",
    )
    _reject_unchanged(
        case,
        document,
        "fx.preset.apply",
        {"fx_id": effect, "directory": str(case.workspace / "unregistered")},
        "UNREGISTERED_PRESET",
    )
    blur = case.mutate("fx.create", document, type="STD_blurFx")["fx_id"]
    _reject_unchanged(
        case,
        document,
        "fx.preset.apply",
        {"fx_id": blur, "directory": str(directory)},
        "INCOMPATIBLE_PRESET",
    )
    extra = directory / "unrecorded.txt"
    extra.write_text("unexpected file", encoding="utf-8")
    _reject_unchanged(
        case,
        document,
        "fx.preset.apply",
        {"fx_id": effect, "directory": str(directory)},
        "INVALID_PRESET",
    )
    extra.unlink()


def fx_preset_capacity_keeps_exact_replay(case: NativeAcceptance) -> None:
    """Enforce preset and receipt quotas without invalidating exact file publication retries."""
    document = _scene(case)
    effect = case.mutate("fx.create", document, type="STD_blurFx")["fx_id"]
    first_params = case.write_params(
        document, {"fx_id": effect, "directory": str(case.workspace / "preset-0")}
    )
    first = case.success("fx.preset.save", first_params)
    for index in range(1, 128):
        _save(case, document, effect, f"preset-{index}")
    _reject_unchanged(
        case,
        document,
        "fx.preset.save",
        {"fx_id": effect, "directory": str(case.workspace / "one-too-many")},
        "RESOURCE_LIMIT",
    )
    case.assertFalse((case.workspace / "one-too-many").exists())
    case.mutate("fx.rename", document, fx_id=effect, name="Revision after capacity")
    case.assertEqual(case.success("fx.preset.save", first_params), first)
    for _index in range(2048):
        params = case.write_params(document, {"fx_id": effect, "name": "Revision after capacity"})
        response = case.worker.call("fx.rename", params)
        if "error" in response:
            case.assertEqual(response["error"]["code"], "resource_limit", response)
            break
        case.assertEqual(response["result"]["revision"], case.revisions[document])
    else:
        case.fail("Ordinary receipt limit was not enforced")
    case.assertEqual(case.success("fx.preset.save", first_params), first)
    _close_worker(case)


def _wired_effect(case: NativeAcceptance, document: str, kind: str, artwork: str) -> str:
    node = case.mutate("fx.create", document, type=kind, frame_count=13)
    for port in node["ports"]:
        case.mutate(
            "fx.connect", document, fx_id=node["fx_id"], port=port["name"], source_fx_id=artwork
        )
    return node["fx_id"]


def presets_cover_native_types(case: NativeAcceptance, kinds: Sequence[str]) -> None:
    """Compare actual copied and reopened renders for each listed native preset factory.

    Each type takes four preview renders, so keep a call within the worker's 32-job limit.

    Args:
        case: A fresh native worker acceptance fixture.
        kinds: Native FX types from NATIVE_PRESETS to cover.
    """
    document, artwork = _artwork_fixture(case)
    references = {}
    parameters = {}
    for kind in kinds:
        values = NATIVE_PRESETS[kind]
        source = _wired_effect(case, document, kind, artwork)
        _output(case, document, source)
        baseline = _pixels(case, document, 6)
        for parameter, value in values.items():
            case.mutate(
                "fx.parameter.set", document, fx_id=source, parameter=parameter, value=value
            )
        references[kind] = _pixels(case, document, 6)
        if values:
            case.assertNotEqual(references[kind], baseline, kind)
        else:
            case.assertEqual(references[kind], baseline, kind)
        parameters[kind] = case.success(
            "fx.parameter.get", {"document_id": document, "fx_id": source, "frame": 6}
        )["parameters"]
        preset = _save(case, document, source, "native-" + kind)
        target = _wired_effect(case, document, kind, artwork)
        case.mutate("fx.rename", document, fx_id=target, name="Target " + kind)
        graph = _node(case, document, target)
        applied = case.mutate(
            "fx.preset.apply", document, fx_id=target, directory=preset["preset_directory"]
        )
        case.assertEqual(applied["changed"], bool(values), kind)
        case.assertEqual(_node(case, document, target), graph)
        _output(case, document, target)
        case.assertEqual(_pixels(case, document, 6), references[kind], kind)
    restored, names = _reopen(case, document)
    for kind in kinds:
        target = names["Target " + kind]
        actual = case.success(
            "fx.parameter.get", {"document_id": restored, "fx_id": target, "frame": 6}
        )["parameters"]
        case.assertEqual(actual, parameters[kind], kind)
        _output(case, restored, target)
        case.assertEqual(_pixels(case, restored, 6), references[kind], kind)
    _close_worker(case)


def fx_presets_cover_each_admitted_native_type(case: NativeAcceptance) -> None:
    """Require the full admitted catalog and replay presets for the original six factories."""
    catalog = case.success("fx.catalog", {})
    case.assertEqual({effect["type"] for effect in catalog["effects"]}, set(NATIVE_PRESETS))
    presets_cover_native_types(case, ORIGINAL_TYPES)
