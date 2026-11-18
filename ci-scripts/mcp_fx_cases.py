"""Verify selected FX branches and animated colors through native render and persistence."""

from __future__ import annotations

import base64
import hashlib
from typing import TYPE_CHECKING, Any

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
