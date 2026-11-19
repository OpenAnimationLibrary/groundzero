"""Qualify read-only user scenes: project discovery, scene hosts, admission and limits."""

from __future__ import annotations

import json
import os
import re
import shutil
import signal
import struct
import subprocess
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_audio_model import tone, wav_file
from mcp_studio_palette_cases import call, refuse, tree_digest
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

DRAWING = {"number": 1, "suffix": ""}
TERMINAL = {"completed", "failed", "canceled", "deadline_exceeded"}
FORWARDED = {
    "scene.summary",
    "camera.inspect",
    "level.inspect",
    "cast.list",
    "cast.usage",
    "scene.dependencies",
    "scene.validate",
    "fx.graph",
    "render.preview",
    "render.submit",
    "render.preflight",
    "scene.discard",
    "scene.inventory",
    "scene.source_status",
    "column.cells",
    "drawing.inspect",
    "palette.inspect",
    "palette.pages",
    "fx.parameter.get",
    "animation.channels",
    "animation.keys",
    "animation.sample",
    "vector.inspect",
    "vector.structure",
    "vector.opacity.inspect",
    "raster.read_pixels",
    "scene.check.submit",
}
# Tools the worker answers for a read-only document itself, from its job registry.
ANSWERED_BY_WORKER = {"job.list"}
CHILD_ID = 2000
TIFF_ID = 2100


# ---------------------------------------------------------------- corpus


def tiff(width: int, height: int, samples: int, bits: int) -> bytes:
    """Write an uncompressed little-endian RGB TIFF whose extra samples follow alpha."""
    data = struct.pack("<" + ("H" if bits == 16 else "B") * samples, *([200] * samples))
    data *= width * height
    extra = [2] + [0] * (samples - 4)
    bits_at = 8 + 2 + 12 * 11 + 4
    data_at = bits_at + 2 * samples

    def short(tag: int, value: int) -> bytes:
        return struct.pack("<HHIHH", tag, 3, 1, value, 0)

    def long(tag: int, value: int) -> bytes:
        return struct.pack("<HHII", tag, 4, 1, value)

    entries = [
        short(256, width),
        short(257, height),
        struct.pack("<HHII", 258, 3, samples, bits_at),
        short(259, 1),
        short(262, 2),
        long(273, data_at),
        short(277, samples),
        short(278, height),
        long(279, len(data)),
        short(284, 1),
        struct.pack("<HHIHH", 338, 3, len(extra), extra[0], extra[1] if extra[1:] else 0),
    ]
    header = struct.pack("<2sHIH", b"II", 42, 8, len(entries))
    sizes = struct.pack("<" + "H" * samples, *([bits] * samples))
    return header + b"".join(entries) + struct.pack("<I", 0) + sizes + data


def base_package(case: NativeAcceptance) -> Path:
    """Save a real scene with vector, Toonz raster and PNG levels and a blur."""
    document = case.create("Base")
    vector = case.mutate("level.create", document, name="Ink", format="vector")
    case.mutate(
        "drawing.create", document, level_id=vector["level_id"], drawing_id=DRAWING, format="vector"
    )
    stroke = {
        "action": "add",
        "style_id": 1,
        "closed": False,
        "points": [{"x": x, "y": 0, "thickness": 1} for x in (-10, 0, 10)],
    }
    case.mutate(
        "vector.edit",
        document,
        level_id=vector["level_id"],
        drawing_id=DRAWING,
        coordinate_space="drawing_native",
        sharing="all_exposures",
        edits=[stroke],
    )
    raster = case.mutate("level.create", document, name="Cel", format="toonzRaster")
    case.mutate(
        "drawing.create",
        document,
        level_id=raster["level_id"],
        drawing_id=DRAWING,
        format="toonzRaster",
        width=2,
        height=2,
        dpi=120,
    )
    image = case.mutate("level.import", document, path=str(case.fixture), name="Paint", dpi=120)
    columns = []
    for level, name in ((vector, "Ink"), (raster, "Cel"), (image, "Paint")):
        column = case.mutate("column.create", document, name=name)
        case.mutate(
            "column.expose",
            document,
            column_id=column["column_id"],
            level_id=level["level_id"],
            drawing_id=DRAWING,
            start_frame=0,
            length=6,
        )
        columns.append(column["column_id"])
    for level, name in ((vector, "SubA"), (vector, "SubB"), (image, "Deep")):
        column = case.mutate("column.create", document, name=name)
        case.mutate(
            "column.expose",
            document,
            column_id=column["column_id"],
            level_id=level["level_id"],
            drawing_id=DRAWING,
            start_frame=0,
            length=6,
        )
    graph = case.success("fx.graph", {"document_id": document})
    paint = next(e["fx_id"] for e in graph["effects"] if e.get("column_id") == columns[2])
    blur = case.mutate("fx.create", document, type="STD_blurFx")["fx_id"]
    case.mutate("fx.connect", document, fx_id=blur, port="Source", source_fx_id=paint)
    case.mutate("fx.terminal.set", document, fx_id=blur, enabled=True)
    case.mutate("fx.terminal.set", document, fx_id=paint, enabled=False)
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "base"))
    case.mutate("scene.discard", document)
    return Path(saved["package_directory"])


def _cells(text: str, replacements: dict[int, str]) -> str:
    """Replace the level reference of the n-th <cell> block (0-based)."""
    parts = text.split("<cell>")
    for index, cell in replacements.items():
        parts[index + 1] = re.sub(r"<level id='\d+'/>0001 0", cell, parts[index + 1], count=1)
    return "<cell>".join(parts)


def _child_level(vector_id: int) -> str:
    fx = (
        "<params> </params> <ports> </ports> <numberId> 0 </numberId> <name> {0} </name> "
        '<fxId> "" </fxId> <opened> 0 </opened> '
    )
    column_fx = fx.format("LevelColumn")
    return (
        f"<childLevel id='{CHILD_ID}'>\n<xsheet id='{CHILD_ID + 1}'>\n<columns>\n"
        f"<levelColumn id='{CHILD_ID + 2}'> <status> 0 </status> <cells> <cell>\n"
        f"0 6 <level id='{vector_id}'/>0001 0 \n</cell> </cells> <fx>\n"
        f"<Toonz_columnFx id='{CHILD_ID + 3}'> {column_fx}</Toonz_columnFx> </fx>\n"
        "</levelColumn>\n</columns>\n<fxnodes> <terminal> <fxnode>\n"
        f"<Toonz_columnFx id='{CHILD_ID + 3}'/> </fxnode> </terminal> <xsheet>\n"
        f"<Toonz_xsheetFx id='{CHILD_ID + 4}'> {fx.format('Xsheet')}</Toonz_xsheetFx>\n"
        f"</xsheet> <output> <Toonz_outputFx id='{CHILD_ID + 5}'> <params> </params> "
        f"<ports> <source> <Toonz_xsheetFx id='{CHILD_ID + 4}'/> </source> </ports> "
        '<numberId> 0 </numberId> <name> Output </name> <fxId> "" </fxId> <opened> 0 '
        "</opened> </Toonz_outputFx> </output> </fxnodes>\n</xsheet>\n<name> Sub </name>\n"
        "</childLevel>\n"
    )


def _tiff_level() -> str:
    return (
        f'<level id=\'{TIFF_ID}\'>\nDeep \n<info dpix="120.000000" dpiy="120.000000"/>\n'
        '<path> "$scenefolder/assets/deep..tif" </path>\n</level>\n'
    )


def _standalone(base: Path, folder: Path) -> Path:
    """Scene B: $scenefolder levels, a sub-xsheet used twice, a 16-bit TIFF and a
    blur above its authoring range."""
    shutil.copytree(base / "assets", folder / "assets")
    (folder / "assets" / "deep.0001.tif").write_bytes(tiff(4, 4, 4, 16))
    text = (base / "scene.tnz").read_text()
    child = f"<childLevel id='{CHILD_ID}'/>1 1"
    # Count the top sheet's cells before the child definition adds its own.
    text = _cells(text, {3: child, 4: child, 5: f"<level id='{TIFF_ID}'/>0001 0"})
    text = text.replace(
        "</levels>\n    <folder", _child_level(1) + _tiff_level() + "</levels>\n    <folder", 1
    )
    blur = r"(<STD_blurFx id='\d+'>\s*<params>\s*<value>\s*<default>\s*)20"
    text = re.sub(blur, r"\g<1>300", text)
    path = folder / "shot_b.tnz"
    path.write_text(text)
    return path


def project_xml(folders: dict[str, str], rules: str = "") -> str:
    entries = "".join(f'<folder name="{name}" path="{path}"/>' for name, path in folders.items())
    return f"<project><version>70 1</version><folders>{entries}</folders>{rules}</project>\n"


def scenes_xml(relative: str = "..") -> str:
    return f'<parentProject type="projectFolder">"{relative}"</parentProject>\n'


WORKER_RULES = (
    "<filePathProperties><useStandard>0</useStandard><acceptNonAlphabetSuffix>0"
    "</acceptNonAlphabetSuffix><letterCountForSuffix>8</letterCountForSuffix>"
    "<noSeparatorFormatAllowed>0</noSeparatorFormatAllowed></filePathProperties>"
)


def _project(base: Path, root: Path, name: str, drawings: str, rules: str = "") -> Path:
    """A project whose +drawings folder holds every level of the base scene."""
    folder = root / name
    (folder / "scenes").mkdir(parents=True)
    shutil.copytree(base / "assets", folder / drawings)
    outputs = "/nowhere/outputs" if rules else "outputs"
    folders = {"drawings": drawings, "scenes": "scenes", "extras": "extras", "outputs": outputs}
    (folder / f"{name}_otprj.xml").write_text(project_xml(folders, rules))
    (folder / "scenes" / "scenes.xml").write_text(scenes_xml())
    text = (base / "scene.tnz").read_text().replace("$scenefolder/assets/", "+drawings/")
    path = folder / "scenes" / "shot.tnz"
    path.write_text(text)
    return path


def external_effect(program: Path, identifier: int = 3000) -> str:
    """An externalProgramFx node whose program never exists, so nothing can run."""
    return (
        f"<externalProgramFx id='{identifier}'> <name> ext </name> "
        f'<path> "{program}" </path> <args> "sentinel" </args> </externalProgramFx>'
    )


def with_effect(text: str, effect: str, terminal: str = "") -> str:
    """Add an internal effect node, and optionally make a reference to it terminal."""
    text = text.replace("<internal>", f"<internal><fxnode>{effect}</fxnode>", 1)
    if terminal:
        text = text.replace("<terminal>", f"<terminal><fxnode>{terminal}</fxnode>", 1)
    return text


def _unsafe(base: Path, folder: Path, program: Path) -> Path:
    """Scene C: a connected external program, a missing level and an unrecognized
    top-level tag."""
    shutil.copytree(base / "assets", folder / "assets")
    text = (base / "scene.tnz").read_text()
    text = with_effect(text, external_effect(program), "<externalProgramFx id='3000'/>")
    text = text.replace('"$scenefolder/assets/level-1.tlv"', '"$scenefolder/gone/level-1.tlv"')
    text = text.replace("</tnz>", "<gzFutureData> kept </gzFutureData>\n</tnz>")
    path = folder / "shot_c.tnz"
    path.write_text(text)
    unknown = "<STD_noSuchEffectFx id='3001'> <name> ghost </name> </STD_noSuchEffectFx>"
    (folder / "ghost.tnz").write_text(with_effect((base / "scene.tnz").read_text(), unknown))
    return path


def corpus(case: NativeAcceptance) -> dict[str, Path]:
    base = base_package(case)
    root = case.inputs / "corpus"
    return {
        "A": _project(base, root, "ShowA", "drawings"),
        "A2": _project(base, root, "ShowB", "art", WORKER_RULES),
        "B": _standalone(base, root / "Solo"),
        "C": _unsafe(base, root / "Unsafe", case.inputs / "never-run"),
        "base": base,
    }


# ---------------------------------------------------------------- helpers


def _op(case: NativeAcceptance) -> str:
    case.operations += 1
    return f"scene-access-{case.operations}"


def open_scene(
    case: NativeAcceptance,
    worker: Worker,
    path: Path | str,
    project_id: str,
    **extra: Any,
) -> dict[str, Any]:
    params = {"path": str(path), "project_id": project_id, "operation_id": _op(case), **extra}
    return call(case, worker, "scene.open_read_only", params)


def project_id(case: NativeAcceptance, worker: Worker, name: str) -> str:
    listing = call(case, worker, "project.list", {"limit": 64})
    return next(row["project_id"] for row in listing["projects"] if row["name"] == name)


def wait(case: NativeAcceptance, worker: Worker, job: str) -> dict[str, Any]:
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        status = call(case, worker, "job.status", {"job_id": job})
        if status["status"] in TERMINAL:
            return status
        time.sleep(0.05)
    case.fail(f"Render job {job} did not finish")


def preview(case: NativeAcceptance, worker: Worker, document: str, frame: int) -> bytes:
    params = {"document_id": document, "expected_revision": "0", "frame": frame, "width": 32}
    params.update(height=32, operation_id=_op(case))
    job = call(case, worker, "render.preview", params)
    case.assertIsNone(job["inputs"]["snapshot_manifest_sha256"])
    case.assertTrue(job["inputs"]["source_fingerprint"].startswith("source:"))
    status = wait(case, worker, job["job_id"])
    case.assertEqual(status["status"], "completed", status)
    artifacts = call(case, worker, "job.result", {"job_id": job["job_id"]})["artifacts"]
    image = call(case, worker, "artifact.read", {"artifact_id": artifacts[0]["artifact_id"]})
    case.assertEqual((image["width"], image["height"]), (32, 32))
    return image["image"]["data"].encode()


def discard(case: NativeAcceptance, worker: Worker, document: str) -> None:
    params = {"document_id": document, "expected_revision": "0", "operation_id": _op(case)}
    case.assertTrue(call(case, worker, "scene.discard", params)["discarded"])


def host_processes(worker: Worker) -> list[tuple[int, str]]:
    """The scene hosts under a worker, or under the MCP client and adapter that start
    it: (pid, document_id)."""
    listing = subprocess.run(
        ["ps", "-ax", "-o", "pid=,ppid=,command="], capture_output=True, text=True, check=True
    ).stdout
    rows = [line.split(None, 2) for line in listing.splitlines()]
    rows = [(int(row[0]), int(row[1]), row[2]) for row in rows if len(row) == 3]
    tree = {worker.process.pid}
    grown = True
    while grown:
        added = {pid for pid, parent, _command in rows if parent in tree} - tree
        tree |= added
        grown = bool(added)
    hosts = []
    for pid, parent, command in rows:
        match = re.search(r"--scene-host (document_\S+)", command)
        if match and parent in tree:
            hosts.append((pid, match.group(1)))
    return hosts


def fault_worker(case: NativeAcceptance, fault: str, name: str = "") -> Worker:
    worker = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / f"fault-{name or fault}.stderr.log",
        test_arguments=("--test-child-fault", fault),
    )
    case.addCleanup(worker.close)
    return worker


# ---------------------------------------------------------------- cases


def _inspect(case: NativeAcceptance, worker: Worker, document: str) -> dict[str, Any]:
    read = {"document_id": document, "expected_revision": "0", "limit": 128}
    cast = call(case, worker, "cast.list", read)
    case.assertEqual(cast["profile"], "native-read-only-v1")
    dependencies = call(case, worker, "scene.dependencies", read)
    graph = call(case, worker, "fx.graph", {"document_id": document})
    for effect in graph["effects"]:
        case.assertIn(effect["admission"]["render"], {"supported", "blocked"})
    return {"cast": cast["items"], "dependencies": dependencies["items"], "graph": graph}


def _journey_a(case: NativeAcceptance, worker: Worker, scene: Path, project: str) -> str:
    opened = open_scene(case, worker, scene, project)
    case.assertEqual(opened["project"]["binding"], "native")
    case.assertEqual(opened["project"]["native_binding"]["status"], "matches")
    case.assertTrue(opened["render"]["renderable"], opened["render"])
    case.assertEqual(opened["load"]["levels_loaded"], 3)
    found = _inspect(case, worker, opened["document_id"])
    kinds = {row["name"]: (row["kind"], row["load_status"]) for row in found["cast"]}
    case.assertEqual(
        kinds,
        {
            "Ink": ("vector", "loaded"),
            "Cel": ("toonz_raster", "loaded"),
            "Paint": ("raster", "loaded"),
        },
    )
    drawings = str(scene.parent.parent / "drawings")
    primary = [row for row in found["dependencies"] if row["role"] == "primary"]
    case.assertTrue(all(row["resolved_path"].startswith(drawings) for row in primary), primary)
    case.assertTrue(all(row["pinned"] for row in primary), primary)
    return opened["document_id"]


def _journey_b(case: NativeAcceptance, worker: Worker, scene: Path) -> str:
    opened = open_scene(case, worker, scene, "standalone")
    case.assertEqual(opened["project"]["binding"], "standalone")
    case.assertEqual(opened["counts"]["sheets"], 2)
    case.assertTrue(opened["render"]["renderable"], opened["render"])
    document = opened["document_id"]
    found = _inspect(case, worker, document)
    deep = next(row for row in found["cast"] if row["name"] == "Deep")
    case.assertEqual((deep["kind"], deep["load_status"]), ("raster", "loaded"))
    summary = call(
        case, worker, "scene.summary", {"document_id": document, "include_drawings": False}
    )
    case.assertEqual(summary["access"], "read_only")
    rows = {row["name"]: row for row in summary["levels"]}
    case.assertEqual(rows["Deep"]["bit_depth"], 16)
    case.assertNotIn("drawings", rows["Deep"])
    issues = call(
        case, worker, "scene.validate", {"document_id": document, "expected_revision": "0"}
    )
    codes = {row["code"] for row in issues["items"]}
    case.assertIn("authoringRangeExceeded", codes)
    return document


def _journey_c(case: NativeAcceptance, worker: Worker, scene: Path) -> str:
    opened = open_scene(case, worker, scene, "standalone")
    reasons = {row["reason"] for row in opened["content"]["unsupported"]}
    case.assertTrue({"external_code", "missing_file", "unrecognized_scene_tag"} <= reasons, reasons)
    case.assertEqual(opened["source"]["unrecognized_scene_tags"], ["gzFutureData"])
    case.assertFalse(opened["render"]["renderable"])
    rules = {row["rule"] for row in opened["render"]["blockers"]}
    case.assertIn("anywhere", rules)
    document = opened["document_id"]
    params = {"document_id": document, "expected_revision": "0", "frame": 0}
    report = call(case, worker, "render.preflight", {**params, "kind": "preview"})
    checks = {check["id"]: check for check in report["checks"]}
    case.assertEqual(report["first_failure"], "dependencies", report)
    missing = checks["dependencies"]["details"]["dependencies"]
    case.assertEqual({row["reason"] for row in missing}, {"missing_file"}, missing)
    case.assertEqual(checks["render_admission"]["code"], "render_blocked")
    admission = checks["render_admission"]["details"]["blockers"]
    case.assertIn("external_code", {row["reason"] for row in admission})
    case.assertEqual(checks["fx_graph"]["status"], "not_applicable")
    case.assertEqual(checks["source_pins"]["status"], "pass")
    details = refuse(
        case, worker, "render.preview", {**params, "operation_id": _op(case)}, "missing_dependency"
    )
    case.assertEqual(details["check"], "dependencies")
    codes = {
        row["code"]
        for row in call(
            case, worker, "scene.validate", {"document_id": document, "expected_revision": "0"}
        )["items"]
    }
    case.assertTrue({"externalCodeFx", "missingDependency", "unrecognizedSceneData"} <= codes)
    return document


def _cancel(case: NativeAcceptance, worker: Worker, document: str, other: str) -> None:
    params = {"document_id": document, "expected_revision": "0", "start_frame": 0}
    params.update(end_frame=119, width=512, height=512, operation_id=_op(case))
    job = call(case, worker, "render.submit", params)["job_id"]
    elsewhere = {"document_id": other, "expected_revision": "0", "frame": 0}
    busy = refuse(
        case, worker, "render.preview", {**elsewhere, "operation_id": _op(case)}, "render_busy"
    )
    case.assertEqual(busy["job_id"], job)
    busy = refuse(
        case,
        worker,
        "scene.discard",
        {"document_id": document, "expected_revision": "0", "operation_id": _op(case)},
        "render_busy",
    )
    case.assertEqual(busy["job_id"], job)
    cancel = {"job_id": job, "operation_id": _op(case)}
    first = call(case, worker, "job.cancel", cancel)
    case.assertEqual(call(case, worker, "job.cancel", cancel), first)
    case.assertIn(wait(case, worker, job)["status"], {"canceled", "completed"})


def read_only_journey_renders_and_preserves_sources(case: NativeAcceptance) -> None:
    """Open three user scenes, inspect, preview three frames, cancel, discard: no writes."""
    scenes = corpus(case)
    before = tree_digest(case.inputs)
    worker = case.worker
    show = project_id(case, worker, "ShowA")
    documents = [
        _journey_a(case, worker, scenes["A"], show),
        _journey_b(case, worker, scenes["B"]),
        _journey_c(case, worker, scenes["C"]),
    ]
    pixels = [preview(case, worker, documents[0], frame) for frame in (0, 2, 5)]
    case.assertEqual(len(pixels), 3)
    case.assertEqual(preview(case, worker, documents[1], 0), preview(case, worker, documents[1], 0))
    _cancel(case, worker, documents[0], documents[1])
    listed = call(case, worker, "automation.list_documents", {})["documents"]
    case.assertEqual(
        {(row["access"], row["host_state"]) for row in listed}, {("read_only", "ready")}
    )
    case.assertEqual(len(host_processes(worker)), 3)
    for document in documents:
        discard(case, worker, document)
    case.assertEqual(host_processes(worker), [])
    case.assertEqual(tree_digest(case.inputs), before)
    case.assertFalse((case.inputs / "sentinel").exists())


def _variant(case: NativeAcceptance, base: Path, name: str, text: str) -> Path:
    folder = case.inputs / "admission" / name
    shutil.copytree(base / "assets", folder / "assets")
    (folder / "shot.tnz").write_text(text)
    return folder / "shot.tnz"


def _child_with_effect(text: str, effect: str) -> str:
    """Put an effect inside the sub-xsheet's own graph, connected to its output."""
    reference = re.search(r"<(\w+) id='(\d+)'", effect)
    assert reference is not None
    tag, identifier = reference.groups()
    child = _child_level(1).replace(
        "<fxnodes> <terminal>",
        f"<fxnodes> <internal> <fxnode> {effect} </fxnode> </internal> <terminal> "
        f"<fxnode> <{tag} id='{identifier}'/> </fxnode>",
    )
    text = _cells(text, {3: f"<childLevel id='{CHILD_ID}'/>1 1"})
    return text.replace("</levels>\n    <folder", child + "</levels>\n    <folder", 1)


def _missing_level() -> str:
    return (
        '<level id=\'2200\'>\nLost \n<info dpix="120.000000" dpiy="120.000000"/>\n'
        '<path> "$scenefolder/nowhere/lost..png" </path>\n</level>\n'
    )


def _admission_scenes(text: str, program: Path) -> dict[str, tuple[str, str | None]]:
    """name -> (scene text, expected blocker as "reason/rule", or None if renderable)."""
    external = external_effect(program)
    negate = "<STD_inoNegateFx id='4000'> <name> Negate </name> </STD_inoNegateFx>"
    cache = "<STD_particleCacheFx id='4001'> <name> Cache </name> </STD_particleCacheFx>"
    anchor = "</levels>\n    <folder"
    return {
        "connected": (
            with_effect(text, external, "<externalProgramFx id='3000'/>"),
            "external_code/anywhere",
        ),
        "disconnected": (with_effect(text, external), "external_code/anywhere"),
        "sub_xsheet": (_child_with_effect(text, external), "external_code/anywhere"),
        "file_resource": (with_effect(text, cache), "file_resource/anywhere"),
        "other_rendered": (
            with_effect(text, negate, "<STD_inoNegateFx id='4000'/>"),
            "unsupported_fx/rendered",
        ),
        "other_hidden": (with_effect(text, negate), None),
        "unreachable_level": (text.replace(anchor, _missing_level() + anchor, 1), None),
    }


def _admitted(
    case: NativeAcceptance, opened: dict[str, Any], name: str, blocker: str | None
) -> None:
    worker = case.worker
    render = opened["render"]
    pairs = {f"{row['reason']}/{row['rule']}" for row in render["blockers"]}
    case.assertEqual(render["renderable"], blocker is None, (name, render))
    if blocker is None:
        case.assertTrue(opened["content"]["unsupported"], name)
        preview(case, worker, opened["document_id"], 0)
        return
    case.assertIn(blocker, pairs, name)
    params = {"document_id": opened["document_id"], "expected_revision": "0", "frame": 0}
    params["operation_id"] = _op(case)
    refuse(case, worker, "render.preview", params, "render_blocked")


def read_only_render_admission_refuses_unsafe_content(case: NativeAcceptance) -> None:
    """External programs and file effects block anywhere; other unsupported content only
    when the current output renders it. Nothing is ever executed."""
    base = base_package(case)
    text = (base / "scene.tnz").read_text()
    program = case.inputs / "never-run"
    worker = case.worker
    for name, (scene, blocker) in _admission_scenes(text, program).items():
        opened = open_scene(case, worker, _variant(case, base, name, scene), "standalone")
        _admitted(case, opened, name, blocker)
        graph = call(case, worker, "fx.graph", {"document_id": opened["document_id"]})
        classes = {effect["admission"]["class"] for effect in graph["effects"]}
        expected = name in {"connected", "disconnected"}
        case.assertEqual("external_code" in classes, expected, (name, classes))
        discard(case, worker, opened["document_id"])
    ghost = case.inputs / "admission" / "ghost.tnz"
    unknown = "<STD_noSuchEffectFx id='3001'> <name> ghost </name> </STD_noSuchEffectFx>"
    ghost.write_text(with_effect(text, unknown))
    details = refuse(
        case,
        worker,
        "scene.open_read_only",
        {"path": str(ghost), "project_id": "standalone", "operation_id": _op(case)},
        "invalid_scene",
    )
    case.assertEqual(details["cause"], "rejected")
    case.assertIn("STD_noSuchEffectFx", details["native_message"])
    case.assertFalse(program.exists())
    case.assertEqual(host_processes(worker), [])


def _refused_open(
    case: NativeAcceptance, path: Path | str, code: str, project: str = "standalone"
) -> dict[str, Any]:
    params = {"path": str(path), "project_id": project, "operation_id": _op(case)}
    return refuse(case, case.worker, "scene.open_read_only", params, code)


def _oversized(case: NativeAcceptance) -> None:
    large = case.inputs / "large.tnz"
    with large.open("wb") as handle:
        handle.truncate(64 * 1024 * 1024 + 1)
    details = _refused_open(case, large, "scene_limit")
    case.assertEqual(details["limit"], "scene_file_bytes")
    columns = "".join(
        f"<levelColumn id='{5000 + index}'> <status> 0 </status> </levelColumn>\n"
        for index in range(10001)
    )
    text = (case.inputs / "corpus" / "Solo" / "shot_b.tnz").read_text()
    wide = case.inputs / "wide.tnz"
    wide.write_text(text.replace("<columns>", "<columns>\n" + columns, 1))
    case.assertEqual(_refused_open(case, wide, "scene_limit")["limit"], "columns_per_sheet")
    beyond = case.inputs / "beyond.tnz"
    beyond.write_text(
        text.replace(
            '<pegbar id="Table">', '<pegbar id="Col10001"> </pegbar><pegbar id="Table">', 1
        )
    )
    case.assertEqual(_refused_open(case, beyond, "invalid_scene")["cause"], "rejected")


def _paths(case: NativeAcceptance) -> None:
    _refused_open(case, case.inputs / "absent.tnz", "not_found")
    _refused_open(case, "/etc/hosts", "path_denied")
    _refused_open(case, str(case.inputs / "corpus" / ".." / "large.tnz"), "invalid_path")
    link = case.inputs / "link.tnz"
    link.symlink_to(case.inputs / "corpus" / "Solo" / "shot_b.tnz")
    _refused_open(case, link, "path_denied")
    (case.inputs / "notes.txt").write_text("not a scene")
    _refused_open(case, case.inputs / "notes.txt", "unsupported_type")
    details = _refused_open(
        case,
        case.inputs / "corpus" / "Solo" / "shot_b.tnz",
        "not_found",
        project="project_" + "0" * 32,
    )
    case.assertEqual(details["hint"], "project.list")


def _bindings(case: NativeAcceptance, scenes: dict[str, Path]) -> None:
    show = project_id(case, case.worker, "ShowA")
    other = project_id(case, case.worker, "ShowB")
    details = _refused_open(case, scenes["A"], "project_mismatch")
    case.assertEqual(details["native_binding"], "differs")
    case.assertEqual(details["native_project_id"], show)
    details = _refused_open(case, scenes["A"], "project_mismatch", project=other)
    case.assertEqual(details["native_binding"], "differs")
    details = _refused_open(case, scenes["B"], "project_mismatch", project=show)
    case.assertEqual(details["native_binding"], "none_within_roots")
    forced = open_scene(case, case.worker, scenes["A"], other, project_binding="override")
    case.assertEqual(forced["project"]["binding"], "override")
    case.assertEqual(forced["project"]["native_binding"]["project_id"], show)
    discard(case, case.worker, forced["document_id"])


def read_only_rejections_and_limits(case: NativeAcceptance) -> None:
    """Every refusal happens before a host keeps running; limits hold from both sides."""
    scenes = corpus(case)
    _paths(case)
    _oversized(case)
    _bindings(case, scenes)
    worker = case.worker
    documents = [
        open_scene(case, worker, scenes[name], "standalone")["document_id"]
        for name in ("B", "B", "C", "C")
    ]
    details = _refused_open(case, scenes["B"], "resource_limit")
    case.assertEqual(details["limit"], "read_only_documents")
    capabilities = call(case, worker, "automation.capabilities", {})["read_only_scenes"]
    case.assertEqual(capabilities["max_documents"], 4)
    inspected = call(case, worker, "project.inspect", {})
    case.assertTrue(inspected["arbitrary_scene_open"])
    case.assertEqual(inspected["read_only_documents"], {"open": 4, "max": 4})
    case.assertEqual(len(host_processes(worker)), 4)
    for document in documents:
        discard(case, worker, document)
    case.assertEqual(host_processes(worker), [])


def _tool_params(tool: dict[str, Any], document: str) -> dict[str, Any]:
    params: dict[str, Any] = {"document_id": document}
    properties = tool["inputSchema"]["properties"]
    if "expected_revision" in properties:
        params["expected_revision"] = "0"
    if "operation_id" in properties:
        params["operation_id"] = f"refused-{tool['name']}"
    return params


def read_only_documents_refuse_every_other_tool(case: NativeAcceptance) -> None:
    """Every write and every unqualified read refuses before its arguments are checked."""
    scenes = corpus(case)
    worker = case.worker
    document = open_scene(case, worker, scenes["B"], "standalone")["document_id"]
    before = call(case, worker, "scene.summary", {"document_id": document})
    tools = call(case, worker, "automation.capabilities", {})["tools"]
    refused = 0
    for tool in tools:
        name = tool["name"]
        if "document_id" not in tool["inputSchema"]["properties"]:
            continue
        if name in FORWARDED or name in ANSWERED_BY_WORKER:
            continue
        details = refuse(
            case, worker, tool["name"], _tool_params(tool, document), "read_only_document"
        )
        write = not tool["annotations"]["readOnlyHint"]
        case.assertEqual(details["reason"], "mutation" if write else "not_qualified", tool)
        refused += 1
    case.assertGreater(refused, 50)
    case.assertEqual(call(case, worker, "scene.summary", {"document_id": document}), before)
    discard(case, worker, document)


def _refusals(column: str, objects: str, directory: str) -> list[tuple]:
    """(tool, valid arguments, reason, slice) for a sample of every refusal kind."""
    return [
        ("column.create", {"name": "Added"}, "mutation", None),
        ("level.create", {"name": "Added", "format": "vector"}, "mutation", None),
        ("camera.update", {"width_px": 64}, "mutation", None),
        ("fx.create", {"type": "STD_blurFx"}, "mutation", None),
        ("scene.save_copy", {"directory": directory}, "mutation", None),
        ("motion_path.list", {"expected_revision": "0"}, "not_qualified", None),
        ("rig.inspect", {"expected_revision": "0"}, "not_qualified", 3),
    ]


def read_only_documents_refuse_mutations(case: NativeAcceptance) -> None:
    """Writes, and reads not yet qualified, refuse with valid arguments and change nothing."""
    scenes = corpus(case)
    worker = case.worker
    document = open_scene(case, worker, scenes["B"], "standalone")["document_id"]
    before = call(case, worker, "scene.summary", {"document_id": document})
    column = before["columns"][0]["column_id"]
    objects = before["columns"][0]["object_id"]
    directory = str(case.workspace / "copy")
    tools = {
        tool["name"]: tool for tool in call(case, worker, "automation.capabilities", {})["tools"]
    }
    for name, extra, reason, slice_ in _refusals(column, objects, directory):
        params = {"document_id": document, **extra}
        if not tools[name]["annotations"]["readOnlyHint"]:
            params.update(expected_revision="0", operation_id=_op(case))
        details = refuse(case, worker, name, params, "read_only_document")
        case.assertEqual((details["reason"], details.get("slice")), (reason, slice_), name)
    case.assertEqual(call(case, worker, "scene.summary", {"document_id": document}), before)
    case.assertFalse((case.workspace / "copy").exists())
    submit = {"document_id": document, "expected_revision": "0", "format": "mp4"}
    submit.update(start_frame=0, end_frame=0, operation_id=_op(case))
    refuse(case, worker, "render.submit", submit, "render_unavailable")
    discard(case, worker, document)


def _receipts(case: NativeAcceptance, worker: Worker, scenes: dict[str, Path]) -> None:
    params = {"path": str(scenes["B"]), "project_id": "standalone", "operation_id": "same"}
    first = call(case, worker, "scene.open_read_only", params)
    case.assertEqual(call(case, worker, "scene.open_read_only", params), first)
    refuse(
        case,
        worker,
        "scene.open_read_only",
        {**params, "path": str(scenes["C"])},
        "operation_conflict",
    )
    case.assertEqual(len(host_processes(worker)), 1)
    mismatch = {"path": str(scenes["A"]), "project_id": "standalone", "operation_id": "kept"}
    error = refuse(case, worker, "scene.open_read_only", mismatch, "project_mismatch")
    case.assertEqual(
        refuse(case, worker, "scene.open_read_only", mismatch, "project_mismatch"), error
    )
    case.assertEqual(len(host_processes(worker)), 1)
    discard(case, worker, first["document_id"])


def _source_change(case: NativeAcceptance, worker: Worker, scenes: dict[str, Path]) -> None:
    """A source rewritten during a render fails the job; after it, renders refuse."""
    show = project_id(case, worker, "ShowA")
    document = open_scene(case, worker, scenes["A"], show)["document_id"]
    params = {"document_id": document, "expected_revision": "0", "start_frame": 0}
    params.update(end_frame=119, width=512, height=512, operation_id=_op(case))
    job = call(case, worker, "render.submit", params)["job_id"]
    level = scenes["A"].parent.parent / "drawings" / "level-2.0001.png"
    level.write_bytes(level.read_bytes() + b"\0")
    status = wait(case, worker, job)
    case.assertEqual(status["status"], "failed", status)
    case.assertEqual(status["failure"]["code"], "source_changed", status)
    case.assertEqual(status["failure"]["phase"], "verify", status)
    refuse(case, worker, "job.result", {"job_id": job}, "artifacts_unavailable")
    preview = {"document_id": document, "expected_revision": "0", "frame": 0}
    details = refuse(
        case, worker, "render.preview", {**preview, "operation_id": _op(case)}, "source_changed"
    )
    case.assertIn(str(level), details["paths"])
    discard(case, worker, document)


def _lost_host(case: NativeAcceptance, worker: Worker, scenes: dict[str, Path]) -> None:
    editable = case.create("Survivor")
    show = project_id(case, worker, "ShowA")
    rendering = open_scene(case, worker, scenes["A"], show)["document_id"]
    other = open_scene(case, worker, scenes["C"], "standalone")["document_id"]
    params = {"document_id": rendering, "expected_revision": "0", "start_frame": 0}
    params.update(end_frame=119, width=512, height=512, operation_id=_op(case))
    job = call(case, worker, "render.submit", params)["job_id"]
    pid = next(pid for pid, owner in host_processes(worker) if owner == rendering)
    os.kill(pid, signal.SIGKILL)
    record = call(case, worker, "job.status", {"job_id": job})
    case.assertEqual((record["status"], record["failure"]["code"]), ("failed", "scene_host_failed"))
    details = refuse(case, worker, "scene.summary", {"document_id": rendering}, "scene_host_failed")
    case.assertEqual(details["cause"], "crashed")
    states = {
        row["document_id"]: row["host_state"]
        for row in call(case, worker, "automation.list_documents", {})["documents"]
    }
    case.assertEqual((states[rendering], states[other], states[editable]), ("lost", "ready", None))
    call(case, worker, "cast.list", {"document_id": other, "expected_revision": "0"})
    case.success("scene.summary", {"document_id": editable})
    discard(case, worker, rendering)
    expired = refuse(case, worker, "job.status", {"job_id": job}, "unknown_job")
    case.assertEqual(expired["reason"], "expired")
    discard(case, worker, other)
    case.assertEqual(host_processes(worker), [])


def read_only_receipts_sources_and_lost_hosts(case: NativeAcceptance) -> None:
    """Receipts replay without a second host, pins refuse stale renders, a killed host
    leaves the worker and its other documents serving."""
    scenes = corpus(case)
    worker = case.worker
    _receipts(case, worker, scenes)
    _lost_host(case, worker, scenes)
    _source_change(case, worker, scenes)


def _slow_call(worker: Worker, method: str, params: dict[str, Any], timeout: float) -> Any:
    """One call allowed past the client's usual 30 second answer bound."""
    worker.sequence += 1
    identifier = f"acceptance-{worker.sequence}"
    payload = json.dumps({"id": identifier, "method": method, "params": params}).encode()
    response = worker.exchange(payload, timeout=timeout)
    if response.get("id") != identifier:
        raise AssertionError(f"Worker returned an unrelated request id: {response!r}")
    return response


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def _orphan_exits(case: NativeAcceptance, scene: Path) -> None:
    """A host blocked in a load ends when its worker is killed, not only on input EOF."""
    worker = fault_worker(case, "hang", "orphan")
    params = {"path": str(scene), "project_id": "standalone", "operation_id": "orphan"}
    request = {"id": "orphan", "method": "scene.open_read_only", "params": params}
    assert worker.process.stdin is not None
    worker.process.stdin.write(json.dumps(request).encode() + b"\n")
    worker.process.stdin.flush()
    deadline = time.monotonic() + 20
    while not host_processes(worker) and time.monotonic() < deadline:
        time.sleep(0.05)
    hosts = host_processes(worker)
    case.assertEqual(len(hosts), 1)
    worker.process.kill()
    worker.process.wait(timeout=10)
    deadline = time.monotonic() + 5
    while _alive(hosts[0][0]) and time.monotonic() < deadline:
        time.sleep(0.05)
    case.assertFalse(_alive(hosts[0][0]), "The orphaned host kept running")


def read_only_hosts_contain_native_failures(case: NativeAcceptance) -> None:
    """A host that crashes, hangs past the 60 s open deadline or grows past 2 GiB ends the
    open, never the worker."""
    scenes = corpus(case)
    _orphan_exits(case, scenes["B"])
    idle: list[Worker] = []
    for fault, cause, bound in (
        ("crash", "crashed", 30),
        ("memory", "memory", 30),
        ("hang", "timeout", 75),
    ):
        worker = fault_worker(case, fault)
        idle.append(worker)
        started = time.monotonic()
        params = {"path": str(scenes["B"]), "project_id": "standalone", "operation_id": fault}
        response = _slow_call(worker, "scene.open_read_only", params, bound + 15)
        case.assertEqual(response["error"]["code"], "invalid_scene", response)
        case.assertEqual(response["error"]["details"]["cause"], cause)
        elapsed = time.monotonic() - started
        case.assertLess(elapsed, bound)
        if fault == "hang":
            case.assertGreater(elapsed, 59)
        case.assertEqual(host_processes(worker), [])
        call(case, worker, "project.list", {"limit": 1})
        case.assertEqual(call(case, worker, "automation.list_documents", {})["documents"], [])
    _idle_workers_exit_within_the_deadline(case, idle[:2])


DEADLINE = 3.0
# Scheduling slack on a loaded machine, on top of the worker's own deadline.
SLACK = 1.0
DEADLINE_EXIT = 3


def close_and_time(case: NativeAcceptance, worker: Worker) -> tuple[float, int]:
    """Close a worker's input and return how long it took to exit, and its exit code."""
    assert worker.process.stdin is not None
    worker.process.stdin.close()
    started = time.monotonic()
    code = worker.process.wait(timeout=DEADLINE + 10)
    return time.monotonic() - started, code


def _idle_workers_exit_within_the_deadline(case: NativeAcceptance, workers: list[Worker]) -> None:
    """Workers left idle past macOS's nap threshold still exit within the deadline."""
    for worker in workers:
        elapsed, code = close_and_time(case, worker)
        case.assertEqual(code, 0)
        case.assertLess(elapsed, DEADLINE + SLACK)


def read_only_damaged_scenes_are_refused(case: NativeAcceptance) -> None:
    """Scene damage that once crashed, hung or exhausted the parser is refused cleanly."""
    scenes = corpus(case)
    text = (scenes["base"] / "scene.tnz").read_text()
    cell = re.compile(r"<cell>\s*0 [0-9]+ ")
    malformed = {
        "camera": text.replace('id="Camera1"', 'id="Camera-0"', 1),
        "ports": text.replace("<ports>", "<ports>x", 1),
        "rows": cell.sub("<cell>0 2000000000 ", text, count=1),
    }
    for name, broken in malformed.items():
        path = case.inputs / f"malformed-{name}.tnz"
        path.write_text(broken)
        details = _refused_open(case, path, "invalid_scene")
        case.assertEqual(details["cause"], "rejected", name)
    case.assertEqual(host_processes(case.worker), [])
    call(case, case.worker, "project.list", {"limit": 1})


def _projects(case: NativeAcceptance, worker: Worker, scenes: dict[str, Path]) -> None:
    show, other = project_id(case, worker, "ShowA"), project_id(case, worker, "ShowB")
    first = open_scene(case, worker, scenes["A"], show)
    second = open_scene(case, worker, scenes["A2"], other)
    rules = (first["project"]["file_path_rules"], second["project"]["file_path_rules"])
    case.assertEqual((rules[0]["use_standard"], rules[1]["use_standard"]), (True, False))
    case.assertEqual(rules[1]["letter_count_for_suffix"], 8)
    folders = {row["alias"]: row for row in second["project"]["folders"]}
    case.assertEqual(folders["outputs"]["authority"], "outside_roots")
    case.assertIsNone(folders["outputs"]["resolved_path"])
    case.assertEqual(folders["drawings"]["authority"], "input_root")
    for opened, folder in ((first, "drawings"), (second, "art")):
        params = {"document_id": opened["document_id"], "expected_revision": "0"}
        rows = call(case, worker, "scene.dependencies", params)["items"]
        root = str(scenes["A" if folder == "drawings" else "A2"].parent.parent / folder)
        primary = [row for row in rows if row["role"] == "primary"]
        case.assertTrue(all(row["resolved_path"].startswith(root) for row in primary), rows)
        preview(case, worker, opened["document_id"], 1)
    editable = case.create("Editable")
    case.mutate("level.import", editable, path=str(case.fixture), name="Still", dpi=120)
    case.mutate("scene.save_copy", editable, directory=str(case.workspace / "still"))
    for opened in (first, second):
        discard(case, worker, opened["document_id"])


def _listing(case: NativeAcceptance, worker: Worker) -> None:
    whole = call(case, worker, "project.list", {"limit": 64})
    names = {row["name"] for row in whole["projects"]}
    case.assertIn("ShowA", names)
    case.assertNotIn("Rogue", names)
    case.assertTrue(whole["scan"]["complete"], whole["scan"])
    first = call(case, worker, "project.list", {"limit": 1})
    case.assertEqual(first["next_offset"], 1)
    revision = first["listing_revision"]
    case.assertEqual(revision, whole["listing_revision"])
    second = call(
        case,
        worker,
        "project.list",
        {"limit": 1, "offset": 1, "expected_listing_revision": revision},
    )
    case.assertNotEqual(first["projects"], second["projects"])
    (case.inputs / "corpus" / "Late").mkdir()
    (case.inputs / "corpus" / "Late" / "Late_otprj.xml").write_text(project_xml({}))
    details = refuse(
        case,
        worker,
        "project.list",
        {"limit": 1, "expected_listing_revision": revision},
        "revision_conflict",
    )
    case.assertEqual(details["scope"], "listing")


def read_only_projects_bind_their_own_folders(case: NativeAcceptance) -> None:
    """Two projects with different +drawings and path rules open side by side; the
    editor's project roots and folders above a read root are never consulted."""
    scenes = corpus(case)
    rogue = case.directory / "elsewhere" / "Rogue"
    rogue.mkdir(parents=True)
    (rogue / "Rogue_otprj.xml").write_text(project_xml({}))
    above = case.directory / "scenes.xml"
    above.write_text(scenes_xml())
    above.chmod(0)
    case.addCleanup(above.chmod, 0o600)
    previous = os.environ.get("TOONZPROJECTS")
    os.environ["TOONZPROJECTS"] = str(rogue.parent)
    try:
        worker = Worker(
            case.executable, case.workspace, case.inputs, case.directory / "projects.stderr.log"
        )
    finally:
        if previous is None:
            os.environ.pop("TOONZPROJECTS")
        else:
            os.environ["TOONZPROJECTS"] = previous
    case.addCleanup(worker.close)
    _listing(case, worker)
    standalone = open_scene(case, worker, scenes["B"], "standalone")
    case.assertEqual(standalone["project"]["native_binding"]["status"], "none_within_roots")
    discard(case, worker, standalone["document_id"])
    _projects(case, worker, scenes)


def _level(identifier: int, name: str, path: str) -> str:
    coded = path if path.startswith("/") else f"$scenefolder/assets/{path}"
    return (
        f'<level id=\'{identifier}\'>\n{name} \n<info dpix="120.000000" dpiy="120.000000"/>\n'
        f'<path> "{coded}" </path>\n</level>\n'
    )


def read_only_level_prechecks_refuse_before_native_load(case: NativeAcceptance) -> None:
    """Partial vector levels, five-sample images and oversized first frames are refused
    by the reader pre-check, before native load could open a dialog or decode them."""
    base = base_package(case)
    folder = case.inputs / "prechecks"
    shutil.copytree(base / "assets", folder / "assets")
    assets = folder / "assets"
    (assets / "five.0001.tif").write_bytes(tiff(2, 2, 5, 8))
    (assets / "wide.0001.tif").write_bytes(tiff(16385, 1, 4, 8))
    (assets / "edge.0001.tif").write_bytes(tiff(16384, 1, 4, 8))
    vector = (base / "assets" / "level-0.pli").read_bytes()
    (assets / "part.pli").write_bytes(vector[: len(vector) * 3 // 4])
    levels = (
        _level(2300, "Five", "five..tif")
        + _level(2301, "Wide", "wide..tif")
        + _level(2302, "Edge", "edge..tif")
        + _level(2303, "Part", "part.pli")
        + _level(2304, "Layers", "layers.psd")
        + _level(2305, "Clip", "clip.mp4")
        + _level(2306, "Away", "/nowhere/away..png")
    )
    anchor = "</levels>\n    <folder"
    text = (base / "scene.tnz").read_text().replace(anchor, levels + anchor, 1)
    (folder / "shot.tnz").write_text(text)
    opened = open_scene(case, case.worker, folder / "shot.tnz", "standalone")
    rows = {row["name"]: row["reason"] for row in opened["content"]["unsupported"]}
    case.assertEqual(rows.get("Five"), "unsupported_samples", rows)
    case.assertEqual(rows.get("Wide"), "image_too_large", rows)
    case.assertIn(rows.get("Part"), {"partial_level", "level_load_failed"}, rows)
    case.assertNotIn("Edge", rows)
    case.assertEqual(rows.get("Layers"), "unsupported_level_format", rows)
    case.assertEqual(rows.get("Clip"), "unsupported_level_format", rows)
    case.assertEqual(rows.get("Away"), "forbidden_file", rows)
    params = {"document_id": opened["document_id"], "expected_revision": "0", "limit": 128}
    states = {
        row["name"]: row["load_status"]
        for row in call(case, case.worker, "cast.list", params)["items"]
    }
    case.assertEqual((states["Five"], states["Edge"]), ("unsupported_samples", "loaded"))
    case.assertEqual((states["Layers"], states["Away"]), ("not_applicable", "forbidden"))
    case.assertTrue(opened["render"]["renderable"], opened["render"])
    discard(case, case.worker, opened["document_id"])


def _linked_origin(text: str) -> str:
    """Record a linked Library origin on the Ink level, as the editor writes it."""
    origin = (
        '{\\"id\\":\\"rig-id\\",\\"item\\":\\"Rig\\",\\"kind\\":\\"rig\\",'
        '\\"source\\":\\"$scenefolder/assets/level-0.pli\\",\\"revision\\":1,\\"relationship\\":1}'
    )
    return re.sub(r"(Ink \s*<info[^>]*>\s*<path) ", rf'\1 libraryOrigin="{origin}" ', text, count=1)


def _special_columns(text: str) -> str:
    """Add palette, sound-text and mesh levels and a column exposing each."""
    levels = (
        "<paletteLevel id='2400'> <name> Swatches </name> "
        '<path> "$scenefolder/assets/level-1.tpl" </path> </paletteLevel>\n'
        "<soundTextLevel id='2401'> Words <type> textSound </type> "
        '<frame> "hello" </frame> </soundTextLevel>\n'
        "<level id='2402'>\nMesh \n"
        '<path> "$scenefolder/assets/shape.mesh" </path>\n</level>\n'
    )
    columns = (
        "<paletteColumn id='2410'> <cells> <cell> 0 6 <paletteLevel id='2400'/> 1 0 "
        "</cell> </cells> </paletteColumn>\n"
        "<soundTextColumn id='2411'> <cells> <cell> 0 1 <soundTextLevel id='2401'/> "
        "</cell> </cells> </soundTextColumn>\n"
        "<meshColumn id='2412'> <cells> <cell> 0 6 <level id='2402'/> 1 0 "
        "</cell> </cells> </meshColumn>\n"
    )
    anchor = "</levels>\n    <folder"
    text = text.replace(anchor, levels + anchor, 1)
    return text.replace("<columns>", "<columns>\n" + columns, 1)


def _textures(palette: str) -> str:
    """A TLV palette with a texture outside the roots and a shipped library texture."""
    styles = (
        '<style> texture_2 4 "/nowhere/stone.bmp" 255 0 0 255 0 0 1 0 1 1 1 </style>'
        '<style> texture_3 4 "brick.bmp" 255 0 0 255 0 0 1 0 1 1 1 </style>'
    )
    palette = palette.replace("</styles>", styles + "</styles>", 1)
    return re.sub(r"<indices>\s*0 1\s*</indices>", "<indices> 0 1 2 3 </indices>", palette)


def _rows(opened: dict[str, Any]) -> set[tuple[str, str, str]]:
    return {
        (row["kind"], row["reason"], row["name"] or "") for row in opened["content"]["unsupported"]
    }


def _special(case: NativeAcceptance, base: Path) -> None:
    text = _linked_origin(_special_columns((base / "scene.tnz").read_text()))
    opened = open_scene(case, case.worker, _variant(case, base, "special", text), "standalone")
    rows = {(kind, reason) for kind, reason, _name in _rows(opened)}
    expected = {
        ("column", "unsupported_column"),
        ("level", "unsupported_level_format"),
        ("level", "linked_library_origin"),
    }
    case.assertTrue(expected <= rows, rows)
    columns = [row for row in opened["content"]["unsupported"] if row["kind"] == "column"]
    case.assertEqual(len(columns), 3, columns)
    case.assertTrue(opened["render"]["renderable"], opened["render"])
    read = {"document_id": opened["document_id"], "expected_revision": "0", "limit": 128}
    cast = {row["name"]: row for row in call(case, case.worker, "cast.list", read)["items"]}
    case.assertEqual(cast["Ink"]["library"]["relationship"], "linked")
    case.assertEqual(
        (cast["Mesh"]["kind"], cast["Mesh"]["load_status"]), ("mesh", "not_applicable")
    )
    case.assertEqual(cast["Swatches"]["kind"], "palette")
    case.assertEqual(cast["Words"]["kind"], "sound_text")
    discard(case, case.worker, opened["document_id"])


def _styles(case: NativeAcceptance, base: Path) -> None:
    folder = case.inputs / "admission" / "textures"
    shutil.copytree(base / "assets", folder / "assets")
    palette = folder / "assets" / "level-1.tpl"
    palette.write_text(_textures(palette.read_text()))
    (folder / "shot.tnz").write_text((base / "scene.tnz").read_text())
    opened = open_scene(case, case.worker, folder / "shot.tnz", "standalone")
    rows = {(kind, reason) for kind, reason, _name in _rows(opened)}
    case.assertIn(("style", "external_file_reference"), rows)
    case.assertIn(("style", "library_resource_unavailable"), rows)
    pairs = {f"{row['reason']}/{row['rule']}" for row in opened["render"]["blockers"]}
    case.assertIn("external_file_reference/anywhere", pairs)
    case.assertIn("library_resource_unavailable/rendered", pairs)
    discard(case, case.worker, opened["document_id"])


def sound_package(case: NativeAcceptance, directory: Path) -> Path:
    """Save a worker scene with one sound column playing a WAV clip."""
    document = case.create("Sounding")
    path = case.inputs / "tone.wav"
    path.write_bytes(wav_file(tone(48000, "pcm_s16", 9600)))
    level = case.mutate("audio.import", document, path=str(path), name="Tone")["source"]
    column = case.mutate("audio.column.create", document, name="Sound", volume_permille=1000)
    placement = {"timeline_start_frame": 0, "source_start_frame": 0, "frame_count": 4}
    case.mutate(
        "audio.clip.place",
        document,
        column_id=column["column"]["column_id"],
        level_id=level["level_id"],
        placement=placement,
        overlap_policy="reject",
    )
    saved = case.mutate("scene.save_copy", document, directory=str(directory))
    return Path(saved["scene_path"])


def _sound(case: NativeAcceptance) -> None:
    scene = sound_package(case, case.workspace / "sounding")
    opened = open_scene(case, case.worker, scene, "standalone")
    case.assertEqual(opened["counts"]["sound_levels"], 1)
    case.assertTrue(opened["render"]["renderable"], opened["render"])
    read = {"document_id": opened["document_id"], "expected_revision": "0"}
    cast = {row["name"]: row for row in call(case, case.worker, "cast.list", read)["items"]}
    case.assertEqual(
        (cast["Tone"]["kind"], cast["Tone"]["load_status"]), ("sound", "not_applicable")
    )
    preview(case, case.worker, opened["document_id"], 0)
    discard(case, case.worker, opened["document_id"])


def read_only_content_rows_cover_columns_levels_and_styles(case: NativeAcceptance) -> None:
    """Sound, sound-text, palette and mesh columns, a linked Library origin and texture
    styles are each reported, and block renders only by their rule."""
    base = base_package(case)
    _special(case, base)
    _styles(case, base)
    _sound(case)


# ---------------------------------------------------------------- limits

ANCHOR = "</levels>\n    <folder"


def _blurs(count: int, first: int) -> str:
    return "".join(
        f"<fxnode> <STD_blurFx id='{first + index}'> <name> b </name> </STD_blurFx> </fxnode>"
        for index in range(count)
    )


def _child(identifier: int, effects: int = 0) -> str:
    """A sub-xsheet exposing the Ink level, with extra effects in its own graph."""
    child = _child_level(1).replace(f"'{CHILD_ID}'", f"'{identifier}'")
    for offset in range(1, 6):
        child = child.replace(f"'{CHILD_ID + offset}'", f"'{identifier + offset}'")
    if effects:
        internal = f"<internal> {_blurs(effects, identifier + 10)} </internal> "
        child = child.replace("<fxnodes> ", f"<fxnodes> {internal}", 1)
    return child


def _exposing(identifier: int, column: int) -> str:
    return (
        f"<levelColumn id='{column}'> <status> 0 </status> <cells> <cell> "
        f"0 1 <childLevel id='{identifier}'/>1 0 </cell> </cells> </levelColumn>\n"
    )


def _columns(text: str, count: int) -> str:
    plain = "".join(
        f"<levelColumn id='{900000 + index}'> <status> 0 </status> </levelColumn>\n"
        for index in range(count)
    )
    return text.replace("<columns>", "<columns>\n" + plain, 1)


def _with_children(text: str, children: list[str], columns: str, effects: str = "") -> str:
    """Add top-sheet columns and effects first: child definitions hold their own
    <columns> and <internal> tags, and come before the top sheet in the file."""
    text = text.replace("<columns>", "<columns>\n" + columns, 1)
    text = text.replace("<internal>", "<internal>" + effects, 1)
    return text.replace(ANCHOR, "".join(children) + ANCHOR, 1)


def _sheets(text: str, children: int) -> str:
    identifiers = [1000000 + 10 * index for index in range(children)]
    columns = "".join(
        _exposing(identifier, 2000000 + index) for index, identifier in enumerate(identifiers)
    )
    return _with_children(text, [_child(identifier) for identifier in identifiers], columns)


def _scene_effects(text: str, top: int, per_child: int) -> str:
    """Four sub-xsheets with per_child extra effects each, and top extra top effects."""
    identifiers = [3000000 + 10000 * index for index in range(4)]
    columns = "".join(_exposing(identifier, identifier + 9999) for identifier in identifiers)
    children = [_child(identifier, per_child) for identifier in identifiers]
    return _with_children(text, children, columns, _blurs(top, 4000000))


def _pegbars(text: str, count: int) -> str:
    pegbars = "".join(f'<pegbar id="Peg{index + 1}"> </pegbar>\n' for index in range(count))
    return text.replace('<pegbar id="Table">', pegbars + '<pegbar id="Table">', 1)


def _row(text: str, row: int) -> str:
    column = (
        f"<levelColumn id='950000'> <status> 0 </status> <cells> <cell> {row} 1 "
        "<level id='1'/>0001 0 </cell> </cells> </levelColumn>\n"
    )
    return text.replace("<columns>", "<columns>\n" + column, 1)


def _bound(
    case: NativeAcceptance, base: Path, name: str, scenes: tuple[str, str], limit: str
) -> dict[str, Any]:
    """Open the scene at a bound, then refuse the one past it; returns the open report."""
    at = open_scene(case, case.worker, _variant(case, base, name + "-at", scenes[0]), "standalone")
    discard(case, case.worker, at["document_id"])
    details = _refused_open(case, _variant(case, base, name + "-past", scenes[1]), "scene_limit")
    case.assertEqual(details["limit"], limit, name)
    return at


def read_only_structure_limits_hold_at_their_bounds(case: NativeAcceptance) -> None:
    """Columns, effects per sheet and per scene, sheets and stage objects: the bound opens,
    one more is a scene_limit; rows past 999,999 are refused by the native parser."""
    base = base_package(case)
    text = (base / "scene.tnz").read_text()
    plain = open_scene(case, case.worker, _variant(case, base, "plain", text), "standalone")
    discard(case, case.worker, plain["document_id"])
    effects, objects = plain["counts"]["fx_nodes"], plain["counts"]["stage_objects"]
    columns = plain["counts"]["columns"]
    at = _bound(
        case,
        base,
        "columns",
        (_columns(text, 9999 - columns), _columns(text, 10000 - columns)),
        "columns_per_sheet",
    )
    case.assertEqual(at["counts"]["columns"], 9999)
    sheet = (
        text.replace("<internal>", "<internal>" + _blurs(4096 - effects, 5000000), 1),
        text.replace("<internal>", "<internal>" + _blurs(4097 - effects, 5000000), 1),
    )
    case.assertEqual(
        _bound(case, base, "sheet-effects", sheet, "fx_nodes_per_sheet")["counts"]["fx_nodes"], 4096
    )
    top = 16384 - effects - 4 * 4095
    whole = (_scene_effects(text, top, 4093), _scene_effects(text, top + 1, 4093))
    case.assertEqual(
        _bound(case, base, "scene-effects", whole, "fx_nodes")["counts"]["fx_nodes"], 16384
    )
    case.assertEqual(
        _bound(case, base, "sheets", (_sheets(text, 1023), _sheets(text, 1024)), "sheets")[
            "counts"
        ]["sheets"],
        1024,
    )
    pegs = (_pegbars(text, 16384 - objects), _pegbars(text, 16385 - objects))
    case.assertEqual(
        _bound(case, base, "objects", pegs, "stage_objects_per_sheet")["counts"]["stage_objects"],
        16384,
    )
    late = open_scene(
        case, case.worker, _variant(case, base, "row-at", _row(text, 999999)), "standalone"
    )
    case.assertEqual(late["counts"]["frames"], 1000000)
    discard(case, case.worker, late["document_id"])
    details = _refused_open(
        case, _variant(case, base, "row-past", _row(text, 1000000)), "invalid_scene"
    )
    case.assertEqual(details["cause"], "rejected")


def _padded(text: str, size: int) -> str:
    """The scene text followed by spaces to exactly size bytes."""
    return text + " " * (size - len(text.encode()))


def _file_sizes(case: NativeAcceptance, base: Path, scenes: dict[str, Path]) -> None:
    text = (base / "scene.tnz").read_text()
    limit = 64 * 1024 * 1024
    at = open_scene(
        case, case.worker, _variant(case, base, "size-at", _padded(text, limit)), "standalone"
    )
    case.assertEqual(at["source"]["size_bytes"], limit)
    discard(case, case.worker, at["document_id"])
    details = _refused_open(
        case, _variant(case, base, "size-past", _padded(text, limit + 1)), "scene_limit"
    )
    case.assertEqual((details["limit"], details["value"]), ("scene_file_bytes", limit + 1))
    project = scenes["A"].parent.parent / "ShowA_otprj.xml"
    show = project_id(case, case.worker, "ShowA")
    original = project.read_text()
    project.write_text(_padded(original, 1024 * 1024))
    opened = open_scene(case, case.worker, scenes["A"], show)
    discard(case, case.worker, opened["document_id"])
    project.write_text(_padded(original, 1024 * 1024 + 1))
    details = _refused_open(case, scenes["A"], "invalid_project", project=show)
    case.assertEqual(details["cause"], "too_large")
    project.write_text(original)
    binding = scenes["A"].parent / "scenes.xml"
    binding.write_text(_padded(scenes_xml(), 64 * 1024))
    opened = open_scene(case, case.worker, scenes["A"], show)
    case.assertEqual(opened["project"]["native_binding"]["status"], "matches")
    discard(case, case.worker, opened["document_id"])
    binding.write_text(_padded(scenes_xml(), 64 * 1024 + 1))
    details = _refused_open(case, scenes["A"], "project_mismatch", project=show)
    case.assertEqual((details["native_binding"], details["cause"]), ("unreadable", "too_large"))


def _row_limits(case: NativeAcceptance, base: Path) -> None:
    text = (base / "scene.tnz").read_text()
    program = case.inputs / "never-run"
    for count in (64, 65, 256, 257):
        nodes = "".join(
            f"<fxnode>{external_effect(program, 6000000 + index)}</fxnode>"
            for index in range(count)
        )
        scene = text.replace("<internal>", "<internal>" + nodes, 1)
        opened = open_scene(
            case, case.worker, _variant(case, base, f"rows-{count}", scene), "standalone"
        )
        content, render = opened["content"], opened["render"]
        case.assertEqual((content["unsupported_count"], render["blocker_count"]), (count, count))
        case.assertEqual(len(content["unsupported"]), min(count, 256))
        case.assertEqual(content["truncated"], count > 256)
        case.assertEqual(len(render["blockers"]), min(count, 64))
        discard(case, case.worker, opened["document_id"])


def _documents(case: NativeAcceptance, scenes: dict[str, Path]) -> None:
    for index in range(28):
        case.create(f"Filler {index}")
    opened = [
        open_scene(case, case.worker, scenes["B"], "standalone")["document_id"] for _ in range(4)
    ]
    case.assertEqual(len(call(case, case.worker, "automation.list_documents", {})["documents"]), 32)
    discard(case, case.worker, opened.pop())
    case.mutate("scene.create", None, name="Last")
    details = _refused_open(case, scenes["B"], "resource_limit")
    case.assertEqual(details["limit"], "documents")
    for document in opened:
        discard(case, case.worker, document)


def read_only_file_and_report_limits_hold_at_their_bounds(case: NativeAcceptance) -> None:
    """Scene, project and scenes.xml sizes, the 32-document limit and the 256 unsupported
    and 64 blocker rows: the bound is admitted, one more is refused or truncated."""
    scenes = corpus(case)
    _file_sizes(case, scenes["base"], scenes)
    _row_limits(case, scenes["base"])
    _documents(case, scenes)
