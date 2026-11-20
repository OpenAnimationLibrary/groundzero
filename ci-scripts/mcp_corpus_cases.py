"""Open each old-scene corpus scene read-only, render it through the worker, compare baselines.

The corpus, its baselines and the rules that keep the baselines honest are described in
`mcp_corpus_compare.py`. These cases run it through the worker: every scene is opened from a read
root, inspected, left alone by a no-op session, rendered, and compared with its stored frames under
the scene's declared policy; the batch renderer of the same build is then asked for the same
frames, which shows that the baselines still describe this build; and a scene that has been changed
on disk must fail the comparison.
"""

from __future__ import annotations

import base64
import os
import shutil
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_corpus_compare import CORPUS, check, compare_scene, load_manifest
from mcp_native_oracle import OracleError, find_tcomposer, render_with_tcomposer
from mcp_scene_access_cases import _op, discard, open_scene, project_id, wait
from mcp_studio_palette_cases import call, tree_digest
from mcp_w2_cases import _noise_png

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


def _install(case: NativeAcceptance, name: str) -> Path:
    """Copy one corpus project under the read root and return its scene."""
    target = case.inputs / "corpus" / name
    shutil.copytree(CORPUS / name, target)
    return target / "scenes" / "shot.tnz"


def _read(case: NativeAcceptance, tool: str, document: str) -> list[dict[str, Any]]:
    read = {"document_id": document, "expected_revision": "0", "limit": 128}
    return call(case, case.worker, tool, read)["items"]


def _inspect(
    case: NativeAcceptance, name: str, opened: dict[str, Any], *, renderable: bool = True
) -> None:
    """Check what the scene reads as: what its construction says it holds."""
    expected = load_manifest()["scenes"][name]["expected"]
    document = opened["document_id"]
    cast = _read(case, "cast.list", document)
    case.assertEqual(len(cast), expected["levels"], name)
    case.assertEqual(len({row["level_id"] for row in cast}), len(cast), name)
    states = {row["load_status"] for row in cast}
    case.assertLessEqual(states, {"loaded", "not_applicable"}, name)
    case.assertIn("loaded", states, name)
    case.assertEqual(opened["counts"]["levels"], expected["levels"], name)
    case.assertGreaterEqual(opened["counts"]["frames"], 6, name)
    case.assertIs(opened["render"]["renderable"], renderable, opened["render"])
    case.assertEqual(opened["load"]["levels_failed"], 0, name)


def _check_effects(case: NativeAcceptance, name: str, document: str) -> None:
    """The effects the scene file declares are in the loaded graph, enabled but for the bypassed."""
    scene = load_manifest()["scenes"][name]
    graph = call(case, case.worker, "fx.graph", {"document_id": document})
    nodes = {node["type"]: node for node in graph["effects"] if node["type"] in scene["effects"]}
    case.assertEqual(sorted(nodes), sorted(scene["effects"]), name)
    for kind, node in nodes.items():
        case.assertEqual(node["enabled"], kind not in scene["bypassed"], f"{name} {kind}")


def _render_frames(case: NativeAcceptance, document: str, folder: Path) -> list[Path]:
    """Render frames 0 to 5 at 32 by 32 through the worker and write them as files."""
    manifest = load_manifest()["frames"]
    request = {
        "document_id": document,
        "expected_revision": "0",
        "start_frame": manifest["first"],
        "end_frame": manifest["last"],
        "step": 1,
        "width": manifest["width"],
        "height": manifest["height"],
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


def _blocked_render(case: NativeAcceptance, name: str, opened: dict[str, Any]) -> None:
    """A scene the read-only render profile refuses: the refusal is typed."""
    case.assertFalse(opened["render"]["renderable"], opened["render"])
    request = {
        "document_id": opened["document_id"],
        "expected_revision": "0",
        "frame": 0,
        "width": 32,
        "height": 32,
        "operation_id": _op(case),
    }
    response = case.worker.call("render.preview", request)
    case.assertIn("error", response, f"{name} is listed as blocked but rendered")
    case.assertEqual(response["error"]["details"]["check"], "render_admission")


def _open_and_render(case: NativeAcceptance, name: str) -> tuple[dict[str, Any], list[Path]]:
    scene = _install(case, name)
    show = project_id(case, case.worker, name)
    opened = open_scene(case, case.worker, scene, show)
    return opened, _render_frames(case, opened["document_id"], case.directory / "worker" / name)


@acceptance_case
def corpus_scenes_open_read_only_render_and_match_their_baselines(case: NativeAcceptance) -> None:
    """Open every corpus scene, inspect it, change nothing, render it and compare its baselines."""
    case.assertEqual(check(), [], "the corpus does not match its manifest")
    manifest = load_manifest()
    before = tree_digest(case.inputs)
    for name, scene in manifest["scenes"].items():
        with case.subTest(scene=name):
            if scene["read_only_render"] == "blocked":
                opened = open_scene(
                    case, case.worker, _install(case, name), project_id(case, case.worker, name)
                )
                _inspect(case, name, opened, renderable=False)
                _check_effects(case, name, opened["document_id"])
                _blocked_render(case, name, opened)
                discard(case, case.worker, opened["document_id"])
                continue
            opened, frames = _open_and_render(case, name)
            _inspect(case, name, opened)
            _check_effects(case, name, opened["document_id"])
            status = call(
                case,
                case.worker,
                "scene.source_status",
                {"document_id": opened["document_id"], "expected_revision": "0"},
            )
            case.assertFalse(status["changed"], status)
            case.assertEqual(compare_scene(manifest, name, frames), [], name)
            discard(case, case.worker, opened["document_id"])
    case.assertEqual(case.success("automation.list_documents", {})["documents"], [])
    installed = case.inputs / "corpus"
    case.assertEqual(sorted(path.name for path in installed.iterdir()), sorted(manifest["scenes"]))
    case.assertTrue(before != tree_digest(case.inputs), "the corpus was installed")


@acceptance_case
def corpus_baselines_are_reproduced_by_this_builds_batch_renderer(case: NativeAcceptance) -> None:
    """Render each scene with the build's own batch renderer and compare the stored baselines.

    The batch renderer is the `tcomposer` target of the build next to the application; a build
    made without it cannot run this case (build it with the build tool's `--target tcomposer`).
    The case skips then, unless `GZ_REQUIRE_BATCH_RENDERER` is set, as a qualification run
    sets it: a required backend that is missing fails the run.
    """
    tcomposer = find_tcomposer(case.native_executable.parent)
    if tcomposer is None:
        message = (
            f"no tcomposer next to {case.native_executable.name}: build the tcomposer target of "
            "this build to qualify the corpus baselines against it"
        )
        if os.environ.get("GZ_REQUIRE_BATCH_RENDERER"):
            case.fail(message)
        case.skipTest(message)
        return
    manifest = load_manifest()
    for name in manifest["scenes"]:
        with case.subTest(scene=name):
            project = case.directory / "batch" / name
            shutil.copytree(CORPUS / name, project)
            output = case.directory / "batch" / f"{name}-frames"
            output.mkdir()
            try:
                frames = render_with_tcomposer(
                    tcomposer,
                    project / "scenes" / "shot.tnz",
                    output,
                    range(manifest["frames"]["first"], manifest["frames"]["last"] + 1),
                    stuff=case.directory / "batch" / "stuff",
                )
            except OracleError as error:
                case.fail(str(error))
            case.assertEqual(compare_scene(manifest, name, frames), [], name)


@acceptance_case
def a_corpus_scene_changed_on_disk_fails_its_comparison(case: NativeAcceptance) -> None:
    """Repaint the PNG a scene shows: the worker's render no longer matches, and says where."""
    name = "png_pan"
    scene = _install(case, name)
    asset = next((scene.parent.parent / "drawings").glob("*.png"))
    original = asset.read_bytes()
    _noise_png(asset, 32, 7)
    case.assertNotEqual(asset.read_bytes(), original)
    opened = open_scene(case, case.worker, scene, project_id(case, case.worker, name))
    frames = _render_frames(case, opened["document_id"], case.directory / "changed")
    problems = compare_scene(load_manifest(), name, frames)
    case.assertTrue(problems, "a repainted level passed the comparison")
    case.assertTrue(any("differs by" in problem for problem in problems), problems)
    case.assertEqual(check(), [], "the corpus itself must be untouched")
    discard(case, case.worker, opened["document_id"])


@acceptance_case
def a_corpus_effect_changed_on_disk_fails_its_comparison(case: NativeAcceptance) -> None:
    """Raise the gamma of the raster scene's effect in its file: the render no longer matches.

    The change leaves the drawings and the structure alone, so only a comparison of pixels
    under the effect can notice it.
    """
    name = "raster_effects"
    scene = _install(case, name)
    original = scene.read_text(encoding="utf-8")
    changed = original.replace("1.6000000000000001", "2.2000000000000002")
    case.assertNotEqual(changed, original, "the gamma value is not in the scene file")
    scene.write_text(changed, encoding="utf-8")
    opened = open_scene(case, case.worker, scene, project_id(case, case.worker, name))
    frames = _render_frames(case, opened["document_id"], case.directory / "changed-effect")
    problems = compare_scene(load_manifest(), name, frames)
    case.assertTrue(problems, "a changed effect passed the comparison")
    case.assertTrue(any("differs by" in problem for problem in problems), problems)
    case.assertEqual(check(), [], "the corpus itself must be untouched")
    discard(case, case.worker, opened["document_id"])
