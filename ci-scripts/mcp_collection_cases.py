"""Collection preflight: what a package save would check, place and weigh, writing nothing."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import random
import struct
import subprocess
import zlib
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_audio_model import tone, wav_file
from mcp_case import acceptance_case
from mcp_output_root_cases import _flags, _granted, _roots
from mcp_rig_cases import _drawing, _hook, _level
from mcp_scene_access_cases import open_scene

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

CHECKS = [
    "document",
    "root",
    "path",
    "parent",
    "collision",
    "filesystem",
    "space",
    "size",
    "journal",
    "external_paths",
]
NOT_CONTAINED = [
    {"class": "reference_images", "reason": "not_admitted_in_editable_profile"},
    {"class": "cast_collections", "reason": "conversion_production_c1"},
    {"class": "fx_external_resources", "reason": "not_admitted"},
    {"class": "models", "reason": "never_packaged"},
    {"class": "fonts", "reason": "not_used_by_worker_scenes"},
    {"class": "plugins", "reason": "builtin_only"},
    {"class": "project_settings", "reason": "private_worker_project"},
    {"class": "editor_view_state", "reason": "user_state"},
]


def _noise_png(path: Path, size: int, seed: int) -> None:
    """An opaque PNG of random colors, which no encoder can compress."""

    def chunk(kind: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(kind + data)
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", crc)

    noise = random.Random(seed)
    rows = bytearray()
    for _ in range(size):
        rows.append(0)
        for _ in range(size):
            rows.extend(noise.randbytes(3) + b"\xff")
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(rows)))
        + chunk(b"IEND", b"")
    )


def _raster(case: NativeAcceptance, document: str) -> None:
    first, second = case.inputs / "noise-1.png", case.inputs / "noise-2.png"
    _noise_png(first, 512, 1)
    _noise_png(second, 512, 2)
    level = case.mutate("level.import", document, path=str(first), name="Noise", dpi=120)
    case.mutate(
        "raster.import_png",
        document,
        level_id=level["level_id"],
        drawing_id=_drawing(2),
        path=str(second),
        dpi=120,
    )
    column = case.mutate("column.create", document, name="Noise")
    case.mutate(
        "column.expose",
        document,
        column_id=column["column_id"],
        level_id=level["level_id"],
        drawing_id=_drawing(1),
        start_frame=0,
        length=25,
    )


def _toonz_raster(case: NativeAcceptance, document: str) -> None:
    level = case.mutate("level.create", document, name="Cel", format="toonzRaster")
    target = {"level_id": level["level_id"], "drawing_id": _drawing(1)}
    case.mutate(
        "drawing.create", document, **target, format="toonzRaster", width=64, height=64, dpi=120
    )
    noise = random.Random(3)
    pixels = [
        {"ink": noise.randrange(2), "paint": noise.randrange(2), "tone": noise.randrange(256)}
        for _ in range(64 * 16)
    ]
    case.mutate(
        "raster.patch",
        document,
        **target,
        coordinate_space="raster_pixels_lower_left",
        x=0,
        y=0,
        width=64,
        height=16,
        sharing="all_exposures",
        pixels=pixels,
    )


def _vector(case: NativeAcceptance, document: str) -> None:
    level = _level(case, document, "Ink", 2)
    points = [{"x": x, "y": x / 2, "thickness": 1} for x in range(-40, 41, 4)]
    edit = {"action": "add", "style_id": 1, "closed": False, "points": points}
    for number in (1, 2):
        case.mutate(
            "vector.edit",
            document,
            level_id=level,
            drawing_id=_drawing(number),
            coordinate_space="drawing_native",
            sharing="all_exposures",
            edits=[edit],
        )
    _hook(case, document, level, 1, {1: (0.1, 0.2), 2: (0.2, 0.3)})


def _sound(case: NativeAcceptance, document: str) -> None:
    path = case.inputs / "tone.wav"
    path.write_bytes(wav_file(tone(48000, "pcm_s16", 9600)))
    level = case.mutate("audio.import", document, path=str(path), name="Tone")["source"]
    column = case.mutate("audio.column.create", document, name="Sound", volume_permille=1000)
    case.mutate(
        "audio.clip.place",
        document,
        column_id=column["column"]["column_id"],
        level_id=level["level_id"],
        placement={"timeline_start_frame": 0, "source_start_frame": 0, "frame_count": 4},
        overlap_policy="reject",
    )


def _every_kind(case: NativeAcceptance) -> str:
    """A worker scene with raster, Toonz raster and vector levels, hooks and a sound."""
    document = case.create("Collected")
    _raster(case, document)
    _toonz_raster(case, document)
    _vector(case, document)
    _sound(case, document)
    return document


def _preflight(case: NativeAcceptance, document: str, directory: str, **extra: Any) -> Any:
    params = {
        "document_id": document,
        "expected_revision": case.revisions[document],
        "directory": directory,
        **extra,
    }
    return case.success("scene.save_preflight", params)


def _refused(case: NativeAcceptance, document: str, **params: Any) -> dict[str, Any]:
    full = {"document_id": document, "expected_revision": case.revisions[document], **params}
    return case.failure("scene.save_preflight", full)


def _states(result: dict[str, Any]) -> dict[str, str]:
    return {check["name"]: check["state"] for check in result["checks"]}


def _failed(result: dict[str, Any], name: str) -> dict[str, Any]:
    check = next(check for check in result["checks"] if check["name"] == name)
    return {"state": check["state"], "code": check["code"]}


def _tree(root: Path) -> dict[str, str]:
    return {
        str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
        if path.is_file()
        else "dir"
        for path in sorted(root.rglob("*"))
        if ".groundzero" not in path.parts and "full" not in path.parts
    }


def _all_items(case: NativeAcceptance, document: str, directory: str) -> list[dict[str, Any]]:
    items: list[dict[str, Any]] = []
    offset: int | None = 0
    while offset is not None:
        page = _preflight(case, document, directory, offset=offset, limit=2)
        items += page["items"]
        offset = page["next_offset"]
    return items


@acceptance_case
def preflight_names_every_dependency_and_bounds_the_size(case: NativeAcceptance) -> None:
    """The preflight of a ready save passes every check, places each dependency in the
    package where the save then writes it, bounds the package's size and files from above
    and states what the package does not contain; it writes nothing, and once the package
    exists the same preflight names the collision."""
    document = _every_kind(case)
    before = _tree(case.workspace)
    result = _preflight(case, document, str(case.workspace / "collected"))
    case.assertEqual(_tree(case.workspace), before)
    states = _states(result)
    case.assertEqual(list(states), CHECKS)
    case.assertIn(states.pop("filesystem"), {"pass", "unknown"})
    case.assertEqual(set(states.values()), {"pass"}, result["checks"])
    case.assertEqual((result["ready"], result["first_failure"]), (True, None))
    case.assertEqual(result["not_contained"], NOT_CONTAINED)
    plan = result["plan"]
    case.assertEqual(
        (plan["location"]["root"], plan["location"]["path"], plan["staging"], plan["save_mode"]),
        ("workspace", "collected", "private_rename", "self_contained"),
    )
    case.assertIs(plan["same_volume_as_workspace"], True)
    items = _all_items(case, document, str(case.workspace / "collected"))
    case.assertEqual(len(items), result["items_total"])
    case.assertEqual(items[: len(result["items"])], result["items"])
    roles = {(item["role"], item["source"]) for item in items}
    case.assertEqual(
        roles,
        {
            ("primary", "memory"),
            ("palette", "memory"),
            ("hook_sidecar", "memory"),
            ("sound", "private_copy"),
        },
    )
    hook = next(item for item in items if item["role"] == "hook_sidecar")
    case.assertEqual((hook["name"], hook["availability"]), ("Ink", "unsaved"))
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "collected"))
    package = Path(saved["package_directory"])
    _matches_package(case, items, package)
    files = [path for path in package.rglob("*") if path.is_file()]
    written = sum(path.stat().st_size for path in files)
    bound = plan["bytes_estimate"]
    case.assertLessEqual(written, bound["upper_bound"])
    case.assertLessEqual(len(files), plan["files_estimate"])
    sound = next(item for item in items if item["role"] == "sound")
    case.assertEqual(bound["known_bytes"], sound["size_bytes"])
    scene = (package / "scene.tnz").read_text(errors="replace")
    for root in (case.workspace, case.inputs, case.directory):
        case.assertNotIn(str(root), scene)
    again = _preflight(case, document, str(case.workspace / "collected"))
    case.assertEqual(
        (again["ready"], again["first_failure"], _failed(again, "collision")),
        (False, "collision", {"state": "fail", "code": "already_exists"}),
    )


def _matches_package(case: NativeAcceptance, items: list[dict[str, Any]], package: Path) -> None:
    """Every packaged item is where the save wrote it."""
    manifest = json.loads((package / "manifest.json").read_text())
    written = {entry["path"] for entry in manifest["files"]}
    written |= {level["path"] for level in manifest["levels"]}
    for item in items:
        with case.subTest(item=item):
            case.assertIn(item["package_path"], written)


def _full_volume(case: NativeAcceptance) -> Path | None:
    """A small disk image mounted inside the workspace."""
    if platform.system() != "Darwin":
        return None
    image = case.directory / "full.dmg"
    mount = case.workspace / "full"
    mount.mkdir()
    subprocess.run(
        ["hdiutil", "create", "-size", "8m", "-fs", "APFS", "-volname", "gzqfull", str(image)],
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


def _place_failures(case: NativeAcceptance, document: str) -> None:
    taken = case.workspace / "taken"
    taken.mkdir()
    locked = case.workspace / "locked"
    locked.mkdir()
    locked.chmod(0o555)
    case.addCleanup(locked.chmod, 0o755)
    volume = _full_volume(case)
    before = _tree(case.workspace)
    expected = {
        "taken": ("collision", "already_exists"),
        "absent/package": ("parent", "missing_parent"),
        "locked/package": ("parent", "package_write_failed"),
    }
    for directory, (name, code) in expected.items():
        with case.subTest(directory=directory):
            result = _preflight(case, document, str(case.workspace / directory))
            case.assertEqual(result["first_failure"], name)
            case.assertEqual(_failed(result, name), {"state": "fail", "code": code})
    if volume is not None:
        result = _preflight(case, document, str(volume / "package"))
        case.assertEqual(_failed(result, "space"), {"state": "fail", "code": "insufficient_space"})
        case.assertIs(result["plan"]["same_volume_as_workspace"], False)
        case.assertEqual(sorted(path.name for path in volume.iterdir() if path.name[0] != "."), [])
    case.assertEqual(_tree(case.workspace), before)


@acceptance_case
def preflight_reports_every_failing_check(case: NativeAcceptance) -> None:
    """Each failing check is a result naming its code, the first failure first; a read-only
    document, a stale revision, a root not granted, a path outside the grammar and an offset
    past the items are errors; nothing is written either way."""
    document = _every_kind(case)
    empty = case.create("Empty level")
    case.mutate("level.create", empty, name="Blank", format="vector")
    before = _tree(case.workspace)
    result = _preflight(case, empty, str(case.workspace / "blank"))
    case.assertEqual(
        (result["first_failure"], _failed(result, "document")),
        ("document", {"state": "fail", "code": "empty_level"}),
    )
    case.assertEqual(_tree(case.workspace), before)
    if platform.system() != "Windows":
        _place_failures(case, document)
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "source"))
    opened = open_scene(case, case.worker, Path(saved["scene_path"]), "standalone")
    case.revisions[opened["document_id"]] = opened["revision"]
    error = _refused(case, opened["document_id"], directory=str(case.workspace / "copy"))
    case.assertEqual(
        (error["code"], error["details"]["reason"], error["details"]["next"]),
        ("unsupported_target", "read_only_document", "scene.import_plan"),
    )
    stale = case.failure(
        "scene.save_preflight",
        {"document_id": document, "expected_revision": "0", "directory": "x"},
    )
    case.assertEqual(stale["code"], "stale_revision")
    case.assertEqual(
        _refused(case, document, directory="x", root="renders")["code"], "root_unavailable"
    )
    case.assertEqual(_refused(case, document, directory="x", offset=4096)["code"], "invalid_params")
    _root_refusals(case)


def _root_refusals(case: NativeAcceptance) -> None:
    roots = _roots(case, "renders")
    (roots["renders"] / "real").mkdir()
    os.symlink(roots["renders"] / "real", roots["renders"] / "alias")
    _granted(case, _flags(roots))
    document = case.create("Rooted")
    case.mutate("level.create", document, name="Ink", format="vector")
    result = _preflight(case, document, "real/package", root="renders")
    plan = result["plan"]
    case.assertEqual(
        (plan["location"]["root"], plan["location"]["path"], plan["staging"]),
        ("renders", "real/package", "private_then_copy"),
    )
    case.assertRegex(plan["location"]["root_sha256"], "^[0-9a-f]{64}$")
    linked = _preflight(case, document, "alias/package", root="renders")
    case.assertEqual(_failed(linked, "path"), {"state": "fail", "code": "path_denied"})
    for directory, reason in (
        ("../escape", "grammar"),
        ("real/.groundzero-x", "reserved"),
        ("/absolute", "grammar"),
        ("real/new name", "grammar"),
    ):
        with case.subTest(directory=directory):
            error = _refused(case, document, directory=directory, root="renders")
            case.assertEqual((error["code"], error["details"]["reason"]), ("invalid_path", reason))
    case.assertEqual(sorted(os.listdir(roots["renders"])), ["alias", "real"])
    case.assertEqual(os.listdir(roots["renders"] / "real"), [])
