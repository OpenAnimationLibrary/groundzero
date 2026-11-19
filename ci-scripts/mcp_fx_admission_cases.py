"""Read-only renders admit the built-in effects and still refuse what is not built in.

An effect is built in when the application's own code declares it. The scene below uses the eight
built-in compositing and color effects whose identifiers do not begin with `STD_`; its read-only
render must be admitted and must equal what the batch renderer of the same build writes for the
same project. A scene that also holds an external program is refused for that effect alone.
"""

from __future__ import annotations

import base64
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_fx_authoring_cases import _artwork_fixture, _output
from mcp_native_oracle import (
    OracleError,
    compare_files,
    find_tcomposer,
    render_with_tcomposer,
)
from mcp_scene_access_cases import (
    _op,
    _project,
    discard,
    external_effect,
    open_scene,
    project_id,
    wait,
    with_effect,
)
from mcp_studio_palette_cases import call, refuse

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

EIGHT = ("colorCardFx", "overFx", "invertFx", "multFx", "screenFx", "inFx", "outFx", "blendFx")
FRAMES = range(0, 3)
HALF = [60, 170, 90, 128]


def _connect(case: NativeAcceptance, document: str, node: str, **sources: str) -> None:
    for port, source in sources.items():
        case.mutate("fx.connect", document, fx_id=node, port=port, source_fx_id=source)


def _create(case: NativeAcceptance, document: str, kind: str) -> str:
    return case.mutate("fx.create", document, type=kind, name=kind, frame_count=13)["fx_id"]


def _build(case: NativeAcceptance) -> Path:
    """Save a scene whose output uses all eight effects over a PNG with four colored quadrants."""
    document, art = _artwork_fixture(case)
    half = _create(case, document, "colorCardFx")
    case.mutate("fx.parameter.set", document, fx_id=half, parameter="color", value=HALF)
    node = {kind: _create(case, document, kind) for kind in EIGHT[1:]}
    _connect(case, document, node["invertFx"], source=art)
    _connect(case, document, node["overFx"], Source1=half, Source2=node["invertFx"])
    _connect(case, document, node["multFx"], Source1=node["overFx"], Source2=half)
    for frame, value in ((0, 20), (2, 200)):
        case.mutate(
            "fx.parameter.set_key",
            document,
            fx_id=node["multFx"],
            parameter="value",
            frame=frame,
            value=value,
        )
    _connect(case, document, node["screenFx"], Source1=node["multFx"], Source2=art)
    _connect(case, document, node["inFx"], Source=node["screenFx"], Matte=half)
    _connect(case, document, node["outFx"], Source=art, Matte=half)
    _connect(case, document, node["blendFx"], Source1=node["inFx"], Source2=node["outFx"])
    case.mutate("fx.parameter.set", document, fx_id=node["blendFx"], parameter="value", value=60)
    _output(case, document, node["blendFx"])
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "eight"))
    case.mutate("scene.discard", document)
    return Path(saved["package_directory"])


def _render_worker(case: NativeAcceptance, document: str, folder: Path) -> list[Path]:
    """Render the frames at 32 by 32 through the worker and write them as PNG files."""
    request = {
        "document_id": document,
        "expected_revision": "0",
        "start_frame": FRAMES.start,
        "end_frame": FRAMES.stop - 1,
        "step": 1,
        "width": 32,
        "height": 32,
        "operation_id": _op(case),
    }
    job = call(case, case.worker, "render.submit", request)["job_id"]
    case.assertEqual(wait(case, case.worker, job)["status"], "completed")
    artifacts = call(case, case.worker, "job.result", {"job_id": job})["artifacts"]
    folder.mkdir(parents=True)
    written = []
    for artifact in sorted(artifacts, key=lambda item: item["frame"]):
        image = call(case, case.worker, "artifact.read", {"artifact_id": artifact["artifact_id"]})
        path = folder / f"frame.{artifact['frame'] + 1:04d}.png"
        path.write_bytes(base64.b64decode(image["image"]["data"], validate=True))
        written.append(path)
    return written


def _admissions(case: NativeAcceptance, document: str) -> dict[str, dict[str, Any]]:
    graph = call(case, case.worker, "fx.graph", {"document_id": document})
    return {node["type"]: node["admission"] for node in graph["effects"]}


@acceptance_case
def read_only_render_admits_built_in_effects_and_matches_the_batch_renderer(
    case: NativeAcceptance,
) -> None:
    """Open the eight-effect scene read-only, render it, and compare it exactly with tcomposer."""
    scene = _project(_build(case), case.inputs / "eight", "Eight", "drawings")
    opened = open_scene(case, case.worker, scene, project_id(case, case.worker, "Eight"))
    document = opened["document_id"]
    case.assertTrue(opened["render"]["renderable"], opened["render"])
    case.assertEqual(opened["render"]["blockers"], [])
    admissions = _admissions(case, document)
    for kind in EIGHT:
        case.assertEqual(admissions[kind]["class"], "builtin_admitted", kind)
        case.assertEqual(admissions[kind]["render"], "supported", kind)
    frames = _render_worker(case, document, case.directory / "worker")
    case.assertEqual(len(frames), len(FRAMES))
    case.assertEqual(len({path.read_bytes() for path in frames}), len(FRAMES), "frames must vary")
    discard(case, case.worker, document)
    tcomposer = find_tcomposer(case.native_executable.parent)
    if tcomposer is None:
        case.skipTest("no tcomposer next to the application: build its target to compare")
        return
    output = case.directory / "batch"
    output.mkdir()
    try:
        expected = render_with_tcomposer(
            tcomposer, scene, output, FRAMES, stuff=case.directory / "batch-stuff"
        )
    except OracleError as error:
        case.fail(str(error))
    for index, (batch, worker) in enumerate(zip(expected, frames, strict=True)):
        difference = compare_files(batch, worker)
        case.assertTrue(difference.equal, f"frame {index}: {difference.problems}")


@acceptance_case
def read_only_render_still_refuses_an_external_program_beside_built_in_effects(
    case: NativeAcceptance,
) -> None:
    """One external program blocks the render, and no built-in effect is named as a blocker."""
    base = _build(case)
    program = case.inputs / "never-run"
    scene = _project(base, case.inputs / "mixed", "Mixed", "drawings")
    scene.write_text(
        with_effect(
            scene.read_text(),
            external_effect(program),
            "<externalProgramFx id='3000'/>",
        )
    )
    opened = open_scene(case, case.worker, scene, project_id(case, case.worker, "Mixed"))
    document = opened["document_id"]
    case.assertFalse(opened["render"]["renderable"])
    blockers = opened["render"]["blockers"]
    case.assertEqual({row["reason"] for row in blockers}, {"external_code"}, blockers)
    admissions = _admissions(case, document)
    case.assertEqual(admissions["externalProgramFx"]["class"], "external_code")
    for kind in EIGHT:
        case.assertEqual(admissions[kind]["class"], "builtin_admitted", kind)
    base_params = {"document_id": document, "expected_revision": "0", "operation_id": _op(case)}
    preview = {**base_params, "frame": 0, "width": 32, "height": 32}
    refuse(case, case.worker, "render.preview", preview, "render_blocked")
    submit = {**base_params, "operation_id": _op(case), "start_frame": 0, "end_frame": 1}
    submit.update(step=1, width=32, height=32)
    refuse(case, case.worker, "render.submit", submit, "render_blocked")
    case.assertFalse(program.exists())
    discard(case, case.worker, document)
    case.assertEqual(case.success("automation.list_documents", {})["documents"], [])


@acceptance_case
def read_only_open_refuses_an_effect_the_application_does_not_register(
    case: NativeAcceptance,
) -> None:
    """A plug-in or shader effect is not registered in the worker; its identifier is unknown."""
    base = _build(case)
    scene = _project(base, case.inputs / "unknown", "Unknown", "drawings")
    ghost = "<gzShaderFx id='3001'> <name> shader </name> </gzShaderFx>"
    scene.write_text(with_effect(scene.read_text(), ghost, "<gzShaderFx id='3001'/>"))
    show = project_id(case, case.worker, "Unknown")
    params = {"path": str(scene), "project_id": show, "operation_id": _op(case)}
    details = refuse(case, case.worker, "scene.open_read_only", params, "invalid_scene")
    case.assertIn("gzShaderFx", details["native_message"])
