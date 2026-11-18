"""Qualify slice-2 reads of read-only user scenes: inventories, sources, projects, reads."""

from __future__ import annotations

import json
import os
import re
import shutil
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

import mcp_scene_access_cases as access
from mcp_studio_palette_cases import call, refuse
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

DRAWING = {"number": 1, "suffix": ""}
# The scene host's content digest bounds (PinDigestFileBytes, PinDigestTotalBytes).
DIGEST_FILE_BYTES = 64 * 1024 * 1024
DIGEST_TOTAL_BYTES = 512 * 1024 * 1024
TICK_NS = 1_700_000_000 * 1_000_000_000


# ---------------------------------------------------------------- helpers


def read(case: NativeAcceptance, method: str, document: str, **params: Any) -> dict[str, Any]:
    """A read of a read-only document, whose revision is always "0"."""
    return call(case, case.worker, method, {"document_id": document, **params})


def inventory(case: NativeAcceptance, document: str, section: str, **params: Any) -> dict[str, Any]:
    params = {"expected_revision": "0", "section": section, "limit": 128, **params}
    return read(case, "scene.inventory", document, **params)


def source_status(case: NativeAcceptance, document: str) -> dict[str, Any]:
    return read(case, "scene.source_status", document, expected_revision="0")


def job_status(case: NativeAcceptance, job: str) -> dict[str, Any]:
    """Poll one render job; the job tools are renamed by a later contract."""
    return call(case, case.worker, "render.status", {"job_id": job})


def wait_job(case: NativeAcceptance, job: str) -> dict[str, Any]:
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        status = job_status(case, job)
        if status["status"] in access.TERMINAL:
            return status
        time.sleep(0.05)
    case.fail(f"Render job {job} did not finish")


def open_b(case: NativeAcceptance, scenes: dict[str, Path]) -> str:
    return access.open_scene(case, case.worker, scenes["B"], "standalone")["document_id"]


# ---------------------------------------------------------------- inventory


def _sheets(case: NativeAcceptance, document: str) -> list[dict[str, Any]]:
    rows = inventory(case, document, "sheets")
    case.assertTrue(rows["traversal_complete"], rows)
    case.assertEqual(rows["discovered"], 3)
    top, *children = rows["items"]
    case.assertEqual((top["sheet_path"], top["level_id"], top["depth"]), ([], None, 0))
    case.assertEqual(len({json.dumps(child["sheet_path"]) for child in children}), 2)
    case.assertEqual(len({child["level_id"] for child in children}), 1)
    case.assertEqual([child["instance_count"] for child in children], [2, 2])
    case.assertEqual({child["name"] for child in children}, {"Sub"})
    return children


def _instance_rows(case: NativeAcceptance, document: str, child: dict[str, Any]) -> None:
    path = child["sheet_path"]
    columns = inventory(case, document, "columns", sheet_path=path)["items"]
    case.assertEqual(len(columns), 1)
    case.assertEqual((columns[0]["sheet_path"], columns[0]["kind"]), (path, "level"))
    graph = read(case, "fx.graph", document, sheet_path=path)
    case.assertTrue(
        any(effect.get("column_id") == columns[0]["column_id"] for effect in graph["effects"])
    )
    objects = inventory(case, document, "stage_objects", sheet_path=path)["items"]
    case.assertTrue(all(row["sheet_path"] == path for row in objects))
    wrong = [{"column_id": path[0]["column_id"], "level_id": "level_unknown"}]
    details = refuse(
        case,
        case.worker,
        "scene.inventory",
        {
            "document_id": document,
            "expected_revision": "0",
            "section": "columns",
            "sheet_path": wrong,
        },
        "invalid_handle",
    )
    case.assertEqual(details["sheet_path"], wrong)
    refuse(
        case,
        case.worker,
        "fx.graph",
        {"document_id": document, "sheet_path": wrong},
        "invalid_handle",
    )


def _top_rows(case: NativeAcceptance, document: str) -> None:
    columns = inventory(case, document, "columns", sheet_path=[])["items"]
    names = {row["name"]: row for row in columns}
    case.assertEqual(names["SubA"]["kind"], "child")
    case.assertTrue(names["SubA"]["duplicate_name"])
    case.assertFalse(names["Ink"]["duplicate_name"])
    first = inventory(case, document, "columns", limit=2)
    case.assertEqual((len(first["items"]), first["next_offset"]), (2, 2))
    refuse(
        case,
        case.worker,
        "scene.inventory",
        {"document_id": document, "expected_revision": "1", "section": "sheets"},
        "stale_revision",
    )
    refuse(
        case,
        case.worker,
        "scene.inventory",
        {"document_id": document, "expected_revision": "0", "section": "sheets", "offset": 99},
        "invalid_params",
    )


def _count_objects(case: NativeAcceptance, document: str) -> int:
    return inventory(case, document, "stage_objects")["discovered"]


def read_only_inventory_tells_instances_from_definitions(case: NativeAcceptance) -> None:
    """Two instances of one sub-xsheet are two rows with one level_id; reads, graphs and
    renders never add stage objects."""
    scenes = access.corpus(case)
    text = scenes["B"].read_text().replace("SubB \n", "SubA \n")
    scenes["B"].write_text(text)
    document = open_b(case, scenes)
    before = _count_objects(case, document)
    children = _sheets(case, document)
    _instance_rows(case, document, children[0])
    _top_rows(case, document)
    read(case, "scene.summary", document)
    read(case, "fx.graph", document)
    access.preview(case, case.worker, document, 0)
    case.assertEqual(_count_objects(case, document), before)
    editable = case.create("Editable inventory")
    rows = call(
        case,
        case.worker,
        "scene.inventory",
        {
            "document_id": editable,
            "expected_revision": case.revisions[editable],
            "section": "sheets",
        },
    )
    case.assertEqual(rows["discovered"], 1)
    access.discard(case, case.worker, document)


def _many_objects(base: Path, folder: Path) -> Path:
    """A sub-xsheet holding 16,000 pegbars, exposed by five columns."""
    pegbars = "".join(f'<pegbar id="Peg{index + 1}"> </pegbar>\n' for index in range(16000))
    child = access._child(5000000).replace(
        "<fxnodes>", f"<pegbars>\n{pegbars}</pegbars>\n<fxnodes>", 1
    )
    columns = "".join(access._exposing(5000000, 5100000 + index) for index in range(5))
    text = access._with_children((base / "scene.tnz").read_text(), [child], columns)
    shutil.copytree(base / "assets", folder / "assets")
    (folder / "shot.tnz").write_text(text)
    return folder / "shot.tnz"


def read_only_inventory_bounds_its_rows(case: NativeAcceptance) -> None:
    """A section past 65,536 rows is cut there and says so; pages stay within 1 MiB."""
    base = access.base_package(case)
    path = _many_objects(base, case.inputs / "objects")
    document = access.open_scene(case, case.worker, path, "standalone")["document_id"]
    rows = inventory(case, document, "stage_objects", offset=65536 - 128)
    case.assertEqual(rows["discovered"], 65536)
    case.assertFalse(rows["traversal_complete"])
    case.assertEqual(rows["reasons"], ["row_limit"])
    case.assertEqual(rows["next_offset"], None)
    access.discard(case, case.worker, document)


# ---------------------------------------------------------------- source status


def _preflight_agrees(case: NativeAcceptance, document: str, changed: dict[str, Any]) -> None:
    """Sources changed since open fail source_pins with the paths scene.source_status names;
    dependencies, which reports what the scene lacked at open, still passes."""
    params = {"expected_revision": "0", "kind": "preview", "frame": 0}
    report = read(case, "render.preflight", document, **params)
    checks = {check["id"]: check for check in report["checks"]}
    case.assertEqual(checks["dependencies"]["status"], "pass", report)
    pins = checks["source_pins"]
    case.assertEqual((pins["status"], pins["code"]), ("fail", "source_changed"), report)
    case.assertEqual(set(pins["details"]["paths"]), {row["path"] for row in changed["changed"]})
    case.assertEqual(report["first_failure"], "source_pins")


def read_only_source_status_reports_each_change(case: NativeAcceptance) -> None:
    """Modified, replaced and missing pins are told apart; a missing scene is missing;
    a decoding read of a changed level is refused."""
    scenes = access.corpus(case)
    document = open_b(case, scenes)
    unchanged = source_status(case, document)
    case.assertEqual((unchanged["state"], unchanged["changed"]), ("unchanged", []))
    case.assertEqual(unchanged["current_fingerprint"], unchanged["fingerprint_at_open"])
    assets = scenes["B"].parent / "assets"
    (assets / "level-2.0001.png").write_bytes((assets / "level-2.0001.png").read_bytes() + b"\0")
    replacement = assets / "replacement.pli"
    shutil.copy(assets / "level-0.pli", replacement)
    os.replace(replacement, assets / "level-0.pli")
    (assets / "level-1.tpl").unlink()
    changed = source_status(case, document)
    kinds = {Path(row["path"]).name: row["change"] for row in changed["changed"]}
    case.assertEqual(
        kinds, {"level-2.0001.png": "modified", "level-0.pli": "replaced", "level-1.tpl": "missing"}
    )
    case.assertEqual((changed["state"], changed["changed_count"]), ("changed", 3))
    case.assertNotEqual(changed["current_fingerprint"], unchanged["fingerprint_at_open"])
    _preflight_agrees(case, document, changed)
    summary = read(case, "scene.summary", document)
    ink = next(level["level_id"] for level in summary["levels"] if level["name"] == "Ink")
    refuse(
        case,
        case.worker,
        "vector.inspect",
        {"document_id": document, "level_id": ink, "drawing_id": DRAWING},
        "source_changed",
    )
    scenes["B"].unlink()
    gone = source_status(case, document)
    case.assertEqual((gone["state"], gone["current_fingerprint"]), ("missing", None))
    editable = case.create("Editable source")
    refuse(
        case,
        case.worker,
        "scene.source_status",
        {"document_id": editable, "expected_revision": case.revisions[editable]},
        "unsupported_target",
    )
    access.discard(case, case.worker, document)


def _rewrite_in_place(path: Path, offset: int) -> None:
    """Flip one byte without changing size or identity, then put back the whole-second time a
    file system with a coarse clock would show."""
    with path.open("r+b") as stream:
        stream.seek(offset)
        byte = stream.read(1)
        stream.seek(offset)
        stream.write(bytes([byte[0] ^ 0xFF]))
    os.utime(path, ns=(TICK_NS, TICK_NS))


def _metadata_only(status: dict[str, Any]) -> set[str]:
    return {Path(path).name for path in status["verification"]["metadata_only_paths"]}


def read_only_source_status_checks_content_within_bounds(case: NativeAcceptance) -> None:
    """An equal-size rewrite in place within one clock tick is a change for a digested file;
    a file above the per-file bound is reported as checked by metadata only."""
    scenes = access.corpus(case)
    assets = scenes["B"].parent / "assets"
    small, large = assets / "level-0.pli", assets / "level-2.0001.png"
    os.truncate(large, DIGEST_FILE_BYTES + 1)
    for path in (small, large):
        os.utime(path, ns=(TICK_NS, TICK_NS))
    opened = access.open_scene(case, case.worker, scenes["B"], "standalone")
    document = opened["document_id"]
    before = source_status(case, document)
    scope = before["verification"]
    case.assertEqual(scope["content_checked_files"], opened["source"]["content_pinned_file_count"])
    case.assertEqual(
        scope["content_checked_files"] + scope["metadata_only_files"], before["checked_files"]
    )
    case.assertIn(large.name, _metadata_only(before))
    case.assertNotIn(small.name, _metadata_only(before))
    _rewrite_in_place(small, small.stat().st_size // 2)
    _rewrite_in_place(large, DIGEST_FILE_BYTES)
    after = source_status(case, document)
    changes = {Path(row["path"]).name: row["change"] for row in after["changed"]}
    case.assertEqual(changes, {small.name: "modified"})
    case.assertNotEqual(after["current_fingerprint"], before["fingerprint_at_open"])
    case.assertIn(large.name, _metadata_only(after))
    summary = read(case, "scene.summary", document)
    ink = next(level["level_id"] for level in summary["levels"] if level["name"] == "Ink")
    decode = {"document_id": document, "level_id": ink, "drawing_id": DRAWING}
    refuse(case, case.worker, "vector.inspect", decode, "source_changed")
    preview = {"document_id": document, "expected_revision": "0", "frame": 0, "width": 32}
    preview.update(height=32, operation_id=access._op(case))
    refuse(case, case.worker, "render.preview", preview, "source_changed")
    access.discard(case, case.worker, document)


def _budget_scene(case: NativeAcceptance, count: int) -> Path:
    """The base package plus count PNG levels, each one frame padded to the per-file bound."""
    base = access.base_package(case)
    folder = case.inputs / "budget"
    shutil.copytree(base / "assets", folder / "assets")
    png = (base / "assets" / "level-2.0001.png").read_bytes()
    levels = ""
    for index in range(count):
        frame = folder / "big" / f"b{index}" / f"b{index}.0001.png"
        frame.parent.mkdir(parents=True)
        frame.write_bytes(png)
        os.truncate(frame, DIGEST_FILE_BYTES)
        levels += (
            f"<level id='{6000 + index}'>\nBig{index} \n"
            '<info dpix="120.000000" dpiy="120.000000"/>\n'
            f'<path> "$scenefolder/big/b{index}/b{index}..png" </path>\n</level>\n'
        )
    anchor = "</levels>\n    <folder"
    text = (base / "scene.tnz").read_text().replace(anchor, levels + anchor, 1)
    (folder / "shot.tnz").write_text(text)
    return folder / "shot.tnz"


def read_only_content_digests_hold_their_deadlines(case: NativeAcceptance) -> None:
    """At the digest budget, open and source_status finish inside their deadlines, and the
    files past the budget are checked by metadata only."""
    count = DIGEST_TOTAL_BYTES // DIGEST_FILE_BYTES + 1
    opened = access.open_scene(case, case.worker, _budget_scene(case, count), "standalone")
    case.assertLess(opened["load"]["elapsed_ms"], 60000)
    status = source_status(case, opened["document_id"])
    case.assertEqual(status["state"], "unchanged")
    case.assertLess(status["elapsed_ms"], 30000)
    skipped = {name for name in _metadata_only(status) if name.startswith("b")}
    case.assertIn(f"b{count - 1}.0001.png", skipped)
    case.assertIn(len(skipped), {1, 2}, skipped)
    access.discard(case, case.worker, opened["document_id"])


# ---------------------------------------------------------------- projects


def _sceneful_project(case: NativeAcceptance, scenes: dict[str, Path]) -> None:
    """Give ShowA scene defaults that differ from its scene, and add a nested scene."""
    root = scenes["A"].parent.parent
    properties = re.search(r"<properties>(.*?)</properties>", scenes["A"].read_text(), re.S)
    assert properties is not None
    defaults = properties.group(1).replace("32 32 \n", "64 64 \n", 1)
    defaults = re.sub(r"<fps>\s*24 \s*</fps>", "<fps> 12 </fps>", defaults, count=1)
    folders = {
        "drawings": "drawings",
        "scenes": "scenes",
        "extras": "extras",
        "outputs": "/nowhere/outputs",
        "library": "$scenepath/library",
    }
    entries = "".join(f'<folder name="{name}" path="{path}"/>' for name, path in folders.items())
    (root / "ShowA_otprj.xml").write_text(
        f"<project><version>70 1</version><folders>{entries}</folders>"
        f"<sceneProperties>{defaults}</sceneProperties></project>\n"
    )
    (root / "scenes" / "act1").mkdir()
    shutil.copy(scenes["A"], root / "scenes" / "act1" / "shot2.tnz")
    (root / "scenes" / "notes.txt").write_text("not a scene")


def _inspect_project(case: NativeAcceptance, show: str) -> None:
    project = call(case, case.worker, "project.inspect", {"project_id": show})
    case.assertEqual(
        (project["kind"], project["format"], project["version"]),
        ("granted_project", "otprj", "70.1"),
    )
    case.assertTrue(project["file_revision"].startswith("sha256:"))
    folders = {row["alias"]: row for row in project["folders"]}
    case.assertEqual(folders["drawings"]["authority"], "input_root")
    case.assertTrue(folders["drawings"]["exists"])
    case.assertEqual(
        (folders["outputs"]["authority"], folders["outputs"]["resolved_path"]),
        ("outside_roots", None),
    )
    case.assertEqual(folders["library"]["authority"], "scene_dependent")
    defaults = project["scene_defaults"]
    case.assertEqual((defaults["camera_width_px"], defaults["frame_rate"]), (64, 12))
    refuse(case, case.worker, "project.inspect", {"project_id": "standalone"}, "invalid_params")
    refuse(case, case.worker, "project.inspect", {"project_id": "project_" + "1" * 32}, "not_found")


def _list_scenes(case: NativeAcceptance, show: str) -> None:
    listing = call(case, case.worker, "project.scenes", {"project_id": show})
    rows = {row["relative_path"]: row["kind"] for row in listing["items"]}
    case.assertEqual(rows, {"act1": "folder", "act1/shot2.tnz": "scene", "shot.tnz": "scene"})
    case.assertTrue(listing["scan"]["complete"])
    nested = call(case, case.worker, "project.scenes", {"project_id": show, "folder": "act1"})
    case.assertEqual([row["relative_path"] for row in nested["items"]], ["act1/shot2.tnz"])
    first = call(case, case.worker, "project.scenes", {"project_id": show, "limit": 1})
    case.assertEqual(first["next_offset"], 1)
    refuse(
        case,
        case.worker,
        "project.scenes",
        {"project_id": show, "expected_listing_revision": "listing:0"},
        "revision_conflict",
    )
    refuse(
        case, case.worker, "project.scenes", {"project_id": show, "folder": "../x"}, "invalid_path"
    )
    refuse(case, case.worker, "project.scenes", {"project_id": "standalone"}, "invalid_params")


def _advisories(case: NativeAcceptance, scene: Path, show: str) -> None:
    document = access.open_scene(case, case.worker, scene, show)["document_id"]
    issues = read(case, "scene.validate", document, expected_revision="0")["items"]
    codes = {row["code"]: row["severity"] for row in issues}
    case.assertEqual(codes.get("frameRateMismatch"), "info")
    case.assertEqual(codes.get("resolutionMismatch"), "info")
    access.discard(case, case.worker, document)


def read_only_projects_inspect_and_list_scenes(case: NativeAcceptance) -> None:
    """A granted project's file is parsed in a helper and its scenes folder listed;
    scenes that differ from its defaults are advised."""
    scenes = access.corpus(case)
    _sceneful_project(case, scenes)
    show = access.project_id(case, case.worker, "ShowA")
    _inspect_project(case, show)
    _list_scenes(case, show)
    _advisories(case, scenes["A"], show)
    outside = case.inputs / "corpus" / "Elsewhere"
    (outside / "scenes").mkdir(parents=True)
    (outside / "Elsewhere_otprj.xml").write_text(access.project_xml({"scenes": "/nowhere/scenes"}))
    elsewhere = access.project_id(case, case.worker, "Elsewhere")
    listing = call(case, case.worker, "project.scenes", {"project_id": elsewhere})
    case.assertEqual((listing["scenes_folder"], listing["items"]), (None, []))
    case.assertEqual(listing["scan"]["reasons"], ["unreadable"])


# ---------------------------------------------------------------- dependencies


def _dependency_scene(case: NativeAcceptance, base: Path) -> Path:
    folder = case.inputs / "dependencies"
    shutil.copytree(base / "assets", folder / "assets")
    (folder / "assets" / "level-0_hooks.xml").write_text("<hooks> </hooks>\n")
    (folder / "assets" / "grain.png").write_bytes(case.fixture.read_bytes())
    palette = folder / "assets" / "level-1.tpl"
    styles = (
        '<style> texture_2 4 "$scenefolder/assets/grain.png" 255 0 0 255 0 0 1 0 1 1 1 </style>'
        '<style> texture_3 4 "brick.bmp" 255 0 0 255 0 0 1 0 1 1 1 </style>'
    )
    text = palette.read_text().replace("</styles>", styles + "</styles>", 1)
    palette.write_text(
        re.sub(r"<indices>\s*0 1\s*</indices>", "<indices> 0 1 2 3 </indices>", text)
    )
    cache = "<STD_particleCacheFx id='4001'> <name> Cache </name> </STD_particleCacheFx>"
    (folder / "shot.tnz").write_text(access.with_effect((base / "scene.tnz").read_text(), cache))
    return folder / "shot.tnz"


def read_only_dependencies_list_every_file_read(case: NativeAcceptance) -> None:
    """Hook sidecars, textures, sound sources and FX file resources are listed with their
    resolution and pin state."""
    base = access.base_package(case)
    scene = _dependency_scene(case, base)
    document = access.open_scene(case, case.worker, scene, "standalone")["document_id"]
    rows = read(case, "scene.dependencies", document, expected_revision="0", limit=128)["items"]
    by_role: dict[str, list[dict[str, Any]]] = {}
    for row in rows:
        by_role.setdefault(row["role"], []).append(row)
    hook = by_role["hook_sidecar"][0]
    case.assertEqual((hook["availability"], hook["pinned"]), ("present", True))
    textures = {row["coded_path"]: row for row in by_role["texture"]}
    case.assertTrue(textures["$scenefolder/assets/grain.png"]["pinned"])
    case.assertEqual(textures["brick.bmp"]["availability"], "unsupported")
    case.assertIn("fx_file_resource", by_role)
    access.discard(case, case.worker, document)
    sound = case.workspace / "sounding"
    access.sound_package(case, sound)
    document = access.open_scene(case, case.worker, sound / "scene.tnz", "standalone")[
        "document_id"
    ]
    rows = read(case, "scene.dependencies", document, expected_revision="0")["items"]
    sounds = [row for row in rows if row["role"] == "sound"]
    case.assertEqual(len(sounds), 1)
    case.assertEqual((sounds[0]["availability"], sounds[0]["pinned"]), ("present", True))
    access.discard(case, case.worker, document)


# ---------------------------------------------------------------- routed reads


def _drawing_reads(case: NativeAcceptance, document: str, levels: dict[str, str]) -> None:
    target = {"drawing_id": DRAWING}
    drawing = read(case, "drawing.inspect", document, level_id=levels["Ink"], **target)
    case.assertEqual(drawing["drawing_id"], DRAWING)
    read(case, "vector.inspect", document, level_id=levels["Ink"], **target)
    read(case, "vector.structure", document, level_id=levels["Ink"], **target)
    pixels = read(
        case,
        "raster.read_pixels",
        document,
        level_id=levels["Paint"],
        coordinate_space="raster_pixels_lower_left",
        x=0,
        y=0,
        width=2,
        height=2,
        **target,
    )
    case.assertEqual(len(pixels["pixels"]), 4)


def _palette_reads(case: NativeAcceptance, document: str, levels: dict[str, str]) -> None:
    palette = read(case, "palette.inspect", document, level_id=levels["Cel"])
    case.assertGreaterEqual(len(palette["styles"]), 2)
    pages = read(case, "palette.pages", document, level_id=levels["Cel"])
    case.assertTrue(pages["pages"])


def _object_reads(case: NativeAcceptance, document: str, summary: dict[str, Any]) -> None:
    column = summary["columns"][0]
    cells = read(
        case, "column.cells", document, column_id=column["column_id"], start_frame=0, length=6
    )
    case.assertEqual(len(cells["cells"]), 6)
    channels = read(case, "animation.channels", document, object_id=column["object_id"])
    case.assertTrue(channels["channels"])
    target = {"object_id": column["object_id"], "channel": "x", "unit": "inch"}
    read(case, "animation.keys", document, **target)
    sample = read(case, "animation.sample", document, frames=[0, 2], **target)
    case.assertEqual(len(sample["values"]), 2)


def _fx_reads(case: NativeAcceptance, document: str) -> None:
    graph = read(case, "fx.graph", document)
    blur = next(e["fx_id"] for e in graph["effects"] if e["type"] == "STD_blurFx")
    parameters = read(case, "fx.parameter.get", document, fx_id=blur, frame=0)
    case.assertIn("value", {row["name"] for row in parameters["parameters"]})


def read_only_documents_answer_the_slice_two_reads(case: NativeAcceptance) -> None:
    """Each of the twelve slice-2 reads answers on a user scene, from its host."""
    scenes = access.corpus(case)
    show = access.project_id(case, case.worker, "ShowA")
    document = access.open_scene(case, case.worker, scenes["A"], show)["document_id"]
    summary = read(case, "scene.summary", document)
    levels = {level["name"]: level["level_id"] for level in summary["levels"]}
    _drawing_reads(case, document, levels)
    opacity = read(
        case,
        "vector.opacity.inspect",
        document,
        expected_revision="0",
        level_id=levels["Ink"],
        drawing_id=DRAWING,
        stroke_index=0,
        offset=0,
        limit=1,
    )
    case.assertTrue(opacity)
    _palette_reads(case, document, levels)
    _object_reads(case, document, summary)
    _fx_reads(case, document)
    access.discard(case, case.worker, document)


# ---------------------------------------------------------------- deadline


def read_only_reads_end_at_the_read_deadline(case: NativeAcceptance) -> None:
    """A host that stops answering a read is stopped at 30 seconds; the document is lost
    and the worker keeps serving."""
    scenes = access.corpus(case)
    worker: Worker = access.fault_worker(case, "read-hang")
    opened = access.open_scene(case, worker, scenes["B"], "standalone")
    started = time.monotonic()
    response = access._slow_call(
        worker, "scene.summary", {"document_id": opened["document_id"]}, 60
    )
    elapsed = time.monotonic() - started
    case.assertEqual(response["error"]["code"], "scene_host_failed", response)
    case.assertEqual(
        (response["error"]["details"]["cause"], response["error"]["details"]["phase"]),
        ("timeout", "read"),
    )
    case.assertGreater(elapsed, 29.5)
    case.assertLess(elapsed, 40)
    rows = call(case, worker, "automation.list_documents", {})["documents"]
    case.assertEqual([row["host_state"] for row in rows], ["lost"])
    access.discard(case, worker, opened["document_id"])
    case.assertEqual(access.host_processes(worker), [])
