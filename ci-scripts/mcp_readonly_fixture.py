"""Build one scene that every kind of read tool has something to read in."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest import mock

from mcp_audio_model import constant, wav_file
from mcp_library_cases import _build_library, _package_scene
from mcp_lipsync_cases import DAT_CUES, dat_file
from mcp_readonly_tree import Tree, snapshot, snapshot_named
from mcp_rig_cases import _arm_rig
from mcp_scene_access_cases import _op, _variant, corpus, open_scene, project_id, wait
from mcp_studio_palette_cases import _build_studio, call
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

USER_LEVEL_NAMES = ("GroundzeroAutomation*", "com.GroundzeroAutomation*")
VECTOR_DRAWING = {"number": 1, "suffix": ""}
RASTER_SIZE = 4


@dataclass
class Fixture:
    """Handles to everything the read calls name, plus the roots the harness hashes."""

    case: NativeAcceptance
    worker: Worker
    roots: dict[str, Path]
    document: str = ""
    reopened: str = ""
    bare: str = ""
    ids: dict[str, str] = field(default_factory=dict)
    package: Path = field(default_factory=Path)
    input_package: Path = field(default_factory=Path)
    read_only: dict[str, str] = field(default_factory=dict)
    user_directories: dict[str, Path] = field(default_factory=dict)

    def watched(self) -> Tree:
        """Hash the granted roots, the worker's temporary directory and its user-level files."""
        return {**snapshot(self.roots), **snapshot_named(self.user_directories, USER_LEVEL_NAMES)}

    @property
    def documents(self) -> list[str]:
        """The documents every read is checked against."""
        return [self.document, self.reopened, self.bare]

    def revision(self, document: str) -> str:
        """Return the current revision the host reports for a document."""
        listed = self.case.success("automation.list_documents", {})["documents"]
        return next(item["revision"] for item in listed if item["document_id"] == document)


def _shared_roots(case: NativeAcceptance) -> dict[str, Path]:
    studio, project = case.directory / "studio", case.directory / "project-palettes"
    _build_studio(studio, project)
    library = case.directory / "library"
    package = _package_scene(case)
    _build_library(case, package, library)
    return {"studio_palettes": studio, "project_palettes": project, "library": library}


def user_level_directories() -> dict[str, Path]:
    """Return the directories where Qt keeps user-level settings, data and caches."""
    home = Path.home()
    if sys.platform == "darwin":
        library = home / "Library"
        return {
            "preferences": library / "Preferences",
            "support": library / "Application Support",
            "caches": library / "Caches",
        }
    if sys.platform == "win32":
        names = {"roaming": "APPDATA", "local": "LOCALAPPDATA"}
        return {
            label: Path(os.environ[name]) for label, name in names.items() if name in os.environ
        }
    xdg = {"config": (".config", "XDG_CONFIG_HOME"), "data": (".local/share", "XDG_DATA_HOME")}
    xdg["cache"] = (".cache", "XDG_CACHE_HOME")
    return {
        label: Path(os.environ.get(name, home / folder)) for label, (folder, name) in xdg.items()
    }


def _redirected(pid: int) -> str:
    """Return the environment text of a process, or an empty string where it cannot be read."""
    if sys.platform == "linux":
        return Path(f"/proc/{pid}/environ").read_bytes().replace(b"\0", b"\n").decode()
    if sys.platform == "darwin":
        shown = subprocess.run(
            ["ps", "-E", "-ww", "-o", "command=", "-p", str(pid)], capture_output=True, text=True
        )
        return shown.stdout
    return ""


def _worker_pids(worker: Worker) -> list[int]:
    listing = subprocess.run(
        ["ps", "-ax", "-o", "pid=,ppid=,command="], capture_output=True, text=True, check=True
    ).stdout
    rows = [line.split(None, 2) for line in listing.splitlines()]
    rows = [(int(row[0]), int(row[1]), row[2]) for row in rows if len(row) == 3]
    tree = {worker.process.pid}
    grown = True
    while grown:
        added = {pid for pid, parent, _ in rows if parent in tree} - tree
        tree |= added
        grown = bool(added)
    return [pid for pid, _, command in rows if pid in tree and "--automation-worker" in command]


def require_redirect(case: NativeAcceptance, worker: Worker, temporary: Path) -> None:
    """Require the native worker process to have been started with the private temp directory."""
    if os.name != "posix":
        return
    pids = _worker_pids(worker)
    case.assertTrue(pids, "No native worker process was found")
    for pid in pids:
        case.assertIn(f"TMPDIR={temporary}", _redirected(pid))


def _start_worker(case: NativeAcceptance, shared: dict[str, Path], temporary: Path) -> Worker:
    case.worker.close()
    temporary.mkdir()
    variables = {"TMPDIR": str(temporary), "TMP": str(temporary), "TEMP": str(temporary)}
    with mock.patch.dict(os.environ, variables):
        worker = Worker(
            case.executable,
            case.workspace,
            case.inputs,
            case.directory / "readonly.stderr.log",
            shared_roots=shared,
            shared_write=("library",),
        )
    case.addCleanup(worker.close)
    case.worker = worker
    require_redirect(case, worker, temporary)
    return worker


def _expose(case: NativeAcceptance, document: str, level: str, name: str, length: int) -> str:
    column = case.mutate("column.create", document, name=name)["column_id"]
    case.mutate(
        "column.expose",
        document,
        column_id=column,
        level_id=level,
        drawing_id=VECTOR_DRAWING,
        start_frame=0,
        length=length,
    )
    return column


def _raster_levels(case: NativeAcceptance, document: str, ids: dict[str, str]) -> None:
    imported = case.mutate(
        "level.import", document, path=str(case.fixture), name="Quadrants", dpi=120
    )
    ids["rgba_level"] = imported["level_id"]
    ids["rgba_column"] = _expose(case, document, imported["level_id"], "Artwork", 6)
    toonz = case.mutate("level.create", document, name="Paint", format="toonzRaster")
    ids["toonz_level"] = toonz["level_id"]
    case.mutate(
        "drawing.create",
        document,
        level_id=toonz["level_id"],
        drawing_id=VECTOR_DRAWING,
        format="toonzRaster",
        width=RASTER_SIZE,
        height=RASTER_SIZE,
        dpi=120,
    )
    pixels = [{"ink": 1, "paint": 0, "tone": 20}, {"ink": 0, "paint": 1, "tone": 255}]
    case.mutate(
        "raster.patch",
        document,
        level_id=toonz["level_id"],
        drawing_id=VECTOR_DRAWING,
        coordinate_space="raster_pixels_lower_left",
        x=0,
        y=0,
        width=2,
        height=1,
        sharing="all_exposures",
        pixels=pixels,
    )
    ids["toonz_column"] = _expose(case, document, toonz["level_id"], "Paint", 3)


def _animation_and_paths(case: NativeAcceptance, document: str, ids: dict[str, str]) -> None:
    peg = case.mutate("object.create", document, kind="pegbar", name="Peg")["object_id"]
    ids["peg"] = peg
    keys = [{"frame": f, "value": f * 0.1, "interpolation": "linear"} for f in (0, 4, 8)]
    case.mutate("animation.set_keys", document, object_id=peg, channel="x", unit="inch", keys=keys)
    path = case.mutate(
        "motion_path.create",
        document,
        name="Arc",
        unit="inch",
        control_points=[{"x": 0, "y": 0}, {"x": 0.1, "y": 0.1}, {"x": 0.2, "y": 0}],
        coordinate_policy="native_roundtrip",
        max_coordinate_error_inch=1e-4,
    )
    ids["motion_path"] = path["path_id"]
    case.mutate(
        "object.path.attach",
        document,
        object_id=peg,
        path_id=path["path_id"],
        aim=False,
        position_policy="use_path_percentage",
        update_position_keys=False,
    )


def _effects(case: NativeAcceptance, document: str, ids: dict[str, str]) -> None:
    graph = case.success("fx.graph", {"document_id": document})
    hsv = case.mutate("fx.create", document, type="STD_hsvScaleFx", name="Keyed hue")["fx_id"]
    case.mutate(
        "fx.connect", document, fx_id=hsv, port="Source", source_fx_id=graph["xsheet_fx_id"]
    )
    case.mutate("fx.parameter.set_key", document, fx_id=hsv, parameter="hue", frame=2, value=30)
    ids["fx"] = hsv


def _audio_and_panels(case: NativeAcceptance, document: str, ids: dict[str, str]) -> None:
    path = case.inputs / "tone.wav"
    path.write_bytes(wav_file(constant(48000, "pcm_s16", 48000, 300)))
    source = case.mutate("audio.import", document, path=str(path), name="Line")["source"]
    column = case.mutate("audio.column.create", document, name="Line", volume_permille=1000)
    case.mutate(
        "audio.clip.place",
        document,
        column_id=column["column"]["column_id"],
        level_id=source["level_id"],
        placement={"timeline_start_frame": 0, "source_start_frame": 0, "frame_count": 6},
        overlap_policy="reject",
    )
    case.mutate(
        "storyboard.panel.create",
        document,
        panel_frame=0,
        caption="Opening",
        notes="Read-only check",
        color={"r": 10, "g": 64, "b": 200},
    )


def _render_and_analysis(case: NativeAcceptance, document: str, ids: dict[str, str]) -> None:
    job = case.mutate("render.preview", document, frame=0, width=32, height=32)["job_id"]
    case.assertEqual(case.wait_job(job)["status"], "completed")
    ids["job"] = job
    ids["artifact"] = case.success("render.artifacts", {"job_id": job})["artifacts"][0][
        "artifact_id"
    ]
    path = case.inputs / "speech.dat"
    path.write_bytes(dat_file(DAT_CUES))
    imported = case.mutate(
        "lipsync.import",
        None,
        path=str(path),
        format="papagayo_dat",
        shape_set="preston_blair",
        frame_rate=24,
    )
    ids["analysis"] = imported["analysis_id"]


def _vector_style(case: NativeAcceptance, document: str, level: str) -> None:
    red = {"r": 255, "g": 0, "b": 0, "a": 255}
    blue = {"r": 0, "g": 0, "b": 255, "a": 255}
    style = case.mutate("palette.create_style", document, level_id=level, name="Fade", color=red)
    target = {"level_id": level, "style_id": style["style_id"]}
    case.mutate("palette.update_style", document, **target, color=red, keyframe=0)
    case.mutate("palette.update_style", document, **target, color=blue, keyframe=24)


def _no_stage_objects(text: str) -> str:
    return re.sub(
        r'<pegbar [^>]*id="(Camera1|Table|Col\d+)"[^>]*>.*?</pegbar>\s*', "", text, flags=re.S
    )


def _read_only_documents(
    case: NativeAcceptance, worker: Worker, ids: dict[str, str]
) -> dict[str, str]:
    scenes = corpus(case)
    stripped = _no_stage_objects((scenes["base"] / "scene.tnz").read_text())
    scenes["N"] = _variant(case, scenes["base"], "no-stage-objects", stripped)
    project = project_id(case, worker, "ShowA")
    opened = {
        "project": open_scene(case, worker, scenes["A"], project),
        "sheets": open_scene(case, worker, scenes["B"], "standalone"),
        "unsafe": open_scene(case, worker, scenes["C"], "standalone"),
        "no_stage_objects": open_scene(case, worker, scenes["N"], "standalone"),
    }
    documents = {name: result["document_id"] for name, result in opened.items()}
    params = {"document_id": documents["project"], "expected_revision": "0", "frame": 0}
    params.update(width=32, height=32, operation_id=_op(case))
    job = call(case, worker, "render.preview", params)["job_id"]
    case.assertEqual(wait(case, worker, job)["status"], "completed")
    ids["read_only_job"] = job
    artifacts = call(case, worker, "render.artifacts", {"job_id": job})["artifacts"]
    ids["read_only_artifact"] = artifacts[0]["artifact_id"]
    return documents


def build(case: NativeAcceptance) -> Fixture:
    """Create the shared roots, a worker that holds them, and one scene with every kind of content.

    Args:
        case: A fresh acceptance case; its worker is replaced by one with the shared roots.
    Returns:
        The fixture, whose `roots` are every directory the harness hashes.
    """
    shared = _shared_roots(case)
    temporary = case.directory / "temporary"
    worker = _start_worker(case, shared, temporary)
    bare = case.create("Bare")
    document, body, _body_column, arm_column = _arm_rig(case)
    ids = {
        "body_level": body,
        "arm_column": arm_column["column_id"],
        "arm_object": arm_column["object_id"],
    }
    summary = case.success("scene.summary", {"document_id": document})
    ids["arm_level"] = next(item["level_id"] for item in summary["levels"] if item["name"] == "Arm")
    ids["camera"] = summary["camera"]["object_id"]
    _vector_style(case, document, ids["arm_level"])
    _raster_levels(case, document, ids)
    _animation_and_paths(case, document, ids)
    _effects(case, document, ids)
    _audio_and_panels(case, document, ids)
    _render_and_analysis(case, document, ids)
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "saved"))
    package = Path(saved["package_directory"])
    project = case.inputs / "Shots" / "Shot A"
    project.mkdir(parents=True)
    (project / "Shot A_otprj.xml").write_text("<project/>\n")
    copied = case.inputs / "saved-package"
    shutil.copytree(package, copied)
    reopened = case.mutate("scene.open", None, path=str(package))["document_id"]
    read_only = _read_only_documents(case, worker, ids)
    roots: dict[str, Any] = {"workspace": case.workspace, "inputs": case.inputs, **shared}
    roots["temporary"] = temporary
    user = user_level_directories()
    fixture = Fixture(case, worker, roots, document, reopened, bare, ids, package, copied)
    fixture.read_only = read_only
    fixture.user_directories = user
    return fixture
