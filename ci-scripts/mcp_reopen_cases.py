"""Packages reopen by their registered content, wherever they were moved, in a later worker."""

from __future__ import annotations

import base64
import hashlib
import json
import platform
import shutil
import subprocess
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_audio_cases import _inspect, _mixdown, _package_scene
from mcp_audio_cases import _without_ids as _sound_without_ids
from mcp_case import acceptance_case
from mcp_journal_cases import CRASH_EXIT_CODE, restart, status
from mcp_macro_cases import _chain, _macro
from mcp_palette_cases import COLOR, DRAWING, SECOND_COLOR, _artwork, _make_artwork, _styles
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


def _frames(case: NativeAcceptance, document: str) -> list[bytes]:
    """Three rendered frames of the document, as PNG bytes."""
    frames = []
    for frame in (0, 6, 12):
        job = case.mutate("render.preview", document, frame=frame, width=32, height=32)
        case.assertEqual(case.wait_job(job["job_id"])["status"], "completed")
        artifact = case.success("job.result", {"job_id": job["job_id"]})["artifacts"][0]
        image = case.success("artifact.read", {"artifact_id": artifact["artifact_id"]})
        frames.append(base64.b64decode(image["image"]["data"]))
    return frames


def _without_ids(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            key: _without_ids(item)
            for key, item in value.items()
            # The scene name is the package's file name after any reopen.
            if not key.endswith("_id") and key not in {"revision", "operation_id", "name"}
        }
    if isinstance(value, list):
        return [_without_ids(item) for item in value]
    return value


def _sorted(value: Any) -> Any:
    """The FX graph lists nodes in the order the loader made them: compare as sets."""
    if isinstance(value, dict):
        return {key: _sorted(item) for key, item in value.items()}
    if isinstance(value, list):
        return sorted((_sorted(item) for item in value), key=json.dumps)
    return value


def _shape(case: NativeAcceptance, document: str) -> Any:
    summary = _without_ids(case.success("scene.summary", {"document_id": document}))
    graph = _without_ids(case.success("fx.graph", {"document_id": document}))
    return [summary, _sorted(graph)]


def _macro_scene(case: NativeAcceptance) -> str:
    document, _artwork, blur, glow = _chain(case)
    _macro(case, document, [glow, blur])
    return document


@acceptance_case
def relocated_package_reopens_in_a_new_worker(case: NativeAcceptance) -> None:
    """A package saved by one worker, moved elsewhere in the workspace and then into an input
    root, opens in a later worker by its registered content, from a private copy, with the
    same structure and pixels; its save is still answered from the journal."""
    document = _macro_scene(case)
    frames, shape = _frames(case, document), _shape(case, document)
    save = case.write_params(document, {"directory": str(case.workspace / "saved")})
    saved = case.success("scene.save_copy", save)
    case.assertIs(saved["registered"], True)
    case.assertRegex(saved["package_id"], "^package:[0-9a-f]{64}$")
    restart(case, "moved")
    moved = case.workspace / "elsewhere" / "moved-package"
    moved.parent.mkdir()
    shutil.move(saved["package_directory"], moved)
    case.assertNotIn(str(case.workspace), (moved / "scene.tnz").read_text(errors="replace"))
    opened = case.mutate("scene.open", None, path=str(moved))
    case.assertEqual(
        (opened["admission"], opened["package_id"], opened["relocated"]),
        ("workspace_registry", saved["package_id"], True),
    )
    case.assertIs(opened["published_by"]["current"], False)
    reopened = opened["document_id"]
    case.assertEqual(_shape(case, reopened), shape)
    case.assertEqual(_frames(case, reopened), frames)
    case.assertEqual(len(list(case.workspace.glob("runtime_*/opened-*"))), 1)
    case.mutate("scene.discard", reopened)
    case.assertEqual(list(case.workspace.glob("runtime_*/opened-*")), [])
    case.assertIs(case.success("scene.save_copy", save)["receipt"]["handles_expired"], True)
    inside = case.inputs / "relocated"
    shutil.move(str(moved), inside)
    info = case.success("scene.package_info", {"path": str(inside)})
    case.assertEqual((info["admission"], info["relocated"]), ("workspace_registry", True))
    again = case.mutate("scene.open", None, path=str(inside / "manifest.json"))
    case.assertEqual(_frames(case, again["document_id"]), frames)
    case.assertEqual(again["location"]["root"], "input")
    volume = _input_volume(case)
    if volume is not None:
        shutil.move(str(inside), volume / "elsewhere")
        far = case.mutate("scene.open", None, path=str(volume / "elsewhere"))
        case.assertEqual(_frames(case, far["document_id"]), frames)


def _input_volume(case: NativeAcceptance) -> Path | None:
    """A disk image mounted inside the input root: another volume the worker may read."""
    if platform.system() != "Darwin":
        return None
    image = case.directory / "input-volume.dmg"
    mount = case.inputs / "volume"
    mount.mkdir()
    subprocess.run(
        ["hdiutil", "create", "-size", "32m", "-fs", "APFS", "-volname", "gzqin", str(image)],
        check=True,
        capture_output=True,
    )
    subprocess.run(
        ["hdiutil", "attach", str(image), "-mountpoint", str(mount), "-nobrowse"],
        check=True,
        capture_output=True,
    )
    case.addCleanup(
        subprocess.run, ["hdiutil", "detach", "-force", str(mount)], capture_output=True
    )
    return mount


def _refused(case: NativeAcceptance, path: Path) -> dict[str, Any]:
    return case.failure("scene.open", case.write_params(None, {"path": str(path)}))


@acceptance_case
def changed_and_foreign_packages_are_refused(case: NativeAcceptance) -> None:
    """Admission is by registered content: an edited manifest, an edited or extra file, and a
    package another workspace registered are refused and say why; nothing is loaded."""
    document, _level, _column = case.populated_scene()
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "original"))
    original = Path(saved["package_directory"])
    restart(case, "checker")
    copies = {name: case.workspace / name for name in ("copy", "asset", "extra")}
    for path in copies.values():
        shutil.copytree(original, path)
    case.assertEqual(case.mutate("scene.open", None, path=str(copies["copy"]))["relocated"], True)
    asset = next(path for path in (copies["asset"] / "assets").iterdir() if path.is_file())
    asset.write_bytes(asset.read_bytes() + b"\0")
    case.assertEqual(_refused(case, copies["asset"])["code"], "invalid_package")
    (copies["extra"] / "extra.txt").write_text("not listed")
    case.assertEqual(_refused(case, copies["extra"])["code"], "invalid_package")
    manifest = original / "manifest.json"
    manifest.write_bytes(manifest.read_bytes() + b"\n")
    changed = _refused(case, original)
    case.assertEqual(
        (changed["code"], changed["details"]["reason"], changed["details"]["next"]),
        ("unregistered_package", "manifest_changed", "scene.import_editable"),
    )
    foreign = _foreign_package(case)
    case.assertEqual(_refused(case, foreign)["details"]["reason"], "not_registered")
    case.assertEqual(len(case.success("automation.list_documents", {})["documents"]), 1)
    case.assertEqual(len(list(case.workspace.glob("runtime_*/opened-*"))), 1)


def _foreign_package(case: NativeAcceptance) -> Path:
    """A package saved and registered by a worker of another workspace, copied in."""
    other = case.directory / "other-workspace"
    other.mkdir()
    worker = Worker(case.executable, other, case.inputs, case.directory / "other.stderr.log")
    case.addCleanup(worker.close)
    created = worker.call("scene.create", {"name": "Other", "operation_id": "foreign-create"})
    document = created["result"]["document_id"]
    worker.call(
        "level.create",
        {
            "document_id": document,
            "expected_revision": "1",
            "operation_id": "foreign-level",
            "name": "Ink",
            "format": "vector",
        },
    )
    summary = worker.call("scene.summary", {"document_id": document})["result"]
    level = summary["levels"][0]["level_id"]
    params = {"document_id": document, "expected_revision": "2", "operation_id": "foreign-draw"}
    worker.call(
        "drawing.create",
        {
            **params,
            "level_id": level,
            "format": "vector",
            "drawing_id": {"number": 1, "suffix": ""},
        },
    )
    params = {"document_id": document, "expected_revision": "3", "operation_id": "foreign-save"}
    saved = worker.call("scene.save_copy", {**params, "directory": str(other / "foreign")})
    case.assertIn("result", saved, saved)
    worker.close()
    target = case.inputs / "foreign"
    shutil.copytree(other / "foreign", target)
    return target


@acceptance_case(worker_only="needs --test-write-fault, which the adapter never passes")
def crash_during_copy_in_leaves_the_package(case: NativeAcceptance) -> None:
    """A worker killed while copying a package in leaves the package as it was; the next
    worker removes the partial copy, reports the open interrupted and opens it anew."""
    document, _level, _column = case.populated_scene()
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "kept"))
    package = Path(saved["package_directory"])
    before = sorted((path.name, path.stat().st_size) for path in package.rglob("*"))
    restart(case, "copy-in", "--test-write-fault", "reopen:copy:crash")
    request = case.write_params(None, {"path": str(package)})
    try:
        case.worker.call("scene.open", request)
    except RuntimeError:
        pass
    case.assertEqual(case.worker.process.wait(timeout=30), CRASH_EXIT_CODE)
    restart(case, "after-copy-in")
    status(case, request["operation_id"])
    case.assertEqual(list(case.workspace.glob("runtime_*/opened-*")), [])
    case.assertEqual(
        sorted((path.name, path.stat().st_size) for path in package.rglob("*")), before
    )
    answer = status(case, request["operation_id"])
    case.assertEqual(
        (answer["state"], answer["verdict"]["kind"]), ("interrupted", "lost_with_worker")
    )
    case.assertEqual(
        case.mutate("scene.open", None, path=str(package))["admission"], "workspace_registry"
    )


@acceptance_case(worker_only="needs --test-journal-limits, which the adapter never passes")
def expired_registration_says_so(case: NativeAcceptance) -> None:
    """Past the registry's limit the oldest package registration keeps only an expired mark:
    opening that package says registry_expired and points to the import."""
    small = ("--test-journal-limits", "small")
    restart(case, "registry", *small)
    document, _level, _column = case.populated_scene()
    saved = []
    for index in range(6):
        # Each save differs, so each is a package of its own.
        case.mutate("scene.settings", document, frame_rate=12 + index)
        path = str(case.workspace / f"p{index}")
        saved.append(case.mutate("scene.save_copy", document, directory=path))
    for index in range(70):
        case.mutate("scene.settings", document, width_px=32 + index % 2)
    restart(case, "after-registry", *small)
    expired = _refused(case, Path(saved[0]["package_directory"]))
    case.assertEqual(
        (expired["code"], expired["details"]["reason"]),
        ("unregistered_package", "registry_expired"),
    )
    newest = case.mutate("scene.open", None, path=saved[-1]["package_directory"])
    case.assertEqual(newest["admission"], "workspace_registry")
    case.assertLessEqual(case.success("journal.status", {})["retained"]["packages"], 4)


def _art_level(case: NativeAcceptance, document: str, image_format: str) -> str:
    """A level of one image format with a styled, animated palette, artwork and a column."""
    name = f"Art-{image_format}"
    level = case.mutate("level.create", document, name=name, format=image_format)["level_id"]
    sizes = {} if image_format == "vector" else {"width": 2, "height": 2, "dpi": 120}
    case.mutate(
        "drawing.create",
        document,
        level_id=level,
        drawing_id=DRAWING,
        format=image_format,
        **sizes,
    )
    for style in ("Paint", "Ink"):
        case.mutate("palette.create_style", document, level_id=level, name=style, color=COLOR)
    for frame, color in ((0, COLOR), (12, SECOND_COLOR)):
        case.mutate(
            "palette.update_style",
            document,
            level_id=level,
            style_id=2,
            keyframe=frame,
            color=color,
        )
    _make_artwork(case, document, level, image_format)
    column = case.mutate("column.create", document, name=f"Held {image_format}")["column_id"]
    case.mutate(
        "column.expose",
        document,
        level_id=level,
        drawing_id=DRAWING,
        column_id=column,
        start_frame=0,
        length=2,
    )
    return level


def _resources(case: NativeAcceptance, document: str, levels: dict[str, str]) -> dict[str, Any]:
    """Everything the scene holds that a relocated copy must give back unchanged."""
    return {
        "shape": _shape(case, document),
        "sound": _sound_without_ids(_inspect(case, document)),
        "mix": Path(_mixdown(case, document, 0, 21)["output_path"]).read_bytes(),
        "palettes": {
            name: _without_ids(_styles(case, document, level)) for name, level in levels.items()
        },
        "artwork": {
            name: _artwork(case, document, level, name.split("-")[1])
            for name, level in levels.items()
        },
        "frames": _frames(case, document),
    }


@acceptance_case
def a_relocated_package_keeps_its_sound_palettes_and_raster_levels(
    case: NativeAcceptance,
) -> None:
    """A scene with sound columns, a vector level and a Toonz raster level, each with an
    animated palette and artwork, reopens from another folder in a later worker with the same
    sound mix, palettes, artwork, structure and rendered frames (PRD-10 #3, SCN-04 #1)."""
    document, _ids, _sound = _package_scene(case)
    levels = {f"Art-{kind}": _art_level(case, document, kind) for kind in ("vector", "toonzRaster")}
    before = _resources(case, document, levels)
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "saved"))
    restart(case, "moved-resources")
    moved = case.workspace / "elsewhere" / "moved-resources"
    moved.parent.mkdir()
    shutil.move(saved["package_directory"], moved)
    opened = case.mutate("scene.open", None, path=str(moved))
    case.assertIs(opened["relocated"], True)
    reopened = opened["document_id"]
    by_name = {row["name"]: row["level_id"] for row in _shape_levels(case, reopened)}
    after = _resources(case, reopened, {name: by_name[name] for name in levels})
    for key in before:
        case.assertEqual(after[key], before[key], key)


def _shape_levels(case: NativeAcceptance, document: str) -> list[dict[str, Any]]:
    return case.success("scene.summary", {"document_id": document})["levels"]


def _forge(package: Path) -> None:
    """Change one asset and rewrite the manifest so it lists the change consistently."""
    manifest = json.loads((package / "manifest.json").read_text())
    entry = next(row for row in manifest["files"] if row["path"].startswith("assets/"))
    asset = package / entry["path"]
    asset.write_bytes(asset.read_bytes() + b"\0")
    data = asset.read_bytes()
    entry.update(size_bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
    (package / "manifest.json").write_text(json.dumps(manifest, indent=4, sort_keys=True))


@acceptance_case
def forged_package_with_consistent_manifest_is_refused(case: NativeAcceptance) -> None:
    """A copy whose asset and manifest were both edited to agree is a manifest nobody
    registered: it is refused and nothing of it is loaded."""
    document, _level, _column = case.populated_scene()
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "real"))
    forged = case.workspace / "forged"
    shutil.copytree(saved["package_directory"], forged)
    _forge(forged)
    before = case.success("automation.list_documents", {})
    error = _refused(case, forged)
    case.assertEqual(
        (error["code"], error["details"]["reason"]), ("unregistered_package", "not_registered")
    )
    info = case.failure("scene.package_info", {"path": str(forged)})
    case.assertEqual(info["code"], "unregistered_package")
    case.assertEqual(case.success("automation.list_documents", {}), before)
    case.assertEqual(list(case.workspace.glob("runtime_*/opened-*")), [])


@acceptance_case
def own_package_loads_from_a_private_copy(case: NativeAcceptance) -> None:
    """The worker that saved a package also loads it from a private copy: a file changed
    after the save is refused, and an unchanged package opens from its copy."""
    document, _level, _column = case.populated_scene()
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "own"))
    opened = case.mutate("scene.open", None, path=saved["package_directory"])
    case.assertEqual((opened["admission"], opened["relocated"]), ("this_worker", False))
    case.assertEqual(opened["location"], saved["location"])
    case.assertEqual(len(list(case.workspace.glob("runtime_*/opened-*"))), 1)
    manifest = json.loads(Path(saved["manifest_path"]).read_text())
    asset = Path(saved["package_directory"]) / next(
        row["path"] for row in manifest["files"] if row["path"].startswith("assets/")
    )
    asset.write_bytes(asset.read_bytes() + b"\0")
    case.assertEqual(_refused(case, Path(saved["package_directory"]))["code"], "invalid_package")
    case.assertEqual(len(list(case.workspace.glob("runtime_*/opened-*"))), 1)


@acceptance_case
def more_than_128_saves_per_worker(case: NativeAcceptance) -> None:
    """A worker saves as many packages as the registry holds, past the old 128 per worker."""
    document = case.create("Many saves")
    for index in range(129):
        saved = case.mutate(
            "scene.save_copy", document, directory=str(case.workspace / f"save-{index:03d}")
        )
        case.assertIs(saved["registered"], True)
    last = case.mutate("scene.open", None, path=saved["package_directory"])
    case.assertEqual(last["location"]["path"], "save-128")
