"""Qualify Library listing caps, thumbnail bounds, collisions and unreadable dependencies."""

from __future__ import annotations

import base64
import os
import struct
import sys
import zlib
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_library_cases import _build_library, _package_scene
from mcp_studio_palette_cases import call, refuse, shared_worker
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

LISTING_CAP = 256
FOLDER_CAP = 8192
ICON_BYTES = 1024 * 1024
ICON_PIXELS = 1024
SIGNATURE = b"\x89PNG\r\n\x1a\n"


def _chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def _png(width: int, height: int, size: int = 0) -> bytes:
    """Encode a transparent PNG, padded with a text chunk to an exact byte size when asked."""
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    rows = (b"\0" + bytes(width * 4)) * height
    image = [_chunk(b"IHDR", header), _chunk(b"IDAT", zlib.compress(rows)), _chunk(b"IEND", b"")]
    encoded = SIGNATURE + b"".join(image)
    if size:
        padding = size - len(encoded) - 12
        if padding < len(b"Comment\0"):
            raise ValueError("The requested PNG size is below its minimum")
        text = _chunk(b"tEXt", b"Comment\0" + b"x" * (padding - len(b"Comment\0")))
        encoded = SIGNATURE + b"".join([image[0], text, image[1], image[2]])
    return encoded


def _folder(case: NativeAcceptance, name: str) -> Path:
    library = case.directory / name
    (library / "templates" / "sceneIcons").mkdir(parents=True)
    (library / "poses").mkdir()
    return library


def _names(listing: dict[str, Any]) -> list[str]:
    return [item["name"] for item in listing["items"]]


def _list(case: NativeAcceptance, worker: Worker, **params: Any) -> dict[str, Any]:
    result: dict[str, Any] = call(case, worker, "library.list", {"kind": "template", **params})
    return result


@acceptance_case
def library_listing_pages_are_capped(case: NativeAcceptance) -> None:
    """Cap pages at 256 items and offsets at 8192, and refuse a folder of more entries.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    library = _folder(case, "paged")
    for number in range(300):
        (library / "templates" / f"Item {number:04}.tnz").write_bytes(b"")
    worker = shared_worker(case, {"library": library})
    first = _list(case, worker, limit=LISTING_CAP)
    case.assertEqual((len(first["items"]), first["total"]), (LISTING_CAP, 300))
    case.assertEqual(first["next_offset"], LISTING_CAP)
    rest = _list(case, worker, offset=LISTING_CAP, limit=LISTING_CAP)
    case.assertEqual(len(rest["items"]), 44)
    case.assertIsNone(rest["next_offset"])
    case.assertEqual(_names(first) + _names(rest), sorted(_names(first) + _names(rest)))
    case.assertEqual(len(set(_names(first) + _names(rest))), 300)
    for params in ({"limit": LISTING_CAP + 1}, {"limit": 0}, {"offset": FOLDER_CAP + 1}):
        refuse(case, worker, "library.list", {"kind": "template", **params}, case.schema_code())
    past = _list(case, worker, offset=FOLDER_CAP)
    case.assertEqual((past["items"], past["next_offset"]), ([], None))
    for number in range(300, FOLDER_CAP - 1):
        (library / "templates" / f"Item {number:04}.tnz").write_bytes(b"")
    case.assertEqual(_list(case, worker, limit=1)["total"], FOLDER_CAP - 1)
    (library / "templates" / "One too many.tnz").write_bytes(b"")
    details = refuse(case, worker, "library.list", {"kind": "template"}, "resource_limit")
    case.assertEqual(details["limit"], FOLDER_CAP)


def _template_icon(library: Path, name: str, data: bytes) -> None:
    (library / "templates" / f"{name}.tnz").write_bytes(b"")
    (library / "templates" / "sceneIcons" / f"{name} .png").write_bytes(data)


def _thumbnail(case: NativeAcceptance, worker: Worker, name: str) -> dict[str, Any]:
    params = {"kind": "template", "name": name}
    result: dict[str, Any] = call(case, worker, "library.thumbnail", params)
    return result


@acceptance_case
def library_thumbnails_are_bounded(case: NativeAcceptance) -> None:
    """Admit icons of exactly 1 MiB and 1024 pixels and refuse one byte or pixel more.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    library = _folder(case, "icons")
    icons = {
        "ByteEdge": _png(8, 8, ICON_BYTES),
        "ByteOver": _png(8, 8, ICON_BYTES + 1),
        "WideEdge": _png(ICON_PIXELS, 1),
        "WideOver": _png(ICON_PIXELS + 1, 1),
        "TallOver": _png(1, ICON_PIXELS + 1),
        "NotPng": b"GIF89a" + bytes(64),
    }
    for name, data in icons.items():
        _template_icon(library, name, data)
    worker = shared_worker(case, {"library": library})
    before = sorted(library.rglob("*"))
    listed = {item["name"]: item["thumbnail"] for item in _list(case, worker)["items"]}
    case.assertEqual(listed["ByteEdge"], "present")
    case.assertEqual(listed["WideEdge"], "present")
    for name in ("ByteOver", "WideOver", "TallOver", "NotPng"):
        case.assertEqual(listed[name], "invalid", name)
    for name in ("ByteEdge", "WideEdge"):
        icon = _thumbnail(case, worker, name)
        case.assertLessEqual(max(icon["width"], icon["height"]), 512, name)
    refuse(
        case,
        worker,
        "library.thumbnail",
        {"kind": "template", "name": "ByteOver"},
        "resource_limit",
    )
    refuse(
        case,
        worker,
        "library.thumbnail",
        {"kind": "template", "name": "WideOver"},
        "resource_limit",
    )
    refuse(
        case,
        worker,
        "library.thumbnail",
        {"kind": "template", "name": "TallOver"},
        "resource_limit",
    )
    refuse(
        case,
        worker,
        "library.thumbnail",
        {"kind": "template", "name": "NotPng"},
        "asset_unavailable",
    )
    case.assertEqual(sorted(library.rglob("*")), before)


def _pose_set(library: Path, name: str, thumbnail: bytes) -> None:
    encoded = base64.b64encode(thumbnail).decode()
    (library / "poses" / f"{name}.tposes").write_text(
        "<poseSet version='1'><base>Col1 x 0 </base><pose><name>\"Wave\"</name>"
        f'<entries>Col1 x 1.5 </entries><thumbnail>"{encoded}"</thumbnail></pose></poseSet>\n'
    )


@acceptance_case
def library_pose_thumbnails_are_bounded(case: NativeAcceptance) -> None:
    """Apply the same byte and pixel bounds to a pose thumbnail inside its pose set.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    library = _folder(case, "poses")
    _pose_set(library, "Edge", _png(ICON_PIXELS, 1))
    _pose_set(library, "Wide", _png(ICON_PIXELS + 1, 1))
    _pose_set(library, "Heavy", _png(8, 8, ICON_BYTES + 1))
    worker = shared_worker(case, {"library": library})
    params = {"kind": "pose_set", "pose_index": 0}
    case.assertEqual(
        call(case, worker, "library.thumbnail", {**params, "name": "Edge"})["pose_index"], 0
    )
    for name in ("Wide", "Heavy"):
        refuse(case, worker, "library.thumbnail", {**params, "name": name}, "resource_limit")


@acceptance_case
def library_listing_revision_tracks_real_changes(case: NativeAcceptance) -> None:
    """Refuse a stale listing revision after a file really changes, and report the new one.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    library = _folder(case, "changing")
    (library / "templates" / "Alpha.tnz").write_bytes(b"")
    worker = shared_worker(case, {"library": library})
    listing = _list(case, worker)
    revision = listing["listing_revision"]
    case.assertEqual(_names(_list(case, worker, expected_listing_revision=revision)), ["Alpha"])
    (library / "templates" / "Beta.tnz").write_bytes(b"")
    details = refuse(
        case,
        worker,
        "library.list",
        {"kind": "template", "expected_listing_revision": revision},
        "revision_conflict",
    )
    current = _list(case, worker)
    case.assertEqual(details["listing_revision"], current["listing_revision"])
    case.assertNotEqual(current["listing_revision"], revision)
    case.assertEqual(_names(current), ["Alpha", "Beta"])
    case.assertEqual(
        _names(_list(case, worker, expected_listing_revision=details["listing_revision"])),
        ["Alpha", "Beta"],
    )
    (library / "templates" / "Beta.tnz").unlink()
    case.assertEqual(_list(case, worker)["listing_revision"], revision)
    for malformed in ("listing:abc", "sha256:" + "0" * 64, ""):
        refuse(
            case,
            worker,
            "library.list",
            {"kind": "template", "expected_listing_revision": malformed},
            case.schema_code(),
        )


@acceptance_case
def library_case_fold_collisions_are_reported(case: NativeAcceptance) -> None:
    """Flag items whose names differ only by case, and no others.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    library = _folder(case, "folding")
    names = ("Rig", "rig", "RIG", "Other")
    for name in names:
        (library / "templates" / f"{name}.tnz").write_bytes(b"")
    if len(list((library / "templates").glob("*.tnz"))) != len(names):
        case.skipTest(
            "The output volume is case-insensitive, so Rig.tnz, rig.tnz and RIG.tnz are one file; "
            "run with --output on a case-sensitive volume"
        )
    worker = shared_worker(case, {"library": library})
    listing = _list(case, worker)
    flags = {item["name"]: item["case_fold_collision"] for item in listing["items"]}
    case.assertEqual(flags, {"Rig": True, "rig": True, "RIG": True, "Other": False})
    case.assertEqual(_names(listing), ["Other", "RIG", "Rig", "rig"])
    filtered = _list(case, worker, name_contains="RIG")
    case.assertEqual(_names(filtered), ["RIG", "Rig", "rig"])


@acceptance_case
def library_unreadable_dependency_is_refused_not_hidden(case: NativeAcceptance) -> None:
    """Report a permission-denied level file as a typed I/O error and keep serving.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    if sys.platform == "win32":
        case.skipTest("POSIX permission bits do not deny access on Windows")
    package = _package_scene(case)
    library = case.directory / "library"
    _build_library(case, package, library)
    level = library / "templates" / "Rig" / "level-0.pli"
    worker = shared_worker(case, {"library": library})
    params = {"kind": "template", "name": "Rig"}
    readable = call(case, worker, "library.inspect", params)
    case.addCleanup(level.chmod, 0o644)
    level.chmod(0)
    if os.access(level, os.R_OK):
        case.skipTest("The test user can read files without permission bits")
    refuse(case, worker, "library.inspect", params, "io_error")
    legacy = call(case, worker, "library.inspect", {"kind": "template", "name": "Legacy"})
    case.assertFalse(legacy["dependencies_complete"])
    level.chmod(0o644)
    case.assertEqual(call(case, worker, "library.inspect", params), readable)
