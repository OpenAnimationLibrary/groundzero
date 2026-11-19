"""Qualify the editable import of user scenes: the plan names every loss and changes nothing.

`scene.import_plan` runs in a read-only document's scene host. These cases open the user
scenes of the read-only corpus (a project scene with nothing to lose; a scene with a
sub-xsheet, a 16-bit TIFF and a blur above its authoring range; a scene with an external
program, a missing level and an unknown top-level tag) and one with a raster level wider than
the editable profile admits, and check that each loss is named by kind, that the digest is
stable and follows the source, and that no file under the read roots changes.
"""

from __future__ import annotations

import shutil
import struct
import zlib
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_scene_access_cases import corpus, discard, open_scene, project_id
from mcp_studio_palette_cases import call, refuse, tree_digest

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance
    from mcp_worker import Worker

WIDE_ID = 2200
WIDE_PIXELS = 9000


def png(width: int, height: int) -> bytes:
    """An opaque 8-bit RGBA PNG of one colour."""

    def chunk(kind: bytes, data: bytes) -> bytes:
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    row = b"\0" + bytes((40, 80, 120, 255)) * width
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(row * height))
        + chunk(b"IEND", b"")
    )


def wide_scene(base: Path, folder: Path) -> Path:
    """The base scene with one more PNG level, wider than the editable profile's 8192."""
    shutil.copytree(base / "assets", folder / "assets")
    (folder / "assets" / "wide.0001.png").write_bytes(png(WIDE_PIXELS, 1))
    level = (
        f"<level id='{WIDE_ID}'>\nWide \n"
        '<info dpix="120.000000" dpiy="120.000000"/>\n'
        '<path> "$scenefolder/assets/wide..png" </path>\n</level>\n'
    )
    text = (base / "scene.tnz").read_text()
    text = text.replace("</levels>\n    <folder", level + "</levels>\n    <folder", 1)
    path = folder / "shot_wide.tnz"
    path.write_text(text)
    return path


def plan(case: NativeAcceptance, worker: Worker, document: str, **extra: Any) -> dict[str, Any]:
    return call(case, worker, "scene.import_plan", {"document_id": document, **extra})


def losses(result: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {row["kind"]: row for row in result["losses"]}


def _assert_bounded(case: NativeAcceptance, result: dict[str, Any], limit: int) -> None:
    for row in [*result["losses"], *result["blockers"]]:
        case.assertLessEqual(len(row["samples"]), min(limit, row["count"]), row)
        case.assertGreaterEqual(row["count"], 1, row)


def _project_scene(case: NativeAcceptance, worker: Worker, scene: Path) -> None:
    """A scene the profile holds: no loss, no blocker, only lossless conversions."""
    opened = open_scene(case, worker, scene, project_id(case, worker, "ShowA"))
    result = plan(case, worker, opened["document_id"])
    case.assertEqual(result["losses"], [], result)
    case.assertEqual(result["blockers"], [], result)
    case.assertTrue(result["importable"])
    case.assertEqual(result["source_copy"], "attach")
    case.assertEqual(result["estimate"]["levels"], 3)
    case.assertEqual(result["estimate"]["drawings"], 3)
    conversions = {row["kind"]: row["count"] for row in result["conversions"]}
    case.assertEqual(conversions["assets_collected"], 3, conversions)
    case.assertEqual(result["source_fingerprint"], opened["source"]["source_fingerprint"])
    discard(case, worker, opened["document_id"])


def _structured_scene(case: NativeAcceptance, worker: Worker, scene: Path) -> None:
    """A sub-xsheet used twice, a 16-bit TIFF and a blur above its authoring range."""
    opened = open_scene(case, worker, scene, "standalone")
    result = plan(case, worker, opened["document_id"])
    found = losses(result)
    case.assertEqual(
        set(found), {"sub_xsheet", "raster_depth_reduced", "fx_value_out_of_range"}, result
    )
    case.assertEqual(found["sub_xsheet"]["effect"], "emptied")
    case.assertEqual(found["sub_xsheet"]["count"], 1)
    case.assertEqual(found["sub_xsheet"]["samples"][0]["name"], "Sub")
    deep = found["raster_depth_reduced"]
    case.assertEqual((deep["effect"], deep["samples"][0]["name"]), ("converted", "Deep"))
    case.assertIn("16-bit", deep["samples"][0]["detail"])
    blur = found["fx_value_out_of_range"]
    case.assertEqual(blur["effect"], "clamped")
    case.assertIsNotNone(blur["samples"][0]["fx_id"])
    case.assertTrue(result["importable"], result)
    discard(case, worker, opened["document_id"])


def _unsafe_scene(case: NativeAcceptance, worker: Worker, scene: Path) -> None:
    """An external program, a missing level and an unknown top-level tag."""
    opened = open_scene(case, worker, scene, "standalone")
    result = plan(case, worker, opened["document_id"], sample_limit=1)
    found = losses(result)
    case.assertTrue(
        {"fx_not_admitted", "level_load_failed", "unrecognized_scene_tag"} <= set(found), result
    )
    case.assertEqual(found["fx_not_admitted"]["effect"], "dropped")
    case.assertIn("externalProgramFx", found["fx_not_admitted"]["samples"][0]["detail"])
    case.assertEqual(found["level_load_failed"]["samples"][0]["name"], "Cel")
    case.assertEqual(found["unrecognized_scene_tag"]["samples"][0]["name"], "gzFutureData")
    _assert_bounded(case, result, 1)
    case.assertEqual(result["source_copy"], "attach")
    discard(case, worker, opened["document_id"])


def _wide_scene(case: NativeAcceptance, worker: Worker, scene: Path) -> None:
    """A level past the profile's raster size is a loss of that level, not a blocker."""
    opened = open_scene(case, worker, scene, "standalone")
    result = plan(case, worker, opened["document_id"])
    found = losses(result)
    case.assertEqual(set(found), {"level_not_admitted"}, result)
    wide = found["level_not_admitted"]
    case.assertEqual((wide["effect"], wide["count"]), ("emptied", 1))
    case.assertEqual(wide["samples"][0]["name"], "Wide")
    case.assertTrue(wide["samples"][0]["detail"].startswith("raster_size: "), wide)
    case.assertTrue(result["importable"], result)
    case.assertEqual(result["estimate"]["levels"], 3)
    discard(case, worker, opened["document_id"])


@acceptance_case
def plan_names_every_loss_kind(case: NativeAcceptance) -> None:
    """Each fixture's losses are named by kind with samples; a clean scene has none."""
    scenes = corpus(case)
    wide = wide_scene(scenes["base"], case.inputs / "corpus" / "Wide")
    before = tree_digest(case.inputs)
    worker = case.worker
    _project_scene(case, worker, scenes["A"])
    _structured_scene(case, worker, scenes["B"])
    _unsafe_scene(case, worker, scenes["C"])
    _wide_scene(case, worker, wide)
    case.assertEqual(tree_digest(case.inputs), before)


@acceptance_case
def plan_digest_is_stable_and_follows_the_source(case: NativeAcceptance) -> None:
    """The digest repeats for one source, ignores the sample bound, and changes with it."""
    scenes = corpus(case)
    worker = case.worker
    opened = open_scene(case, worker, scenes["B"], "standalone")
    document = opened["document_id"]
    first = plan(case, worker, document)
    case.assertRegex(first["plan_digest"], r"^plan:[0-9a-f]{64}$")
    case.assertEqual(plan(case, worker, document)["plan_digest"], first["plan_digest"])
    narrow = plan(case, worker, document, sample_limit=1)
    case.assertEqual(narrow["plan_digest"], first["plan_digest"])
    case.assertEqual(narrow["losses"][0]["count"], first["losses"][0]["count"])
    other = open_scene(case, worker, scenes["C"], "standalone")
    case.assertNotEqual(
        plan(case, worker, other["document_id"])["plan_digest"], first["plan_digest"]
    )
    tiff = scenes["B"].parent / "assets" / "deep.0001.tif"
    tiff.write_bytes(tiff.read_bytes()[:-4] + b"\x10\x10\x10\x10")
    details = refuse(case, worker, "scene.import_plan", {"document_id": document}, "source_changed")
    case.assertTrue(any(path.endswith("deep.0001.tif") for path in details["paths"]), details)
    reopened = open_scene(case, worker, scenes["B"], "standalone")
    changed = plan(case, worker, reopened["document_id"])
    case.assertNotEqual(changed["source_fingerprint"], first["source_fingerprint"])
    case.assertNotEqual(changed["plan_digest"], first["plan_digest"])
    for name in (document, other["document_id"], reopened["document_id"]):
        discard(case, worker, name)


@acceptance_case
def plan_changes_nothing(case: NativeAcceptance) -> None:
    """Planning reads only: the read roots, the documents and the hosts stay as they were."""
    scenes = corpus(case)
    worker = case.worker
    opened = open_scene(case, worker, scenes["C"], "standalone")
    document = opened["document_id"]
    before = tree_digest(case.inputs)
    listed = call(case, worker, "automation.list_documents", {})
    for limit in (1, 8, 16):
        plan(case, worker, document, sample_limit=limit)
    case.assertEqual(tree_digest(case.inputs), before)
    case.assertEqual(call(case, worker, "automation.list_documents", {}), listed)
    status = call(
        case, worker, "scene.source_status", {"document_id": document, "expected_revision": "0"}
    )
    case.assertFalse(status["changed"], status)
    editable = case.create("Editable")
    details = refuse(
        case, worker, "scene.import_plan", {"document_id": editable}, "unsupported_target"
    )
    case.assertEqual(details["reason"], "editable_document")
    discard(case, worker, document)
