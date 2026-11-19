"""Generate the old-scene corpus and, only on request and with a reason, its baselines.

    mcp_corpus_build.py scenes --executable APP
    mcp_corpus_build.py baselines --tcomposer PATH --reason "why the baselines change"

Both commands take `--only NAME ...` to touch just those scenes. `declare` rewrites the effects
and probes of every scene in the manifest from this file, without touching scenes or baselines.

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
    scene_effects,
    scene_files,
    sha256_file,
)
from mcp_native_oracle import find_tcomposer, render_with_tcomposer
from mcp_scene_access_cases import CHILD_ID, DRAWING, _cells, _child_level, base_package

FIRST, LAST, SIZE = 0, 5, 32
FRAMES = LAST - FIRST + 1
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


def _column_fx(author: Author, document: str, column: str) -> str:
    """The effect node of a column, which is what its drawings feed."""
    graph = author.success("fx.graph", {"document_id": document})
    return next(node["fx_id"] for node in graph["effects"] if node.get("column_id") == column)


def _effect(
    author: Author, document: str, kind: str, sources: dict[str, str], **parameters: Any
) -> str:
    """Create an effect, connect each named port to its source, set parameters, make it the end.

    A source that was the end of its chain stops being one: the effect carries it on.
    """
    created = author.mutate("fx.create", document, type=kind, frame_count=FRAMES)
    for port, source in sources.items():
        author.mutate(
            "fx.connect", document, fx_id=created["fx_id"], port=port, source_fx_id=source
        )
        author.mutate("fx.terminal.set", document, fx_id=source, enabled=False)
    for name, value in parameters.items():
        author.mutate(
            "fx.parameter.set", document, fx_id=created["fx_id"], parameter=name, value=value
        )
    author.mutate("fx.terminal.set", document, fx_id=created["fx_id"], enabled=True)
    return created["fx_id"]


def _key(
    author: Author, document: str, effect: str, parameter: str, values: dict[int, Any]
) -> None:
    for frame, value in values.items():
        author.mutate(
            "fx.parameter.set_key",
            document,
            fx_id=effect,
            parameter=parameter,
            frame=frame,
            value=value,
        )


def _style(author: Author, document: str, level: str, name: str, rgb: tuple[int, int, int]) -> int:
    color = {"r": rgb[0], "g": rgb[1], "b": rgb[2], "a": 255}
    created = author.mutate(
        "palette.create_style", document, level_id=level, name=name, color=color
    )
    return created["style_id"]


def _bar(
    author: Author,
    document: str,
    name: str,
    points: list[tuple[float, float]],
    rgb: tuple[int, int, int] | None = None,
    thickness: float = 1.2,
) -> tuple[str, str]:
    """A vector level with one thick stroke, exposed in its own column; returns (column, level)."""
    level = author.mutate("level.create", document, name=name, format="vector")["level_id"]
    author.mutate("drawing.create", document, level_id=level, drawing_id=DRAWING, format="vector")
    style = _style(author, document, level, name, rgb) if rgb else 1
    stroke = {
        "action": "add",
        "style_id": style,
        "closed": False,
        "points": [{"x": x, "y": y, "thickness": thickness} for x, y in points],
    }
    edits = {"coordinate_space": "drawing_native", "sharing": "all_exposures"}
    author.mutate(
        "vector.edit", document, level_id=level, drawing_id=DRAWING, **edits, edits=[stroke]
    )
    return _expose(author, document, level, name), level


def _expose(author: Author, document: str, level: str, name: str) -> str:
    column = author.mutate("column.create", document, name=name)["column_id"]
    author.mutate(
        "column.expose",
        document,
        column_id=column,
        level_id=level,
        drawing_id=DRAWING,
        start_frame=FIRST,
        length=LAST - FIRST + 1,
    )
    return column


def _block(
    author: Author,
    document: str,
    name: str,
    box: tuple[int, int, int, int],
    paint_rgb: tuple[int, int, int],
    ink_rgb: tuple[int, int, int],
) -> str:
    """A Toonz raster level, camera sized, with a painted block that has an ink border."""
    level = author.mutate("level.create", document, name=name, format="toonzRaster")["level_id"]
    author.mutate(
        "drawing.create",
        document,
        level_id=level,
        drawing_id=DRAWING,
        format="toonzRaster",
        width=SIZE,
        height=SIZE,
        dpi=120,
    )
    paint = _style(author, document, level, name + " paint", paint_rgb)
    ink = _style(author, document, level, name + " ink", ink_rgb)
    left, bottom, right, top = box
    pixels = []
    for y in range(SIZE):
        for x in range(SIZE):
            inside = left <= x < right and bottom <= y < top
            edge = inside and (x in (left, right - 1) or y in (bottom, top - 1))
            pixels.append(
                {"ink": ink, "paint": paint, "tone": 0}
                if edge
                else {"ink": 0, "paint": paint if inside else 0, "tone": 255}
            )
    author.mutate(
        "raster.patch",
        document,
        level_id=level,
        drawing_id=DRAWING,
        coordinate_space="raster_pixels_lower_left",
        x=0,
        y=0,
        width=SIZE,
        height=SIZE,
        sharing="all_exposures",
        pixels=pixels,
    )
    return level


def _card(author: Author, document: str, rgb: tuple[int, int, int]) -> str:
    """A color card generator that fills the camera for every frame; returns its effect."""
    created = author.mutate("fx.create", document, type="colorCardFx", frame_count=FRAMES)
    color = [*rgb, 255]
    author.mutate(
        "fx.parameter.set", document, fx_id=created["fx_id"], parameter="color", value=color
    )
    author.mutate("fx.terminal.set", document, fx_id=created["fx_id"], enabled=False)
    return created["fx_id"]


def vector_effects(author: Author, scratch: Path) -> Path:
    """Two thick vector strokes side by side: one blurred more each frame, one darkened."""
    document = author.create("Vector effects")
    left, _ = _bar(author, document, "Ink", [(-6, -4), (-4.5, 0), (-3, 4)])
    right, _ = _bar(author, document, "Red", [(3, 4), (4.5, 0), (6, -4)], (200, 40, 30))
    blur = _effect(author, document, "STD_blurFx", {"Source": _column_fx(author, document, left)})
    _key(author, document, blur, "value", {0: 0, 5: 3})
    source = {"Source": _column_fx(author, document, right)}
    _effect(author, document, "STD_hsvScaleFx", source, value_scale=50)
    return _save(author, document, "vfx")


def raster_effects(author: Author, scratch: Path) -> Path:
    """A painted Toonz raster block turned in hue across the frames, then raised in gamma."""
    document = author.create("Raster effects")
    level = _block(author, document, "Cel", (6, 8, 22, 24), (200, 60, 40), (20, 30, 120))
    column = _expose(author, document, level, "Cel")
    source = {"Source": _column_fx(author, document, column)}
    hue = _effect(author, document, "STD_hsvScaleFx", source)
    _key(author, document, hue, "hue", {0: 0, 5: 120})
    _effect(author, document, "STD_gammaFx", {"Source": hue}, value=1.6)
    return _save(author, document, "rfx")


def layered_effects(author: Author, scratch: Path) -> Path:
    """A vector and a Toonz raster column: the vector's blur bypassed, the raster posterized."""
    document = author.create("Layered effects")
    level = _block(author, document, "Cel", (2, 2, 14, 14), (60, 170, 90), (10, 50, 30))
    cel = _expose(author, document, level, "Cel")
    ink, _ = _bar(author, document, "Wave", [(-6, 3), (0, -3), (6, 3)], (240, 200, 40), 1)
    bypassed = _effect(
        author, document, "STD_blurFx", {"Source": _column_fx(author, document, ink)}, value=2
    )
    author.mutate("fx.enabled.set", document, fx_id=bypassed, enabled=False)
    source = {"Source": _column_fx(author, document, cel)}
    _effect(author, document, "STD_posterizeFx", source, levels=3)
    return _save(author, document, "lfx")


def keyed_effects(author: Author, scratch: Path) -> Path:
    """A vector stroke whose brightness and contrast are keyed across the six frames."""
    document = author.create("Keyed effects")
    column, _ = _bar(author, document, "Arc", [(-6, -3), (0, 4), (6, -3)], (120, 120, 200), 1.2)
    source = {"Source": _column_fx(author, document, column)}
    tone = _effect(author, document, "STD_brightContFx", source)
    _key(author, document, tone, "brightness", {0: -40, 5: 40})
    _key(author, document, tone, "contrast", {0: 0, 5: 60})
    return _save(author, document, "kfx")


def builtin_effects(author: Author, scratch: Path) -> Path:
    """Effects that are not named STD_: a card, an over and an invert around a vector stroke."""
    document = author.create("Built-in effects")
    level = _block(author, document, "Cel", (4, 4, 14, 14), (60, 170, 90), (10, 50, 30))
    cel = _expose(author, document, level, "Cel")
    bar, _ = _bar(author, document, "Red", [(0, 5), (2, 0), (5, -5)], (200, 40, 30), 1.2)
    inverted = _effect(author, document, "invertFx", {"source": _column_fx(author, document, bar)})
    both = _effect(
        author,
        document,
        "overFx",
        {"Source1": inverted, "Source2": _column_fx(author, document, cel)},
    )
    card = _card(author, document, (24, 36, 70))
    _effect(author, document, "overFx", {"Source1": both, "Source2": card})
    return _save(author, document, "bfx")


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
    "vector_effects": (
        "Two vector strokes: one blurred more each frame, one darkened by a value scale",
        vector_effects,
        {"levels": 2},
    ),
    "raster_effects": (
        "A painted Toonz raster block turned in hue across the frames and raised in gamma",
        raster_effects,
        {"levels": 1},
    ),
    "layered_effects": (
        "A vector column with its blur bypassed beside a Toonz raster column that is posterized",
        layered_effects,
        {"levels": 2},
    ),
    "keyed_effects": (
        "A vector stroke whose brightness and contrast are keyed across the frames",
        keyed_effects,
        {"levels": 1},
    ),
    "builtin_effects": (
        "A card, an over and an invert (effects not named STD_) around a vector and a raster",
        builtin_effects,
        {"levels": 2},
    ),
}


def _probe(kind: str, what: str, x: int, y: int, **values: Any) -> dict[str, Any]:
    return {"kind": kind, "what": what, "x": x, "y": y, **values}


# What each scene's construction says its baselines must show. The values follow from how the
# scene is built (the style colors, the effect's arithmetic), and `mcp_corpus_compare.py` checks
# them against the baselines, so a baseline that hides a column or loses an effect is caught.
PROBES: dict[str, list[dict[str, Any]]] = {
    "png_pan": [
        _probe("png", "top left of the PNG art is red", 2, 2, frame=0, rgba=[255, 0, 0, 255]),
        _probe(
            "png", "bottom right of the PNG art is yellow", 30, 30, frame=0, rgba=[255, 255, 0, 255]
        ),
    ],
    "vector_effects": [
        _probe("vector", "the black stroke of the left column", 8, 8, frame=0, rgba=[0, 0, 0, 255]),
        _probe(
            "effect",
            "a value scale of 50 halves the red stroke (200, 40, 30)",
            22,
            8,
            frame=0,
            rgba=[100, 20, 15, 255],
        ),
        _probe(
            "background", "nothing is drawn between the strokes", 16, 8, frame=0, rgba=[0, 0, 0, 0]
        ),
        _probe(
            "effect",
            "the blur keyed from 0 to 3 softens the stroke between frames 0 and 5",
            8,
            8,
            frames=[0, 5],
            min_difference=60,
        ),
    ],
    "raster_effects": [
        _probe(
            "toonz_raster",
            "the painted block, style (200, 60, 40) through gamma 1.6",
            14,
            16,
            frame=0,
            rgba=[219, 102, 79, 255],
        ),
        _probe(
            "toonz_raster", "the ink border of the block", 6, 16, frame=0, rgba=[50, 67, 159, 255]
        ),
        _probe(
            "background", "nothing is drawn outside the block", 2, 2, frame=0, rgba=[0, 0, 0, 0]
        ),
        _probe(
            "effect",
            "the hue keyed by 120 degrees turns the block between frames 0 and 5",
            14,
            16,
            frames=[0, 5],
            min_difference=100,
        ),
    ],
    "layered_effects": [
        _probe(
            "toonz_raster",
            "the green block (60, 170, 90) posterized to three levels",
            6,
            22,
            frame=0,
            rgba=[0, 254, 127, 255],
        ),
        _probe(
            "vector",
            "the yellow wave keeps its exact style color: its blur is bypassed",
            16,
            14,
            frame=0,
            rgba=[240, 200, 40, 255],
        ),
        _probe("background", "nothing is drawn in the corner", 28, 28, frame=0, rgba=[0, 0, 0, 0]),
    ],
    "keyed_effects": [
        _probe(
            "vector",
            "the stroke at its first brightness key",
            16,
            16,
            frame=0,
            rgba=[82, 82, 137, 255],
        ),
        _probe(
            "effect",
            "brightness and contrast keyed across the frames lighten the stroke",
            16,
            16,
            frames=[0, 5],
            min_difference=80,
        ),
    ],
    "builtin_effects": [
        _probe(
            "toonz_raster", "the green block under an over", 8, 22, frame=0, rgba=[60, 170, 90, 255]
        ),
        _probe(
            "effect",
            "invertFx turns the red stroke (200, 40, 30) into its complement",
            18,
            16,
            frame=0,
            rgba=[55, 215, 225, 255],
        ),
        _probe(
            "effect", "the color card shows through the over", 2, 2, frame=0, rgba=[24, 36, 70, 255]
        ),
    ],
}

BYPASSED: dict[str, list[str]] = {"layered_effects": ["STD_blurFx"]}


COMPARISONS: dict[str, dict[str, Any]] = {
    "bridge_disabled": {
        "status": "compared",
        "cases": [
            "corpus_scenes_open_read_only_render_and_match_their_baselines",
            "corpus_baselines_are_reproduced_by_this_builds_batch_renderer",
        ],
        "what": (
            "every scene is rendered by two processes that never start the live bridge, the "
            "automation worker (read-only open) and the batch renderer, and both are compared "
            "with the baselines under each scene's policy"
        ),
    },
    "bridge_enabled": {
        "status": "not_built",
        "reason": (
            "The live bridge belongs to the editor and has no tool that opens or renders a scene "
            "file: its tools edit the scene already open. A corpus scene cannot be rendered in a "
            "bridge-enabled editor from a script; it needs a GUI session driven by mcp_live.py "
            "and a scene-open step that does not exist before W4."
        ),
    },
    "assisted_save_reopen": {
        "status": "not_built",
        "reason": (
            "Saving and reopening a corpus scene through the tools needs the editable import of "
            "a user scene (W2 intake, items 11 and 15), which this base does not have: a user "
            "scene opens read-only and cannot be saved."
        ),
    },
}

# Scenes whose read-only render the worker refuses, each with a known violation; none today.
BLOCKED_READ_ONLY_RENDER: set[str] = set()


def declare(root: Path = CORPUS) -> None:
    """Write each scene's effects (read from its file) and probes into the manifest."""
    manifest = load_manifest(root / "manifest.json")
    for name, scene in manifest["scenes"].items():
        scene["effects"] = scene_effects(root, name)
        scene["probes"] = PROBES.get(name, [])
        scene["bypassed"] = BYPASSED.get(name, [])
        scene["read_only_render"] = "blocked" if name in BLOCKED_READ_ONLY_RENDER else "admitted"
    manifest["comparisons"] = COMPARISONS
    (root / "manifest.json").write_text(dump_manifest(manifest), encoding="utf-8")


def _empty_scene_entry(description: str, expected: dict[str, int]) -> dict[str, Any]:
    return {
        "description": description,
        "expected": expected,
        "files": {},
        "scene_sha256": "",
        "policy": {"color": 0, "alpha": 0, "pixels": 0.0, "why": ""},
        "baseline": {"files": {}, "rendered_from": ""},
        "effects": [],
        "bypassed": [],
        "read_only_render": "admitted",
        "probes": [],
    }


def build_scenes(executable: Path, root: Path = CORPUS, only: list[str] | None = None) -> None:
    """Author the scenes (all, or those named) and record their files; baselines are left alone."""
    unknown = sorted(set(only or []) - set(SCENES))
    if unknown:
        raise SystemExit(f"not corpus scenes: {unknown}")
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
        if only and name not in only:
            continue
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
    for name in only or SCENES:
        manifest["scenes"][name]["effects"] = scene_effects(root, name)
        manifest["scenes"][name]["probes"] = PROBES.get(name, [])
        manifest["scenes"][name]["bypassed"] = BYPASSED.get(name, [])
        blocked = name in BLOCKED_READ_ONLY_RENDER
        manifest["scenes"][name]["read_only_render"] = "blocked" if blocked else "admitted"
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


def build_baselines(
    tcomposer: Path, reason: str, root: Path = CORPUS, only: list[str] | None = None
) -> None:
    """Replace the baselines (all, or those of the scenes named) and record why."""
    refuse_unless_deliberate(reason)
    manifest = load_manifest(root / "manifest.json")
    scratch = Path(tempfile.mkdtemp(prefix="corpus-baselines-"))
    changes = []
    for name, scene in manifest["scenes"].items():
        if only and name not in only:
            continue
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
    scenes.add_argument("--only", nargs="*", default=[], help="author only these scenes")
    commands.add_parser("declare", help="write effects and probes into the manifest")
    baselines = commands.add_parser("baselines")
    baselines.add_argument("--tcomposer", type=Path)
    baselines.add_argument("--executable", type=Path)
    baselines.add_argument("--reason", default="")
    baselines.add_argument("--only", nargs="*", default=[], help="render only these scenes")
    arguments = parser.parse_args()
    if arguments.command == "declare":
        declare()
        return 0
    if arguments.command == "scenes":
        build_scenes(arguments.executable, only=arguments.only)
        return 0
    tcomposer = arguments.tcomposer or (
        find_tcomposer(arguments.executable.parent) if arguments.executable else None
    )
    if tcomposer is None:
        raise SystemExit(
            "no batch renderer: build the tcomposer target of this build "
            "(cmake --build <build> --target tcomposer) and pass --tcomposer"
        )
    build_baselines(tcomposer, arguments.reason, only=arguments.only)
    return 0


if __name__ == "__main__":
    sys.exit(main())
