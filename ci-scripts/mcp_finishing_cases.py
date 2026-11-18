"""Qualify the finishing FX profile: bounds, constraints, render preflight and provenance."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from mcp_fx_authoring_cases import (
    _close_worker,
    _graph,
    _output,
    _pixels,
    _reject_unchanged,
    _reopen,
    _scene,
    presets_cover_native_types,
)
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

PROFILE = "builtin-2d-v2"
ADMITTED_TYPES = 31
GENERATORS = {
    "STD_radialGradientFx",
    "STD_iwa_LinearGradientFx",
    "STD_lightSpotFx",
    "STD_targetSpotFx",
}
# Admitted ranges that differ from the native ranges; points are bounded per axis.
BOUNDS: dict[str, dict[str, tuple[float, float]]] = {
    "STD_blurFx": {"value": (0, 200)},
    "STD_glowFx": {"value": (0, 200), "brightness": (0, 1000)},
    "STD_radialGradientFx": {"period": (0.01, 1e5), "innerperiod": (0, 1e5)},
    "STD_iwa_LinearGradientFx": {
        "startPoint": (-1e5, 1e5),
        "wave_amplitude": (0, 1e4),
        "wave_frequency": (-1e4, 1e4),
        "wave_phase": (-1e4, 1e4),
    },
    "STD_iwa_BloomFx": {"size": (0.1, 200)},
    "STD_lightWrapFx": {"width": (0, 50), "softness": (0, 20)},
    "STD_backlitFx": {"value": (0, 100)},
    "STD_bodyHighLightFx": {"point": (-100, 100), "blur": (0, 100)},
    "STD_lightSpotFx": {"softness": (0, 10), "a": (1, 5000), "b": (1, 5000)},
    "STD_targetSpotFx": {
        "z": (0, 1e4),
        "angle": (-89, 89),
        "sizeX": (0.01, 100),
        "sizeY": (0.01, 100),
    },
    "STD_directionalBlurFx": {"intensity": (0, 200), "angle": (-360, 360)},
    "STD_localBlurFx": {"value": (0, 200)},
    "STD_solarizeFx": {"maximum": (0, 1.99), "peak_edge": (1, 255)},
    "STD_sharpenFx": {"intensity": (0, 1000)},
    "STD_embossFx": {"radius": (0.5, 5)},
    "STD_erodeDilateFx": {"radius": (-50, 50)},
    "STD_matteChokeFx": {"amount": (-20, 20), "feather": (0, 20)},
    "STD_gammaFx": {"value": (0.1, 10)},
    "STD_iwa_AdjustExposureFx": {"scale": (-3, 3)},
}
REQUIRED_INPUTS = {
    "STD_backlitFx": "Light",
    "STD_lightWrapFx": "Background",
    "STD_localBlurFx": "Reference",
}


def _create(case: NativeAcceptance, document: str, kind: str) -> str:
    extra = {"frame_count": 13} if kind in GENERATORS else {}
    return case.mutate("fx.create", document, type=kind, **extra)["fx_id"]


def _schemas(case: NativeAcceptance, document: str, effect: str) -> dict[str, Any]:
    params = {"document_id": document, "fx_id": effect, "frame": 0}
    return {item["name"]: item for item in case.success("fx.parameter.get", params)["parameters"]}


def _value(schema: dict[str, Any], number: float) -> Any:
    return [number, number] if schema["type"] == "array" else number


def _check_bound(
    case: NativeAcceptance, document: str, effect: str, name: str, bound: tuple[float, float]
) -> None:
    schema = _schemas(case, document, effect)[name]["schema"]
    limits = schema["items"] if schema["type"] == "array" else schema
    case.assertEqual((limits["minimum"], limits["maximum"]), bound, name)
    for number in (bound[0] - 0.001, bound[1] + 0.001):
        params = {"fx_id": effect, "parameter": name, "value": _value(schema, number)}
        _reject_unchanged(case, document, "fx.parameter.set", params, "OUT_OF_RANGE")
        keyed = {**params, "frame": 3}
        _reject_unchanged(case, document, "fx.parameter.set_key", keyed, "OUT_OF_RANGE")
    for number in bound:
        value = _value(schema, number)
        case.mutate("fx.parameter.set", document, fx_id=effect, parameter=name, value=value)
        case.assertEqual(_schemas(case, document, effect)[name]["value"], value, name)


def _check_catalog(case: NativeAcceptance) -> None:
    catalog = case.success("fx.catalog", {})
    case.assertEqual(catalog["profile"], PROFILE)
    effects = {entry["type"]: entry for entry in catalog["effects"]}
    case.assertEqual(len(effects), ADMITTED_TYPES)
    blur = effects["STD_blurFx"]
    case.assertEqual(
        blur["extent_parameters"], [{"name": "value", "coefficient": 1, "max_px": 1024}]
    )
    case.assertEqual((blur["seed_parameters"], blur["replay_class"]), ([], "stateless"))
    case.assertEqual(effects["STD_lightWrapFx"]["required_inputs"], ["Background"])
    case.assertEqual(effects["STD_iwa_BloomFx"]["native_fx_version"], 3)
    separation = effects["STD_iwa_LinearGradientFx"]["constraints"][0]
    case.assertEqual(separation["id"], "linear_gradient_separation")


def finishing_bounds_reject_before_mutation(case: NativeAcceptance) -> None:
    """Advertise every tightened range and reject values past it without changing the scene."""
    _check_catalog(case)
    document = _scene(case)
    for kind, bounds in BOUNDS.items():
        effect = _create(case, document, kind)
        for name, bound in bounds.items():
            _check_bound(case, document, effect, name, bound)
    wrap = _create(case, document, "STD_lightWrapFx")
    params = {"fx_id": wrap, "parameter": "blend", "value": 0.5}
    _reject_unchanged(case, document, "fx.parameter.set", params, "INVALID_PARAMS")
    case.mutate("fx.parameter.set", document, fx_id=wrap, parameter="blend", value=1)
    highlight = _create(case, document, "STD_bodyHighLightFx")
    params = {"fx_id": highlight, "parameter": "color", "value": [255, 255, 255, 128]}
    _reject_unchanged(case, document, "fx.parameter.set", params, "OUT_OF_RANGE")
    _close_worker(case)


def _separation_failure(
    case: NativeAcceptance, document: str, method: str, params: dict[str, Any], frame: float
) -> None:
    _reject_unchanged(case, document, method, params, "PARAMETER_CONSTRAINT")
    error = case.failure(method, case.write_params(document, params))
    details = error["details"]
    case.assertEqual(details["constraint"], "linear_gradient_separation")
    case.assertEqual(details["parameters"], ["startPoint", "endPoint"])
    case.assertEqual(details["fx_id"], params["fx_id"])
    case.assertAlmostEqual(details["frame"], frame, places=6)


def finishing_gradient_separation_rejects_before_mutation(case: NativeAcceptance) -> None:
    """Reject linear-gradient points that come within 0.001 at any frame, keys included."""
    document = _scene(case)
    gradient = _create(case, document, "STD_iwa_LinearGradientFx")
    near = {"fx_id": gradient, "parameter": "endPoint", "value": [-50.0005, 0]}
    _separation_failure(case, document, "fx.parameter.set", near, 0)
    case.mutate("fx.parameter.set", document, fx_id=gradient, parameter="endPoint", value=[-50, 0])
    case.mutate(
        "fx.parameter.set", document, fx_id=gradient, parameter="endPoint", value=[5, 0.0011]
    )
    for frame, x in ((0, 0), (10, 10)):
        case.mutate(
            "fx.parameter.set_key",
            document,
            fx_id=gradient,
            parameter="startPoint",
            frame=frame,
            value=[x, 0],
        )
    crossing = {"fx_id": gradient, "parameter": "endPoint", "value": [5, 0]}
    _separation_failure(case, document, "fx.parameter.set", crossing, 5)
    restored, names = _reopen(case, document)
    case.assertEqual(len(names), len(_graph(case, restored)["effects"]))
    _close_worker(case)


def _artwork(case: NativeAcceptance) -> tuple[str, str, str]:
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
    return document, column["object_id"], source


def _preflight_failure(
    case: NativeAcceptance, document: str, method: str, params: dict[str, Any]
) -> dict[str, Any]:
    revision = case.revisions[document]
    error = case.failure(method, case.write_params(document, params))
    case.assertEqual(error["code"], "fx_preflight_failed", error)
    case.assertEqual(case.success("scene.summary", {"document_id": document})["revision"], revision)
    return error["details"]


def _scale_keys(
    case: NativeAcceptance, document: str, target: str, channel: str, keys: Any
) -> None:
    frames = [{"frame": frame, "value": value, "interpolation": "linear"} for frame, value in keys]
    case.mutate(
        "animation.set_keys",
        document,
        object_id=target,
        channel=channel,
        unit="scalar",
        keys=frames,
    )


def finishing_preflight_bounds_render_extent(case: NativeAcceptance) -> None:
    """Reject renders whose scaled extent passes the limit, at the first failing frame."""
    document, target, artwork = _artwork(case)
    blur = _create(case, document, "STD_blurFx")
    case.mutate("fx.connect", document, fx_id=blur, port="Source", source_fx_id=artwork)
    case.mutate("fx.parameter.set", document, fx_id=blur, parameter="value", value=200)
    _output(case, document, blur)
    job = case.mutate("render.preview", document, frame=0, width=32, height=32)
    provenance: list[dict[str, Any]] = [
        {
            "fx_id": blur,
            "type": "STD_blurFx",
            "native_fx_version": 1,
            "replay_class": "stateless",
            "seeds": {},
        }
    ]
    case.assertEqual(job["fx_provenance"], provenance)
    case.assertEqual(case.wait_job(job["job_id"])["fx_provenance"], provenance)
    large = {"start_frame": 0, "end_frame": 0, "width": 2048, "height": 2048}
    details = _preflight_failure(case, document, "render.submit", large)
    case.assertEqual((details["reason"], details["fx_id"]), ("extent", blur))
    case.assertEqual((details["parameter"], details["frame"]), ("value", 0))
    case.assertEqual(details["max_px"], 1024)
    case.assertGreater(details["extent_px"], 1024)
    case.mutate("fx.parameter.set", document, fx_id=blur, parameter="value", value=4)
    _scale_keys(case, document, target, "scale", ((0, 1), (12, 8)))
    sequence = {"start_frame": 0, "end_frame": 12, "width": 512, "height": 512}
    details = _preflight_failure(case, document, "render.submit", sequence)
    case.assertEqual(details["frame"], 10)
    stepped = _preflight_failure(case, document, "render.submit", {**sequence, "step": 4})
    case.assertEqual(stepped["frame"], 12)
    _close_worker(case)


def finishing_preflight_requires_unmirrored_erosion(case: NativeAcceptance) -> None:
    """Reject erode/dilate under a mirrored placement before rendering, then render unmirrored."""
    document, target, artwork = _artwork(case)
    erode = _create(case, document, "STD_erodeDilateFx")
    case.mutate("fx.connect", document, fx_id=erode, port="Source", source_fx_id=artwork)
    _output(case, document, erode)
    _scale_keys(case, document, target, "scale_x", ((0, -1),))
    details = _preflight_failure(case, document, "render.preview", {"frame": 0})
    case.assertEqual((details["reason"], details["fx_id"]), ("positive_determinant", erode))
    _scale_keys(case, document, target, "scale_x", ((0, 1),))
    case.assertEqual(len(_pixels(case, document, 0)), 32 * 32)
    _close_worker(case)


def finishing_required_inputs_are_diagnosed(case: NativeAcceptance) -> None:
    """Report REQUIRED_INPUT_MISSING until the effect's required input is connected."""
    document = _scene(case)
    card = _create(case, document, "colorCardFx")
    for kind, port in REQUIRED_INPUTS.items():
        node = case.mutate("fx.create", document, type=kind)
        case.assertEqual([item["code"] for item in node["diagnostics"]], ["REQUIRED_INPUT_MISSING"])
        case.assertEqual(node["diagnostics"][0]["port"], port)
        connected = case.mutate(
            "fx.connect", document, fx_id=node["fx_id"], port=port, source_fx_id=card
        )
        case.assertEqual(connected["diagnostics"], [])
    _close_worker(case)


def _deadline_worker(case: NativeAcceptance, milliseconds: int) -> None:
    case.worker.close()
    case.workspace = case.directory / "deadline-workspace"
    case.workspace.mkdir()
    case.worker = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / "deadline-worker.stderr.log",
        test_arguments=("--test-render-deadline-ms", str(milliseconds)),
    )
    case.addCleanup(case.worker.close)


def finishing_render_deadline_stops_slow_job(case: NativeAcceptance) -> None:
    """End a slow light-wrap render at its deadline without artifacts and keep serving."""
    _deadline_worker(case, 1500)
    document, _target, artwork = _artwork(case)
    wrap = _create(case, document, "STD_lightWrapFx")
    for port in ("Source", "Background"):
        case.mutate("fx.connect", document, fx_id=wrap, port=port, source_fx_id=artwork)
    case.mutate("fx.parameter.set", document, fx_id=wrap, parameter="softness", value=20)
    _output(case, document, wrap)
    slow = {"start_frame": 0, "end_frame": 12, "width": 192, "height": 192}
    job = case.mutate("render.submit", document, **slow)
    ended = case.wait_job(job["job_id"])
    case.assertEqual(ended["status"], "deadline_exceeded", ended)
    case.assertTrue(ended["writers_closed"])
    case.assertLess(ended["completed_frames"], ended["frame_count"])
    case.assertIn("deadline", ended["error"])
    error = case.failure("job.result", {"job_id": job["job_id"]})
    case.assertEqual(error["code"], "artifacts_unavailable")
    case.mutate("fx.enabled.set", document, fx_id=wrap, enabled=False)
    case.assertEqual(len(_pixels(case, document, 0)), 32 * 32)
    _close_worker(case)


def _soft_disk(case: NativeAcceptance, document: str) -> str:
    """Create a source with intermediate channel values and a soft alpha edge.

    The quadrant artwork holds only 0 and 255, which tonal effects such as gamma,
    posterize and sharpen leave unchanged, and has no edge for backlit light.
    """
    values = {
        "period": 6,
        "innerperiod": 1,
        "color1": [220, 150, 70, 255],
        "color2": [30, 60, 160, 0],
    }
    disk = _create(case, document, "STD_radialGradientFx")
    for name, value in values.items():
        case.mutate("fx.parameter.set", document, fx_id=disk, parameter=name, value=value)
    return disk


def light_presets_cover_native_types(case: NativeAcceptance) -> None:
    """Replay presets, copies and reopened renders for the six light and glow finishing FX."""
    presets_cover_native_types(
        case,
        (
            "STD_iwa_BloomFx",
            "STD_lightWrapFx",
            "STD_backlitFx",
            "STD_bodyHighLightFx",
            "STD_lightSpotFx",
            "STD_targetSpotFx",
        ),
        _soft_disk,
    )


def stylize_presets_cover_native_types(case: NativeAcceptance) -> None:
    """Replay presets, copies and reopened renders for the six blur and stylize finishing FX."""
    presets_cover_native_types(
        case,
        (
            "STD_directionalBlurFx",
            "STD_localBlurFx",
            "STD_posterizeFx",
            "STD_solarizeFx",
            "STD_sharpenFx",
            "STD_embossFx",
        ),
        _soft_disk,
    )


def grade_presets_cover_native_types(case: NativeAcceptance) -> None:
    """Replay presets, copies and reopened renders for the five matte and grade finishing FX."""
    presets_cover_native_types(
        case,
        (
            "STD_erodeDilateFx",
            "STD_matteChokeFx",
            "STD_channelMixerFx",
            "STD_gammaFx",
            "STD_iwa_AdjustExposureFx",
        ),
        _soft_disk,
    )
