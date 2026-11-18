"""Every tool that names a file or directory, and how to drive each one at the file boundary.

The catalog decides which tools belong here: a tool with a parameter that names a path must have
an entry in PATH_TOOLS, and every entry must name a tool that still exists.
"""

from __future__ import annotations

import re
import shutil
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_audio_model import tone, wav_file
from mcp_library_cases import _package_scene
from mcp_lipsync_cases import Cue, tsv_file
from mcp_png import write_fixture
from mcp_scene_access_cases import project_xml

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

PATH_PARAMETER = re.compile(r"(?:^|_)(?:path|directory|dir|folder|root|file)$")
SHARED_TOOLSET = "shared-library"
State = dict[str, Any]


@dataclass(frozen=True)
class PathTool:
    """One catalog tool and the means to call it with any path text.

    Attributes:
        name: The catalog tool name.
        parameters: The catalog parameters that name a path, as found by the catalog check.
        mode: read (existing input), write (new output) or published (an object this worker made).
        suffix: Extension a valid path must carry.
        missing: Codes the tool answers for a permitted path that holds nothing usable.
        prepare: Builds the documents the call needs and returns them as state.
        drive: Calls the tool with the given path text and returns the raw response.
        place: Puts a valid object at a path: the input, the colliding output or the publication.
    """

    name: str
    parameters: tuple[str, ...]
    mode: str
    suffix: str
    missing: frozenset[str]
    prepare: Callable[[NativeAcceptance], State]
    drive: Callable[[NativeAcceptance, State, str], dict[str, Any]]
    place: Callable[[NativeAcceptance, State, Path], None]
    pinned: bool = True


def invoke(
    case: NativeAcceptance, method: str, document: str | None, **params: Any
) -> dict[str, Any]:
    """Call a mutating tool with a fresh receipt and track the revision of a success."""
    response = case.worker.call(method, case.write_params(document, params))
    result = response.get("result")
    if document is not None and isinstance(result, dict) and "revision" in result:
        case.revisions[document] = result["revision"]
    return response


def _name(case: NativeAcceptance, prefix: str) -> str:
    return f"{prefix}{case.operations + 1}"


def _png(_case: NativeAcceptance, _state: State, path: Path) -> None:
    write_fixture(path)


def _wav(_case: NativeAcceptance, _state: State, path: Path) -> None:
    path.write_bytes(wav_file(tone(48000, "pcm_s16", 24000)))


def _timing(_case: NativeAcceptance, _state: State, path: Path) -> None:
    path.write_bytes(tsv_file([Cue(0, "A"), Cue(500_000, "B")]))


def _file(_case: NativeAcceptance, _state: State, path: Path) -> None:
    path.write_bytes(b"existing destination\n")


def _directory(_case: NativeAcceptance, _state: State, path: Path) -> None:
    path.mkdir()
    (path / "existing.txt").write_bytes(b"existing destination\n")


def _project_folder(_case: NativeAcceptance, _state: State, path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    (path / "tahomaproject.xml").write_text(project_xml({}))


def _list_projects(case: NativeAcceptance, _state: State, path: str) -> dict[str, Any]:
    return case.worker.call("project.list", {"folder": path})


def _packaged(case: NativeAcceptance) -> State:
    existing = case.workspace / "rig-package"
    return {"package": existing if existing.is_dir() else _package_scene(case)}


def _scene_file(_case: NativeAcceptance, state: State, path: Path) -> None:
    package: Path = state["package"]
    shutil.copyfile(package / "scene.tnz", path)
    if not (path.parent / "assets").exists():
        shutil.copytree(package / "assets", path.parent / "assets")


def _open_read_only(case: NativeAcceptance, _state: State, path: str) -> dict[str, Any]:
    response = invoke(case, "scene.open_read_only", None, path=path, project_id="standalone")
    result = response.get("result")
    if isinstance(result, dict):
        case.revisions[result["document_id"]] = result["revision"]
        invoke(case, "scene.discard", result["document_id"])
    return response


def _document(case: NativeAcceptance) -> State:
    return {"document": case.create("Boundary")}


def _populated(case: NativeAcceptance) -> State:
    document, level, _column = case.populated_scene()
    return {"document": document, "level": level["level_id"], "drawing": 1}


def _drawing(state: State) -> dict[str, Any]:
    state["drawing"] += 1
    return {"number": state["drawing"], "suffix": ""}


def _level_import(case: NativeAcceptance, state: State, path: str) -> dict[str, Any]:
    name = _name(case, "Level")
    return invoke(case, "level.import", state["document"], path=path, name=name, dpi=120)


def _png_import(case: NativeAcceptance, state: State, path: str) -> dict[str, Any]:
    return invoke(
        case,
        "raster.import_png",
        state["document"],
        level_id=state["level"],
        drawing_id=_drawing(state),
        path=path,
        dpi=120,
    )


def _png_export(case: NativeAcceptance, state: State, path: str) -> dict[str, Any]:
    drawing = {"number": 1, "suffix": ""}
    return invoke(
        case,
        "raster.export_png",
        state["document"],
        level_id=state["level"],
        drawing_id=drawing,
        path=path,
    )


def _audio_import(case: NativeAcceptance, state: State, path: str) -> dict[str, Any]:
    return invoke(case, "audio.import", state["document"], path=path, name=_name(case, "Sound"))


def _lipsync_import(case: NativeAcceptance, _state: State, path: str) -> dict[str, Any]:
    return invoke(
        case,
        "lipsync.import",
        None,
        path=path,
        format="rhubarb_tsv",
        shape_set="rhubarb_basic",
    )


def _mixable(case: NativeAcceptance) -> State:
    document = case.create("Boundary")
    source = case.inputs / "boundary-tone.wav"
    source.write_bytes(wav_file(tone(48000, "pcm_s16", 48000)))
    level = case.mutate("audio.import", document, path=str(source), name="Tone")["source"]
    column = case.mutate("audio.column.create", document, name="Sound", volume_permille=1000)
    placement = {"timeline_start_frame": 0, "source_start_frame": 0, "frame_count": 12}
    case.mutate(
        "audio.clip.place",
        document,
        column_id=column["column"]["column_id"],
        level_id=level["level_id"],
        placement=placement,
        overlap_policy="reject",
    )
    return {"document": document}


def _mixdown(case: NativeAcceptance, state: State, path: str) -> dict[str, Any]:
    return invoke(
        case,
        "audio.mixdown",
        state["document"],
        start_frame=0,
        end_frame=6,
        output_path=path,
        format_policy="native_mix",
    )


def _save_copy(case: NativeAcceptance, state: State, path: str) -> dict[str, Any]:
    return invoke(case, "scene.save_copy", state["document"], directory=path)


def _with_effect(case: NativeAcceptance) -> State:
    state = _document(case)
    effect = case.mutate("fx.create", state["document"], type="STD_blurFx")
    state["fx"] = effect["fx_id"]
    return state


def _preset_save(case: NativeAcceptance, state: State, path: str) -> dict[str, Any]:
    return invoke(case, "fx.preset.save", state["document"], fx_id=state["fx"], directory=path)


def _preset_apply(case: NativeAcceptance, state: State, path: str) -> dict[str, Any]:
    return invoke(case, "fx.preset.apply", state["document"], fx_id=state["fx"], directory=path)


def _publish_package(case: NativeAcceptance, state: State, path: Path) -> None:
    case.mutate("scene.save_copy", state["document"], directory=str(path))


def _publish_preset(case: NativeAcceptance, state: State, path: Path) -> None:
    case.mutate("fx.preset.save", state["document"], fx_id=state["fx"], directory=str(path))


def _open_package(case: NativeAcceptance, _state: State, path: str) -> dict[str, Any]:
    return invoke(case, "scene.open", None, path=path)


def _package_info(case: NativeAcceptance, _state: State, path: str) -> dict[str, Any]:
    return case.worker.call("scene.package_info", {"path": path})


READ_ONLY_REFUSED = frozenset({"not_found", "invalid_path", "invalid_scene", "unsupported_type"})
INPUT_MISSING = frozenset({"invalid_asset", "io_error", "asset_unavailable", "invalid_params"})
OUTPUT_FAILED = frozenset(
    {"io_error", "missing_parent", "package_write_failed", "PRESET_WRITE_FAILED"}
)
UNREGISTERED = frozenset(
    {"unregistered_package", "UNREGISTERED_PRESET", "INVALID_PRESET", "invalid_package"}
)

PATH_TOOLS: dict[str, PathTool] = {
    tool.name: tool
    for tool in (
        PathTool(
            "level.import", ("path",), "read", ".png", INPUT_MISSING, _document, _level_import, _png
        ),
        PathTool(
            "raster.import_png",
            ("path",),
            "read",
            ".png",
            INPUT_MISSING,
            _populated,
            _png_import,
            _png,
        ),
        PathTool(
            "audio.import", ("path",), "read", ".wav", INPUT_MISSING, _document, _audio_import, _wav
        ),
        PathTool(
            "lipsync.import",
            ("path",),
            "read",
            ".tsv",
            INPUT_MISSING,
            _document,
            _lipsync_import,
            _timing,
        ),
        PathTool(
            "project.list",
            ("folder",),
            "read",
            "",
            READ_ONLY_REFUSED,
            _document,
            _list_projects,
            _project_folder,
            pinned=False,
        ),
        PathTool(
            "scene.open_read_only",
            ("path",),
            "read",
            ".tnz",
            READ_ONLY_REFUSED,
            _packaged,
            _open_read_only,
            _scene_file,
            pinned=False,
        ),
        PathTool(
            "raster.export_png",
            ("path",),
            "write",
            ".png",
            OUTPUT_FAILED,
            _populated,
            _png_export,
            _file,
        ),
        PathTool(
            "audio.mixdown",
            ("output_path",),
            "write",
            ".wav",
            OUTPUT_FAILED,
            _mixable,
            _mixdown,
            _file,
        ),
        PathTool(
            "scene.save_copy",
            ("directory",),
            "write",
            "",
            OUTPUT_FAILED,
            _document,
            _save_copy,
            _directory,
        ),
        PathTool(
            "fx.preset.save",
            ("directory",),
            "write",
            "",
            OUTPUT_FAILED,
            _with_effect,
            _preset_save,
            _directory,
        ),
        PathTool(
            "scene.open",
            ("path",),
            "published",
            "",
            UNREGISTERED,
            _document,
            _open_package,
            _publish_package,
        ),
        PathTool(
            "scene.package_info",
            ("path",),
            "published",
            "",
            UNREGISTERED,
            _document,
            _package_info,
            _publish_package,
        ),
        PathTool(
            "fx.preset.apply",
            ("directory",),
            "published",
            "",
            UNREGISTERED,
            _with_effect,
            _preset_apply,
            _publish_preset,
        ),
    )
}

SHARED_TOOLS = {
    "shared.roots": (),
    "studio_palette.list": ("root", "folder"),
    "studio_palette.inspect": ("root", "path"),
    "studio_palette.find": (),
    "library.list": (),
    "library.inspect": (),
    "library.thumbnail": (),
}
HANDLE_TOOLS = {"artifact.read": ("artifact_id",)}


def path_parameters(schema: Any, prefix: str = "") -> list[str]:
    """Return the dotted names of every property in a schema that names a file system path."""
    found: list[str] = []
    if not isinstance(schema, dict):
        return found
    for key, value in schema.get("properties", {}).items():
        if PATH_PARAMETER.search(key) and isinstance(value, dict) and value.get("type") == "string":
            found.append(prefix + key)
        found.extend(path_parameters(value, f"{prefix}{key}."))
    found.extend(path_parameters(schema.get("items"), f"{prefix}[]."))
    return found


def unqualified_tools(tools: list[dict[str, Any]]) -> list[str]:
    """Return what is wrong between the catalog and the qualification table, as messages."""
    problems: list[str] = []
    names = {tool["name"] for tool in tools}
    for tool in tools:
        name = tool["name"]
        found = tuple(path_parameters(tool["inputSchema"]))
        covered = PATH_TOOLS[name].parameters if name in PATH_TOOLS else None
        if tool.get("toolset") == SHARED_TOOLSET:
            if name not in SHARED_TOOLS:
                problems.append(
                    f"{name} is a new shared-library tool: add it to SHARED_TOOLS in "
                    "ci-scripts/mcp_file_tools.py and give it cases in mcp_file_shared_cases.py"
                )
        elif found and covered != found and name not in SHARED_TOOLS:
            problems.append(
                f"{name} takes path parameters {list(found)}: add or fix its PathTool entry in "
                "ci-scripts/mcp_file_tools.py (mode, suffix, prepare, drive and place) so the "
                "boundary matrix drives it"
            )
    for name in sorted(set(PATH_TOOLS) | set(SHARED_TOOLS) | set(HANDLE_TOOLS)):
        if name not in names:
            problems.append(f"{name} is qualified in mcp_file_tools.py but absent from the catalog")
    return problems
