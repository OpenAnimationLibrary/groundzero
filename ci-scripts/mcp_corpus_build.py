"""Generate the old-scene corpus and, only on request and with a reason, its baselines.

    mcp_corpus_build.py scenes --executable APP
    mcp_corpus_build.py baselines --tcomposer PATH --reason "why the baselines change"

`scenes` authors each corpus scene with the worker's tools (some then edited as text, to give a
scene the shapes a tool cannot make: a sub-xsheet, repeated level names), saves it as a package
and writes it as a project under `fixtures/corpus/<name>/`. It records the files and their
hashes in `manifest.json`. It does not touch the baselines, so the check that compares the two
fails until the baselines are regenerated on purpose.

`baselines` renders every scene with the batch renderer `tcomposer` and replaces the baseline
frames. It refuses without a reason of at least 20 characters, refuses when the `CI` environment
variable is set, and appends the reason, the renderer's hash and the old and new hash of every
baseline to the manifest's `history`. Nothing else in the repository writes a baseline.

The scenes come from this script and the baselines from the same build's renderer: no artist's
scene and no earlier build exists to use. The manifest records that.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from mcp_acceptance import NativeAcceptance
from mcp_corpus_compare import (
    CORPUS,
    MINIMUM_REASON,
    baseline_files,
    baseline_set,
    digest,
    dump_manifest,
    load_manifest,
    scene_files,
    sha256_file,
)
from mcp_native_oracle import find_tcomposer, render_with_tcomposer
from mcp_scene_access_cases import CHILD_ID, _cells, _child_level, base_package

FIRST, LAST, SIZE = 0, 5, 32
PROJECT_FOLDERS = ("inputs", "drawings", "scenes", "extras", "outputs")


class Author(NativeAcceptance):
    """A worker and the helpers of the acceptance suite, outside a test run."""

    def runTest(self) -> None:  # noqa: N802 - the name unittest calls
        """Nothing runs; the builder uses the helpers."""


def project_text(name: str) -> str:
    """The project file as the native editor writes one."""
    folders = "".join(
        f'    <folder name="{folder}" path="{folder}"/>\n' for folder in PROJECT_FOLDERS
    )
    return (
        "<project>\n  <version>\n    70 1 \n  </version>\n  <folders>\n"
        + folders
        + "  </folders>\n"
        "</project>\n"
    )


def to_project(package: Path, destination: Path, name: str) -> None:
    """Write a saved package as a project whose drawings folder holds its levels."""
    folder = destination / name
    (folder / "scenes").mkdir(parents=True)
    shutil.copytree(package / "assets", folder / "drawings")
    (folder / f"{name}_otprj.xml").write_text(project_text(name), encoding="utf-8")
    (folder / "scenes" / "scenes.xml").write_text(
        '<parentProject type="projectFolder">\n  ".."\n</parentProject>\n', encoding="utf-8"
    )
    text = (package / "scene.tnz").read_text(encoding="utf-8")
    (folder / "scenes" / "shot.tnz").write_text(
        text.replace("$scenefolder/assets/", "+drawings/"), encoding="utf-8"
    )


def _copy(package: Path, scratch: Path, name: str) -> Path:
    target = scratch / name
    shutil.copytree(package, target)
    return target


def mixed_levels(author: Author, scratch: Path) -> Path:
    """Vector, Toonz raster and PNG levels with a blur on the PNG, one column each."""
    return base_package(author)


def sub_xsheet(author: Author, scratch: Path) -> Path:
    """The base scene whose last two vector columns show one sub-xsheet."""
    package = _copy(base_package(author), scratch, "sub")
    text = (package / "scene.tnz").read_text(encoding="utf-8")
    child = f"<childLevel id='{CHILD_ID}'/>1 1"
    text = _cells(text, {3: child, 4: child})
    anchor = "</levels>\n    <folder"
    text = text.replace(anchor, _child_level(1) + anchor, 1)
    (package / "scene.tnz").write_text(text, encoding="utf-8")
    return package


def repeated_names(author: Author, scratch: Path) -> Path:
    """The base scene with three more levels that all carry one name, the first exposed."""
    package = _copy(base_package(author), scratch, "names")
    text = (package / "scene.tnz").read_text(encoding="utf-8")
    levels = "".join(
        f'<level id=\'{2400 + index}\'>\nZed \n<info dpix="120.000000" dpiy="120.000000"/>\n'
        '<path> "$scenefolder/assets/level-0.pli" </path>\n</level>\n'
        for index in range(3)
    )
    text = _cells(text, {0: "<level id='2400'/>0001 0"})
    anchor = "</levels>\n    <folder"
    (package / "scene.tnz").write_text(text.replace(anchor, levels + anchor, 1), encoding="utf-8")
    return package


def _stroke(offset: float, thickness: float = 2) -> dict[str, Any]:
    points = [(-12, -8 + offset), (0, 8 + offset), (12, -8 + offset)]
    return {
        "action": "add",
        "style_id": 1,
        "closed": False,
        "points": [{"x": x, "y": y, "thickness": thickness} for x, y in points],
    }


def holds_blanks(author: Author, scratch: Path) -> Path:
    """Vector drawings 1, 1a and 2 on twos with a blank and a one-frame hold."""
    document = author.create("Holds")
    level = author.mutate("level.create", document, name="Ink", format="vector")["level_id"]
    column = author.mutate("column.create", document, name="Rows")["column_id"]
    layout = [(0, 2, 1, ""), (2, 2, 1, "a"), (5, 1, 2, "")]
    for index, (_first, _held, number, suffix) in enumerate(layout):
        drawing = {"number": number, "suffix": suffix}
        edits = {"coordinate_space": "drawing_native", "sharing": "all_exposures"}
        author.mutate(
            "drawing.create", document, level_id=level, drawing_id=drawing, format="vector"
        )
        author.mutate(
            "vector.edit",
            document,
            level_id=level,
            drawing_id=drawing,
            **edits,
            edits=[_stroke(index * 5 - 5)],
        )
    for first, held, number, suffix in layout:
        author.mutate(
            "column.expose",
            document,
            column_id=column,
            level_id=level,
            drawing_id={"number": number, "suffix": suffix},
            start_frame=first,
            length=held,
        )
    return _save(author, document, "holds")


def png_pan(author: Author, scratch: Path) -> Path:
    """A PNG level on a peg that pans and turns, under a camera that moves."""
    document, level, column = author.populated_scene()
    author.mutate(
        "column.expose",
        document,
        column_id=column["column_id"],
        level_id=level["level_id"],
        drawing_id={"number": 1, "suffix": ""},
        start_frame=0,
        length=6,
        overwrite=True,
    )
    peg = author.mutate("object.create", document, name="Pan", kind="pegbar")["object_id"]
    author.mutate(
        "object.parent",
        document,
        object_id=column["object_id"],
        parent_id=peg,
        placement_policy="preserve_local",
    )
    keys = [
        {"frame": 0, "value": 0, "interpolation": "linear"},
        {"frame": 5, "value": 5 / 120, "interpolation": "linear"},
    ]
    author.mutate(
        "animation.set_keys", document, object_id=peg, channel="x", unit="inch", keys=keys
    )
    turn = [{"frame": 0, "value": 0, "interpolation": "linear"}]
    turn.append({"frame": 5, "value": 10, "interpolation": "linear"})
    author.mutate(
        "animation.set_keys", document, object_id=peg, channel="rotation", unit="degree", keys=turn
    )
    return _save(author, document, "pan")


def _save(author: Author, document: str, name: str) -> Path:
    saved = author.mutate("scene.save_copy", document, directory=str(author.workspace / name))
    return Path(saved["package_directory"])


SCENES: dict[str, tuple[str, Callable[[Author, Path], Path], dict[str, int]]] = {
    "mixed_levels": (
        "Vector, Toonz raster and PNG levels, a blur on the PNG column",
        mixed_levels,
        {"levels": 3},
    ),
    "sub_xsheet": (
        "Two columns that show one sub-xsheet (its child level counts as a level)",
        sub_xsheet,
        {"levels": 4},
    ),
    "repeated_names": (
        "Three levels that share a name, the first exposed in a column",
        repeated_names,
        {"levels": 6},
    ),
    "holds_blanks": (
        "Vector drawings 1, 1a and 2 on twos, a blank row and a one-frame hold",
        holds_blanks,
        {"levels": 1},
    ),
    "png_pan": (
        "A PNG level on a peg that pans and turns",
        png_pan,
        {"levels": 1},
    ),
}


def _empty_scene_entry(description: str, expected: dict[str, int]) -> dict[str, Any]:
    return {
        "description": description,
        "expected": expected,
        "files": {},
        "scene_sha256": "",
        "policy": {"color": 0, "alpha": 0, "pixels": 0.0, "why": ""},
        "baseline": {"files": {}, "rendered_from": ""},
    }


def build_scenes(executable: Path, root: Path = CORPUS) -> None:
    """Author every scene and record its files; baselines are left as they are."""
    manifest = (
        load_manifest(root / "manifest.json")
        if (root / "manifest.json").is_file()
        else {
            "schema_version": 1,
            "provenance": {
                "kind": "generated_by_script",
                "note": (
                    "Generated by ci-scripts/mcp_corpus_build.py. No artist-made scene and no "
                    "earlier build exists to draw on; baselines come from the batch renderer of "
                    "the same build as the candidate. A regression net, not evidence about an "
                    "earlier release."
                ),
                "human_review": "none",
            },
            "frames": {"first": FIRST, "last": LAST, "width": SIZE, "height": SIZE},
            "scenes": {},
            "history": [],
        }
    )
    os.environ.setdefault("QT_QPA_PLATFORM", "cocoa")
    Author.executable = executable.resolve(strict=True)
    scratch = Path(tempfile.mkdtemp(prefix="corpus-")).resolve()
    Author.output = scratch
    for name, (description, author_scene, expected) in SCENES.items():
        author = Author()
        author.setUp()
        try:
            package = author_scene(author, scratch)
            if (root / name).exists():
                shutil.rmtree(root / name)
            to_project(package, root, name)
        finally:
            author.doCleanups()
        entry = manifest["scenes"].setdefault(name, _empty_scene_entry(description, expected))
        entry["description"], entry["expected"] = description, expected
        entry["files"] = scene_files(root, name)
        entry["scene_sha256"] = digest(entry["files"])
    manifest["generator_scenes"] = {
        "tool": "mcp_corpus_build.py scenes",
        "executable_sha256": sha256_file(executable),
        "when": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    (root / "manifest.json").write_text(dump_manifest(manifest), encoding="utf-8")
    shutil.rmtree(scratch, ignore_errors=True)


def refuse_unless_deliberate(reason: str) -> None:
    """Stop a regeneration that nobody asked for or explained."""
    if os.environ.get("CI"):
        raise SystemExit("baselines are never regenerated in CI; run this by hand")
    if len(reason.strip()) < MINIMUM_REASON:
        raise SystemExit(
            f"give a --reason of at least {MINIMUM_REASON} characters: what changed and why the "
            "new frames are the right ones"
        )


def _render(tcomposer: Path, root: Path, name: str, scratch: Path) -> list[Path]:
    project = scratch / name
    shutil.copytree(root / name, project)
    output = scratch / f"{name}-frames"
    output.mkdir()
    return render_with_tcomposer(
        tcomposer,
        project / "scenes" / "shot.tnz",
        output,
        range(FIRST, LAST + 1),
        stuff=scratch / "stuff",
    )


def build_baselines(tcomposer: Path, reason: str, root: Path = CORPUS) -> None:
    """Replace every baseline with the batch renderer's frames and record why."""
    refuse_unless_deliberate(reason)
    manifest = load_manifest(root / "manifest.json")
    scratch = Path(tempfile.mkdtemp(prefix="corpus-baselines-"))
    changes = []
    for name, scene in manifest["scenes"].items():
        frames = _render(tcomposer, root, name, scratch)
        folder = root / "baselines" / name
        before = baseline_files(root, name)
        if folder.exists():
            shutil.rmtree(folder)
        folder.mkdir(parents=True)
        for frame in frames:
            shutil.copyfile(frame, folder / frame.name)
        after = baseline_files(root, name)
        scene["baseline"] = {"files": after, "rendered_from": scene["scene_sha256"]}
        changes.append({"scene": name, "before": before, "after": after})
    manifest["generator_baselines"] = {
        "tool": "tcomposer",
        "binary_sha256": sha256_file(tcomposer),
        "same_build_as_candidate": True,
        "arguments": "-nthreads 1 -range 1 6 (window-system platform, one thread)",
    }
    manifest["history"].append(
        {
            "when": time.strftime("%Y-%m-%d %H:%M:%S"),
            "reason": reason.strip(),
            "generator_sha256": manifest["generator_baselines"]["binary_sha256"],
            "baseline_set_sha256": baseline_set(manifest),
            "changes": changes,
        }
    )
    (root / "manifest.json").write_text(dump_manifest(manifest), encoding="utf-8")
    shutil.rmtree(scratch, ignore_errors=True)


def main() -> int:
    """Run `scenes` or `baselines`."""
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    scenes = commands.add_parser("scenes")
    scenes.add_argument("--executable", type=Path, required=True)
    baselines = commands.add_parser("baselines")
    baselines.add_argument("--tcomposer", type=Path)
    baselines.add_argument("--executable", type=Path)
    baselines.add_argument("--reason", default="")
    arguments = parser.parse_args()
    if arguments.command == "scenes":
        build_scenes(arguments.executable)
        return 0
    tcomposer = arguments.tcomposer or (
        find_tcomposer(arguments.executable.parent) if arguments.executable else None
    )
    if tcomposer is None:
        raise SystemExit(
            "no batch renderer: build the tcomposer target of this build "
            "(cmake --build <build> --target tcomposer) and pass --tcomposer"
        )
    build_baselines(tcomposer, arguments.reason)
    return 0


if __name__ == "__main__":
    sys.exit(main())
