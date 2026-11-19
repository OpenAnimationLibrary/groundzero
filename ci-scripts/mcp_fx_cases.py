"""Verify selected FX branches and animated colors through native render and persistence."""

from __future__ import annotations

import base64
import hashlib
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_failure_cases import _fault_worker
from mcp_png import Pixel, decode_png

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


COLORS: dict[int, Pixel] = {
    0: (24, 72, 192, 255),
    2: (112, 56, 104, 255),
    4: (200, 40, 16, 255),
}
OTHER_COLOR: Pixel = (17, 149, 83, 255)


def _card(case: NativeAcceptance, document: str, name: str) -> str:
    return case.mutate("fx.create", document, type="colorCardFx", name=name, frame_count=5)["fx_id"]


def _color(case: NativeAcceptance, document: str, effect: str, color: Pixel) -> None:
    case.mutate("fx.parameter.set", document, fx_id=effect, parameter="color", value=list(color))


def _connect(case: NativeAcceptance, document: str, output: str, source: str) -> None:
    case.mutate("fx.connect", document, fx_id=output, port="source", source_fx_id=source)


def _reopen(case: NativeAcceptance, document: str) -> str:
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "saved-fx"))
    case.mutate("scene.discard", document)
    opened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    case.assertNotEqual(opened, document)
    return opened


def _assert_preview(case: NativeAcceptance, document: str, color: Pixel) -> None:
    width, height, pixels = decode_png(case.preview(document))
    case.assertEqual((width, height), (32, 32))
    case.assertEqual(set(pixels), {color})


@acceptance_case(name="fx_selected_output_survives_reopen")
def selected_output_survives_reopen(case: NativeAcceptance) -> None:
    """Render distinct output branches, persist the selection, and delete the selected output.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Output branches")
    first = _card(case, document, "First card")
    second = _card(case, document, "Second card")
    _color(case, document, first, COLORS[0])
    _color(case, document, second, OTHER_COLOR)
    graph = case.success("fx.graph", {"document_id": document})
    original_output = graph["current_output_fx_id"]
    output = case.mutate("fx.create", document, type="output", name="Second output")["fx_id"]
    _connect(case, document, original_output, first)
    _connect(case, document, output, second)
    case.mutate("fx.output.select", document, fx_id=output)
    _assert_preview(case, document, OTHER_COLOR)
    case.mutate("fx.output.select", document, fx_id=original_output)
    _assert_preview(case, document, COLORS[0])
    case.mutate("fx.output.select", document, fx_id=output)
    reopened = _reopen(case, document)
    restored = case.success("fx.graph", {"document_id": reopened})
    effects = {effect["fx_id"]: effect for effect in restored["effects"]}
    selected = effects[restored["current_output_fx_id"]]
    case.assertEqual(selected["name"], "Second output")
    source = next(port for port in selected["ports"] if port["name"] == "source")
    case.assertEqual(effects[source["source_fx_id"]]["name"], "Second card")
    _assert_preview(case, reopened, OTHER_COLOR)
    case.mutate("fx.delete", reopened, fx_id=selected["fx_id"])
    _assert_preview(case, reopened, COLORS[0])


def _assert_key_values(case: NativeAcceptance, document: str, effect: str) -> None:
    for frame, color in COLORS.items():
        result = case.success(
            "fx.parameter.get", {"document_id": document, "fx_id": effect, "frame": frame}
        )
        parameter = next(item for item in result["parameters"] if item["name"] == "color")
        case.assertEqual(parameter["keyframes"], [0, 4])
        case.assertEqual(parameter["value"], list(color), frame)


def _assert_key_render(case: NativeAcceptance, document: str) -> None:
    revision = case.revisions[document]
    job = case.mutate(
        "render.submit", document, start_frame=0, end_frame=4, step=2, width=17, height=9
    )
    status = case.wait_job(job["job_id"])
    case.assertEqual(status["status"], "completed", status)
    case.assertTrue(status["writers_closed"])
    case.assertEqual(status["completed_frames"], 3)
    case.assertEqual(status["failed_frames"], 0)
    artifacts: list[dict[str, Any]] = case.success("job.result", {"job_id": job["job_id"]})[
        "artifacts"
    ]
    case.assertEqual([item["frame"] for item in artifacts], list(COLORS))
    for artifact in artifacts:
        result = case.success("artifact.read", {"artifact_id": artifact["artifact_id"]})
        case.assertEqual(result["document_id"], document)
        case.assertEqual(result["source_revision"], revision)
        case.assertEqual(result["frame"], artifact["frame"])
        data = base64.b64decode(result["image"]["data"], validate=True)
        case.assertEqual(hashlib.sha256(data).hexdigest(), artifact["sha256"])
        width, height, pixels = decode_png(data)
        case.assertEqual((width, height), (17, 9))
        case.assertEqual(set(pixels), {COLORS[artifact["frame"]]}, artifact["frame"])


@acceptance_case(name="fx_color_keys_survive_render_and_reopen")
def color_keys_survive_render_and_reopen(case: NativeAcceptance) -> None:
    """Exercise typed RGBA keys, linear interpolation, and all rendered channels after reopen.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Animated color")
    effect = _card(case, document, "Animated card")
    for frame in (0, 4):
        case.mutate(
            "fx.parameter.set_key",
            document,
            fx_id=effect,
            parameter="color",
            frame=frame,
            value=list(COLORS[frame]),
        )
    graph = case.success("fx.graph", {"document_id": document})
    _connect(case, document, graph["current_output_fx_id"], effect)
    _assert_key_values(case, document, effect)
    _assert_key_render(case, document)
    reopened = _reopen(case, document)
    restored = case.success("fx.graph", {"document_id": reopened})
    restored_effect = next(item for item in restored["effects"] if item["name"] == "Animated card")
    _assert_key_values(case, reopened, restored_effect["fx_id"])
    _assert_key_render(case, reopened)


def _graph_effect(case: NativeAcceptance, document: str, effect: str) -> dict[str, Any]:
    graph = case.success("fx.graph", {"document_id": document})
    return next(item for item in graph["effects"] if item["fx_id"] == effect)


@acceptance_case
def fx_key_on_a_non_animatable_parameter_is_refused(case: NativeAcceptance) -> None:
    """A key on a public parameter that cannot animate (an enum and a bool) is refused before
    any change; the parameter keeps no key, its value and the revision stay, and a plain set
    of the same value still works."""
    document = case.create("Non-animatable keys")
    gradient = case.mutate("fx.create", document, type="STD_iwa_LinearGradientFx")["fx_id"]
    multiply = case.mutate("fx.create", document, type="multFx")["fx_id"]
    for effect, parameter, value in ((gradient, "curveType", 1), (multiply, "matte", True)):
        with case.subTest(parameter=parameter):
            before = case.success("fx.parameter.get", {"document_id": document, "fx_id": effect})
            described = next(p for p in before["parameters"] if p["name"] == parameter)
            case.assertFalse(described["animatable"])
            revision = case.revisions[document]
            request = {"fx_id": effect, "parameter": parameter, "value": value, "frame": 3}
            error = case.failure("fx.parameter.set_key", case.write_params(document, request))
            case.assertEqual(error["code"], "NOT_ANIMATABLE", error)
            after = case.success("fx.parameter.get", {"document_id": document, "fx_id": effect})
            case.assertEqual(after, before)
            summary = case.success("scene.summary", {"document_id": document})
            case.assertEqual(summary["revision"], revision)
            case.mutate(
                "fx.parameter.set", document, fx_id=effect, parameter=parameter, value=value
            )
            changed = case.success("fx.parameter.get", {"document_id": document, "fx_id": effect})
            current = next(p for p in changed["parameters"] if p["name"] == parameter)
            case.assertEqual((current["value"], current["keyframes"]), (value, []))


REFUSED_OPERATION = "refused-connect"


@acceptance_case(worker_only="needs --test-write-fault, which the adapter never passes")
def fx_native_connection_refusal_keeps_the_previous_link(case: NativeAcceptance) -> None:
    """A native connection that silently leaves the port empty is undone: the port gets its
    previous source back, the write is a typed error naming the restored link, the revision
    stays and the render is the one before the call."""
    _fault_worker(
        case,
        "fx-connect",
        "--test-write-fault",
        "native:fx-connect",
        "--test-fault-operation",
        REFUSED_OPERATION,
    )
    document = case.create("Native refusal")
    first = _card(case, document, "Kept card")
    second = _card(case, document, "Refused card")
    _color(case, document, first, COLORS[0])
    _color(case, document, second, OTHER_COLOR)
    output = case.success("fx.graph", {"document_id": document})["current_output_fx_id"]
    _connect(case, document, output, first)
    _assert_preview(case, document, COLORS[0])
    revision = case.revisions[document]
    request = case.write_params(
        document, {"fx_id": output, "port": "source", "source_fx_id": second}
    )
    request["operation_id"] = REFUSED_OPERATION
    error = case.failure("fx.connect", request)
    case.assertEqual(error["code"], "NATIVE_FAILURE", error)
    details = error["details"]
    case.assertEqual(
        (details["reason"], details["restored"], details["previous_source_fx_id"]),
        ("silent_refusal", True, first),
    )
    summary = case.success("scene.summary", {"document_id": document})
    case.assertEqual(summary["revision"], revision)
    port = _graph_effect(case, document, output)["ports"][0]
    case.assertEqual((port["connected"], port.get("source_fx_id")), (True, first))
    _assert_preview(case, document, COLORS[0])
    case.assertEqual(case.failure("fx.connect", request)["details"], details)
    _connect(case, document, output, second)
    _assert_preview(case, document, OTHER_COLOR)


def _first_parameter(entry: dict[str, Any], animatable: bool) -> dict[str, Any] | None:
    for parameter in entry["parameters"]:
        if parameter["supported"] and (not animatable or parameter["animatable"]):
            if not animatable or parameter["schema"].get("type") in ("number", "array"):
                return parameter
    return None


def _parameter_writes(case: NativeAcceptance, document: str, effect: str, entry: dict) -> None:
    plain = _first_parameter(entry, False)
    if plain is not None and not plain["keyframes"]:
        case.mutate(
            "fx.parameter.set",
            document,
            fx_id=effect,
            parameter=plain["name"],
            value=plain["value"],
        )
    keyed = _first_parameter(entry, True)
    if keyed is not None:
        case.mutate(
            "fx.parameter.set_key",
            document,
            fx_id=effect,
            parameter=keyed["name"],
            frame=1,
            value=keyed["value"],
        )
    case.success("fx.parameter.get", {"document_id": document, "fx_id": effect, "frame": 1})


def _node_writes(case: NativeAcceptance, document: str, effect: str) -> None:
    """Writes whose results describe the effect, so its diagnostics too: an effect with
    unconnected required inputs reports them."""
    for enabled in (True, False):
        case.mutate("fx.terminal.set", document, fx_id=effect, enabled=enabled)
        case.mutate("fx.enabled.set", document, fx_id=effect, enabled=not enabled)


def _type_writes(case: NativeAcceptance, document: str, card: str, entry: dict) -> None:
    effect = case.mutate("fx.create", document, type=entry["type"], name="Typed")["fx_id"]
    _node_writes(case, document, effect)
    for port in entry["ports"]:
        case.mutate("fx.connect", document, fx_id=effect, port=port["name"], source_fx_id=card)
    _node_writes(case, document, effect)
    case.mutate("fx.rename", document, fx_id=effect, name=f"{entry['type']} node")
    _parameter_writes(case, document, effect, entry)
    position = {"x": 10, "y": 20}
    case.mutate("fx.layout.set", document, positions=[{"fx_id": effect, "position": position}])
    copy = case.mutate("fx.duplicate", document, fx_id=effect)["fx_id"]
    for port in entry["ports"]:
        case.mutate("fx.disconnect", document, fx_id=effect, port=port["name"])
    for node in (copy, effect):
        case.mutate("fx.delete", document, fx_id=node)


@acceptance_case
def fx_every_admitted_type_meets_every_write_result_schema(case: NativeAcceptance) -> None:
    """Every admitted effect type goes through create, connect, terminal, enable, rename,
    parameter, layout, duplicate, disconnect and delete; the harness checks each result
    against its tool's output schema, so a result shape one type produces cannot slip by."""
    catalog = case.success("fx.catalog", {})["effects"]
    available = [entry for entry in catalog if entry["available"]]
    case.assertGreaterEqual(len(available), 31)
    document = case.create("Every type")
    card = _card(case, document, "Source card")
    for entry in available:
        with case.subTest(type=entry["type"]):
            _type_writes(case, document, card, entry)
