"""Qualify what an MCP client sees against what the native worker returns."""

from __future__ import annotations

import json
import re
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_audio_model import tone, wav_file
from mcp_lipsync_cases import Cue, dat_file
from mcp_png import write_fixture
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

UUID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
ELAPSED = re.compile(r"\"elapsed_ms\": [0-9]+")
ACTIVE = {"preparing", "running", "finalizing"}
CANCELLED_BY_CLIENT = "MCP_REQUEST_CANCELLED"
TERMINAL = {"completed", "failed", "canceled", "deadline_exceeded"}


class Journey:
    """Drive one worker through a fixed script, keeping its own handles and revisions."""

    def __init__(self, case: NativeAcceptance, worker: Worker, inputs: Path) -> None:
        self.case = case
        self.worker = worker
        self.inputs = inputs
        self.revision = ""
        self.receipts = 0
        self.record: list[tuple[str, Any]] = []

    def read(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        response = self.worker.call(method, params)
        self.case.assertNotIn("error", response, f"{method}: {response}")
        self.record.append((method, response["result"]))
        return response["result"]

    def write(self, method: str, document: str | None, **params: Any) -> dict[str, Any]:
        self.receipts += 1
        params["operation_id"] = f"journey-{self.receipts}"
        if document is not None:
            params.update(document_id=document, expected_revision=self.revision)
        result = self.read(method, params)
        self.revision = result.get("revision", self.revision)
        return result


def _scene(journey: Journey) -> tuple[str, dict[str, Any]]:
    created = journey.write(
        "scene.create",
        None,
        name="Round trip",
        width_px=32,
        height_px=32,
        width_in=32 / 120,
        height_in=32 / 120,
        frame_rate=24,
    )
    document = created["document_id"]
    fixture = journey.inputs / "quadrants.png"
    level = journey.write("level.import", document, path=str(fixture), name="Art", dpi=120)
    column = journey.write("column.create", document, name="Artwork")
    journey.write(
        "column.expose",
        document,
        column_id=column["column_id"],
        level_id=level["level_id"],
        drawing_id={"number": 1, "suffix": ""},
        start_frame=0,
        length=4,
    )
    return document, column


def _core_and_rigging(journey: Journey, document: str, column: dict[str, Any]) -> None:
    target = {"object_id": column["object_id"], "channel": "x", "unit": "inch"}
    key = {"frame": 2, "value": 0.05, "interpolation": "linear"}
    journey.write("animation.set_keys", document, **target, keys=[key])
    journey.read("animation.sample", {"document_id": document, **target, "frames": [0, 2, 3]})
    peg = journey.write("object.create", document, name="Peg", kind="pegbar")["object_id"]
    journey.write("offset_layer.create", document, object_id=peg, name="Sway")
    reads = {"document_id": document, "expected_revision": journey.revision}
    journey.read("rig.inspect", {**reads, "root_object_id": peg})
    journey.read("rig.evaluate", {**reads, "object_ids": [peg], "frames": [0, 2]})
    level = journey.write("level.create", document, name="Ink", format="vector")
    drawing = {"level_id": level["level_id"], "drawing_id": {"number": 1, "suffix": ""}}
    journey.write("drawing.create", document, **drawing, format="vector")
    points = [{"x": x, "y": x / 2, "thickness": 1.5} for x in (-6, 0, 6)]
    stroke = {"action": "add", "style_id": 1, "closed": False, "points": points}
    edits = {"coordinate_space": "drawing_native", "sharing": "all_exposures"}
    journey.write("vector.edit", document, **drawing, **edits, edits=[stroke])
    journey.read("vector.inspect", {"document_id": document, **drawing})
    journey.read("palette.inspect", {"document_id": document, "level_id": level["level_id"]})
    journey.read("camera.inspect", {"document_id": document})
    for method in ("cast.list", "scene.validate"):
        journey.read(method, {"document_id": document, "expected_revision": journey.revision})


def _other_toolsets(journey: Journey, document: str) -> None:
    journey.write(
        "storyboard.panel.create",
        document,
        panel_frame=1,
        caption="Wide",
        notes="",
        color={"r": 1, "g": 64, "b": 200},
    )
    reads = {"document_id": document, "expected_revision": journey.revision}
    journey.read("storyboard.panels", reads)
    sound = journey.inputs / "tone.wav"
    sound.write_bytes(wav_file(tone(24000, "pcm_s16", 2400)))
    journey.write("audio.import", document, path=str(sound), name="Tone")
    journey.read("audio.inspect", {"document_id": document, "expected_revision": journey.revision})
    speech = journey.inputs / "speech.dat"
    speech.write_bytes(dat_file([Cue(1, "rest"), Cue(3, "ai"), Cue(5, "rest")]))
    timing = {"format": "papagayo_dat", "shape_set": "preston_blair", "frame_rate": 24}
    analysis = journey.write("lipsync.import", None, path=str(speech), **timing)
    journey.read(
        "lipsync.inspect", {"analysis_id": analysis["analysis_id"], "offset": 0, "limit": 8}
    )
    journey.read("shared.roots", {})
    journey.read("fx.recipe.list", {})


def _render(journey: Journey, document: str) -> None:
    job = journey.write("render.preview", document, frame=2, width=32, height=32)
    deadline = time.monotonic() + 90
    status = job
    while status["status"] not in TERMINAL and time.monotonic() < deadline:
        time.sleep(0.05)
        status = journey.worker.call("job.status", {"job_id": job["job_id"]})["result"]
    journey.case.assertEqual(status["status"], "completed", status)
    artifacts = journey.read("job.result", {"job_id": job["job_id"]})["artifacts"]
    journey.read("artifact.read", {"artifact_id": artifacts[0]["artifact_id"]})


def run_journey(journey: Journey) -> list[tuple[str, Any]]:
    """Cover each toolset with representative calls, recording every structured result."""
    journey.read("automation.capabilities", {})
    journey.read("render.capabilities", {})
    document, column = _scene(journey)
    _core_and_rigging(journey, document, column)
    _other_toolsets(journey, document)
    _render(journey, document)
    journey.read("scene.summary", {"document_id": document})
    journey.read("automation.list_documents", {})
    return journey.record


def _normalized(record: list[tuple[str, Any]], roots: dict[Path, str]) -> list[tuple[str, Any]]:
    text = json.dumps(record)
    for root, name in roots.items():
        text = text.replace(json.dumps(str(root))[1:-1], name)
    handles: dict[str, str] = {}
    text = UUID.sub(lambda match: handles.setdefault(match[0], f"<id-{len(handles)}>"), text)
    text = ELAPSED.sub('"elapsed_ms": "<elapsed>"', text)
    return [tuple(item) for item in json.loads(text)]


def _native_journey(case: NativeAcceptance) -> list[tuple[str, Any]]:
    workspace = case.directory / "native-workspace"
    inputs = case.directory / "native-inputs"
    workspace.mkdir()
    inputs.mkdir()
    write_fixture(inputs / "quadrants.png")
    log = case.directory / "native-journey.stderr.log"
    worker = Worker(case.native_executable, workspace, inputs, log)
    case.addCleanup(worker.close)
    record = run_journey(Journey(case, worker, inputs))
    return _normalized(record, {workspace: "<workspace>", inputs: "<inputs>"})


def client_results_match_native_serializer(case: NativeAcceptance) -> None:
    """Require every toolset's results through this client to equal the raw worker's.

    Through the SDK client this proves structured content and image content survive the
    adapter and the SDK unchanged; against the worker it proves the script is deterministic.
    """
    expected = _native_journey(case)
    record = run_journey(Journey(case, case.worker, case.inputs))
    observed = _normalized(record, {case.workspace: "<workspace>", case.inputs: "<inputs>"})
    case.assertEqual([method for method, _ in observed], [method for method, _ in expected])
    for (method, result), (_, native) in zip(observed, expected, strict=True):
        case.assertEqual(result, native, method)


def _abandon(case: NativeAcceptance, method: str, params: dict[str, Any]) -> None:
    """Send a call the client gives up on: MCP cancellation through the SDK client.

    The raw worker has no request cancellation, so there the answer is just not used.
    """
    if not case.through_sdk:
        case.worker.call(method, params)
        return
    envelope = {"id": "abandoned", "method": method, "params": params, "cancel": True}
    response = case.worker.exchange(json.dumps(envelope).encode())
    case.assertEqual(response["error"]["code"], CANCELLED_BY_CLIENT, response)


def mcp_cancellation_leaves_render_running(case: NativeAcceptance) -> None:
    """Treat a cancelled MCP request as abandoned, never as cancelled native work.

    The abandoned submit still starts its render, which the exact retry finds; an abandoned
    edit during that render is still applied; the render runs on until cancelled explicitly.
    """
    document, _level, _column = case.populated_scene()
    frames = {"start_frame": 0, "end_frame": 119, "width": 256, "height": 256}
    params = case.write_params(document, frames)
    _abandon(case, "render.submit", params)
    busy = case.failure("render.submit", case.write_params(document, frames))
    case.assertEqual(busy["code"], "render_busy", busy)
    job = case.success("render.submit", params)
    case.assertIn(job["status"], ACTIVE, job)
    revision = case.revisions[document]
    _abandon(case, "scene.settings", case.write_params(document, {"width_px": 64}))
    summary = case.success("scene.summary", {"document_id": document})
    case.assertNotEqual(summary["revision"], revision)
    status = case.success("job.status", {"job_id": job["job_id"]})
    case.assertIn(status["status"], ACTIVE, status)
    case.success("job.cancel", case.write_params(None, {"job_id": job["job_id"]}))
    case.assertEqual(case.wait_job(job["job_id"])["status"], "canceled")


def document_ids_do_not_alias_after_close_and_reopen(case: NativeAcceptance) -> None:
    """Refuse every closed document id instead of resolving it to a later document."""
    seen: list[str] = []
    for index in range(3):
        document = case.create(f"Alias {index}")
        seen.append(document)
        case.mutate("scene.discard", document)
    saved_from = case.create("Saved")
    saved = case.mutate(
        "scene.save_copy", saved_from, directory=str(case.workspace / "alias-package")
    )
    case.mutate("scene.discard", saved_from)
    reopened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    current = case.create("Current")
    ids = [*seen, saved_from, reopened, current]
    case.assertEqual(len(set(ids)), len(ids), ids)
    for closed in [*seen, saved_from]:
        error = case.failure("scene.summary", {"document_id": closed})
        case.assertEqual(error["code"], "document_not_found", error)
    listed = case.success("automation.list_documents", {})["documents"]
    case.assertEqual(sorted(item["document_id"] for item in listed), sorted([reopened, current]))
    case.assertEqual(case.success("scene.summary", {"document_id": current})["name"], "Current")
