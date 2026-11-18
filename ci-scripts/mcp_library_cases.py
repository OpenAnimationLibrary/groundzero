"""Qualify read-only Library discovery and contained parsing of shared files."""

from __future__ import annotations

import base64
import hashlib
import re
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_png import decode_png
from mcp_studio_palette_cases import (
    PALETTE_ID,
    call,
    palette_text,
    refuse,
    shared_worker,
    tree_digest,
)
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

DRAWING = {"number": 1, "suffix": ""}


def _package_scene(case: NativeAcceptance) -> Path:
    """Save a real one-level vector scene through the worker as template material."""
    document = case.create("Library source")
    level = case.mutate("level.create", document, name="Arm", format="vector")
    case.mutate(
        "drawing.create", document, level_id=level["level_id"], drawing_id=DRAWING, format="vector"
    )
    column = case.mutate("column.create", document, name="Arm")
    case.mutate(
        "column.expose",
        document,
        column_id=column["column_id"],
        level_id=level["level_id"],
        drawing_id=DRAWING,
        start_frame=0,
        length=2,
    )
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "rig-package"))
    return Path(saved["package_directory"])


def _sidecar(item_id: str, kind: str, folder: str = "") -> str:
    level = f"<levelFolder>{folder}</levelFolder>" if folder else ""
    return (
        f'<?xml version="1.0"?><libraryItem version="1"><id>{item_id}</id>'
        f"<revision>2</revision><kind>{kind}</kind><sourceScene>Shot</sourceScene>{level}"
        "</libraryItem>"
    )


def _template(library: Path, name: str, scene: str, reference: str) -> None:
    text = scene.replace("$scenefolder/assets/level-0.pli", reference)
    (library / "templates" / f"{name}.tnz").write_text(text)


def _build_library(case: NativeAcceptance, package: Path, library: Path) -> None:
    scene = (package / "scene.tnz").read_text()
    level = (package / "assets" / "level-0.pli").read_bytes()
    templates, poses = library / "templates", library / "poses"
    for folder in (templates / "Rig", templates / "sceneIcons", poses):
        folder.mkdir(parents=True)
    (templates / "Rig" / "level-0.pli").write_bytes(level)
    (templates / "Rig" / "stale.pli").write_bytes(level)
    _template(library, "Rig", scene, "$scenefolder/Rig/level-0.pli")
    _template(library, "Copied", scene, "$scenefolder/Rig/level-0.pli")
    _template(library, "Legacy", scene, "$scenefolder/Legacy/level-0.pli")
    _template(library, "Outside", scene, str(package / "assets" / "level-0.pli"))
    rig = _sidecar("rig-id", "rig", "Rig")
    (templates / "Rig.libinfo").write_text(rig)
    (templates / "Copied.libinfo").write_text(rig)
    (templates / "Outside.libinfo").write_text(_sidecar("outside-id", "symbol"))
    (templates / "sceneIcons" / "Rig .png").write_bytes(case.fixture.read_bytes())
    thumbnail = base64.b64encode(case.fixture.read_bytes()).decode()
    (poses / "Walk.tposes").write_text(
        "<poseSet version='1'><base>Col1 x 0 </base><pose><name>\"Wave\"</name>"
        f'<entries>Col1 x 1.5 </entries><thumbnail>"{thumbnail}"</thumbnail></pose>'
        "</poseSet>\n"
    )
    (poses / "Walk.libinfo").write_text(_sidecar("walk-id", "poseSet"))
    (poses / "Mismatch.tposes").write_text("<poseSet version='1'></poseSet>\n")
    (poses / "Mismatch.libinfo").write_text(_sidecar("mismatch-id", "rig"))


def _listings(case: NativeAcceptance, worker: Worker) -> None:
    templates = call(case, worker, "library.list", {"kind": "template"})
    rows = {item["name"]: item for item in templates["items"]}
    case.assertEqual(list(rows), ["Copied", "Legacy", "Outside", "Rig"])
    case.assertEqual(
        {name: row["identity"]["status"] for name, row in rows.items()},
        {
            "Copied": "duplicate_id",
            "Legacy": "not_recorded",
            "Outside": "recorded",
            "Rig": "duplicate_id",
        },
    )
    case.assertIsNone(rows["Legacy"]["identity"]["item_id"])
    case.assertEqual(rows["Outside"]["identity"]["kind"], "symbol")
    case.assertEqual(rows["Rig"]["thumbnail"], "present")
    case.assertEqual(rows["Legacy"]["thumbnail"], "missing")
    poses = call(case, worker, "library.list", {"kind": "pose_set"})
    statuses = {item["name"]: item["identity"]["status"] for item in poses["items"]}
    case.assertEqual(statuses, {"Mismatch": "kind_mismatch", "Walk": "recorded"})


def _level(result: dict[str, Any]) -> dict[str, Any]:
    return next(row for row in result["dependencies"] if row["role"] == "level")


def _inspections(case: NativeAcceptance, worker: Worker, library: Path) -> None:
    rig = call(case, worker, "library.inspect", {"kind": "template", "name": "Rig"})
    case.assertEqual(rig["template"]["levels"][0]["location"], "item_folder")
    case.assertEqual(rig["template"]["levels"][0]["format"], "vector")
    case.assertFalse(rig["template"]["worker_insertable"])
    case.assertIn("library_insert_unavailable", rig["template"]["not_insertable_reasons"])
    level_bytes = (library / "templates" / "Rig" / "level-0.pli").read_bytes()
    case.assertEqual(
        _level(rig)["file_revision"], "sha256:" + hashlib.sha256(level_bytes).hexdigest()
    )
    case.assertTrue(rig["dependencies_complete"])
    case.assertEqual(rig["unreferenced_level_folder_files"], 1)
    case.assertEqual(rig["ownership"]["structure"], "snapshot_on_insert")
    case.assertEqual(rig["thumbnail"]["status"], "present")
    legacy = call(case, worker, "library.inspect", {"kind": "template", "name": "Legacy"})
    case.assertFalse(legacy["dependencies_complete"])
    case.assertFalse(_level(legacy)["exists"])
    outside = call(case, worker, "library.inspect", {"kind": "template", "name": "Outside"})
    case.assertEqual(_level(outside)["location"], "outside_library")
    case.assertIsNone(_level(outside)["path"])
    case.assertIn("external_level_reference", outside["template"]["not_insertable_reasons"])
    walk = call(case, worker, "library.inspect", {"kind": "pose_set", "name": "Walk"})
    case.assertIsNone(walk["template"])
    case.assertEqual(
        walk["pose_set"],
        {
            "pose_count": 1,
            "has_base": True,
            "poses": [{"pose_index": 0, "name": "Wave", "entry_count": 1, "has_thumbnail": True}],
        },
    )
    case.assertEqual(walk["ownership"]["structure"], "merged_into_scene_poses")


def _thumbnails(case: NativeAcceptance, worker: Worker) -> None:
    for params in (
        {"kind": "template", "name": "Rig"},
        {"kind": "pose_set", "name": "Walk", "pose_index": 0},
    ):
        result = call(case, worker, "library.thumbnail", params)
        width, height, _pixels = decode_png(base64.b64decode(result["image"]["data"]))
        case.assertEqual((width, height), (result["width"], result["height"]))
        case.assertLessEqual(max(width, height), 512)
    refuse(
        case,
        worker,
        "library.thumbnail",
        {"kind": "pose_set", "name": "Walk", "pose_index": 5},
        "not_found",
    )
    refuse(
        case,
        worker,
        "library.thumbnail",
        {"kind": "template", "name": "Legacy"},
        "asset_unavailable",
    )


def library_items_report_identity_dependencies_and_thumbnails(case: NativeAcceptance) -> None:
    """Report identities without inventing them, pin dependencies, and never write."""
    package = _package_scene(case)
    library = case.directory / "library"
    _build_library(case, package, library)
    before = tree_digest(library)
    worker = shared_worker(case, {"library": library})
    _listings(case, worker)
    _inspections(case, worker, library)
    _thumbnails(case, worker)
    case.assertEqual(tree_digest(library), before)
    pinned = call(case, worker, "library.inspect", {"kind": "template", "name": "Rig"})
    with (library / "templates" / "Rig" / "level-0.pli").open("ab") as level:
        level.write(b"\0")
    changed = call(case, worker, "library.inspect", {"kind": "template", "name": "Rig"})
    case.assertNotEqual(changed["dependency_digest"], pinned["dependency_digest"])
    case.assertEqual(changed["file_revision"], pinned["file_revision"])


def _malformed_templates(scene: str) -> dict[str, str]:
    """The probe corpus that once crashed, hung or exhausted the template parser."""
    cell = re.compile(r"<cell>\s*0 [0-9]+ ")
    return {
        "Crash": scene.replace('id="Camera1"', 'id="Camera-0"', 1),
        "Hang": scene.replace("<ports>", "<ports>x", 1),
        "Memory": cell.sub("<cell>0 2000000000 ", scene, count=1),
    }


def _contained(
    case: NativeAcceptance, worker: Worker, method: str, params: dict[str, Any], expected: tuple
) -> None:
    started = time.monotonic()
    details = refuse(case, worker, method, params, expected[0])
    case.assertEqual(details["cause"], expected[1], params)
    case.assertLess(time.monotonic() - started, 14, params)


def _fault_worker(case: NativeAcceptance, library: Path, fault: str) -> Worker:
    worker = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / f"library-{fault}.stderr.log",
        shared_roots={"library": library},
        test_arguments=("--test-child-fault", fault),
    )
    case.addCleanup(worker.close)
    return worker


def _hardened_parsers_refuse(case: NativeAcceptance, scene: str, library: Path) -> None:
    """The once-fatal corpus is now refused cleanly by the hardened native parsers."""
    studio = case.directory / "studio"
    (studio / "Global Palettes").mkdir(parents=True)
    fids = "<refImgPath fids='1-2000000000'>\"x.png\"</refImgPath>"
    texture = 'texture_x 4 "/missing/t.bmp" 255 0 0 255 0 0 1 0 1 1 1'
    palettes = studio / "Global Palettes"
    (palettes / "Fids.tpl").write_bytes(palette_text(PALETTE_ID, ["color_0 3 0 0 0 0"], fids))
    (palettes / "Texture.tpl").write_bytes(palette_text(PALETTE_ID, [texture]))
    for name, text in _malformed_templates(scene).items():
        case.assertNotEqual(text, scene, name)
        (library / "templates" / f"{name}.tnz").write_text(text)
    worker = shared_worker(case, {"library": library, "studio_palettes": studio})
    for name in _malformed_templates(scene):
        params = {"kind": "template", "name": name}
        _contained(case, worker, "library.inspect", params, ("invalid_library_item", "rejected"))
    params = {"root": "studio_palettes", "path": "Global Palettes/Fids.tpl"}
    _contained(case, worker, "studio_palette.inspect", params, ("unsupported_palette", "rejected"))
    params = {"root": "studio_palettes", "path": "Global Palettes/Texture.tpl"}
    call(case, worker, "studio_palette.inspect", params)


def shared_parsers_refuse_damaged_files(case: NativeAcceptance) -> None:
    """The damaged templates and palettes that once ended a parse helper are refused."""
    scene = (_package_scene(case) / "scene.tnz").read_text()
    library = case.directory / "library"
    (library / "templates").mkdir(parents=True)
    _hardened_parsers_refuse(case, scene, library)


def shared_parser_failures_leave_worker_serving(case: NativeAcceptance) -> None:
    """Native parser crashes, hangs and memory growth end only the parse helper."""
    scene = (_package_scene(case) / "scene.tnz").read_text()
    library = case.directory / "library"
    (library / "templates").mkdir(parents=True)
    (library / "templates" / "Valid.tnz").write_text(scene)
    for fault, cause in (("crash", "crashed"), ("hang", "timeout"), ("memory", "memory")):
        worker = _fault_worker(case, library, fault)
        created = call(
            case,
            worker,
            "scene.create",
            {"operation_id": "survivor", "name": "Survivor", "width_px": 32, "height_px": 32},
        )
        params = {"kind": "template", "name": "Valid"}
        _contained(case, worker, "library.inspect", params, ("invalid_library_item", cause))
        documents = call(case, worker, "automation.list_documents", {})["documents"]
        case.assertEqual([entry["document_id"] for entry in documents], [created["document_id"]])
        call(case, worker, "scene.summary", {"document_id": created["document_id"]})
    case.assertEqual(list(case.workspace.glob("runtime_*/shared-parse-*")), [])
