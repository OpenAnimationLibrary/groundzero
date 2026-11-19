"""Intake: preflight, explicit copy or link import, provenance, retries and SVG export."""

from __future__ import annotations

import hashlib
import json
import os
import struct
import zlib
from pathlib import Path
from typing import TYPE_CHECKING, Any
from xml.etree import ElementTree

from mcp_case import acceptance_case
from mcp_png import SIGNATURE
from mcp_vector_opacity_cases import _fixture as _vector_fixture

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

DPI_300 = 11811
SVG = "{http://www.w3.org/2000/svg}"


def _chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(
    size: tuple[int, int] = (4, 4),
    *,
    shade: int = 0,
    color_type: int = 6,
    depth: int = 8,
    density: int | None = DPI_300,
    extra: tuple[tuple[bytes, bytes], ...] = (),
    header_size: tuple[int, int] | None = None,
) -> bytes:
    """Encode a small PNG whose pixels depend on `shade`, with optional ancillary chunks."""
    width, height = size
    channels = {0: 1, 2: 3, 6: 4}[color_type]
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            sample = [(shade + 40 * x) % 256, (80 * y) % 256, 200, 128 + x][:channels]
            for value in sample:
                rows.extend(bytes([value]) * (depth // 8))
    declared = header_size or size
    ihdr = struct.pack(">IIBBBBB", *declared, depth, color_type, 0, 0, 0)
    chunks = [_chunk(b"IHDR", ihdr)]
    if density is not None:
        chunks.append(_chunk(b"pHYs", struct.pack(">IIB", density, density, 1)))
    chunks.extend(_chunk(kind, data) for kind, data in extra)
    chunks += [_chunk(b"IDAT", zlib.compress(bytes(rows))), _chunk(b"IEND", b"")]
    return SIGNATURE + b"".join(chunks)


def _sequence(directory: Path, numbers: list[int], **options: Any) -> dict[int, str]:
    directory.mkdir(parents=True, exist_ok=True)
    digests = {}
    for number in numbers:
        data = png(shade=number, **options)
        (directory / f"shot.{number:04d}.png").write_bytes(data)
        digests[number] = hashlib.sha256(data).hexdigest()
    return digests


def _source(directory: Path, **selection: Any) -> dict[str, Any]:
    if selection:
        return {"source_directory": str(directory), **selection}
    return {
        "source_directory": str(directory),
        "sequence_prefix": "shot",
        "sequence_extension": "png",
    }


def _preflight(case: NativeAcceptance, document: str, **params: Any) -> dict[str, Any]:
    return case.success("intake.preflight", {"document_id": document, **params})


def _import(case: NativeAcceptance, document: str, **params: Any) -> dict[str, Any]:
    choices = {
        "storage": "copy",
        "on_existing": "refuse",
        "dpi_policy": "source",
        "exposure": "none",
    }
    return case.mutate("intake.import", document, **{**choices, **params})


def _refused(case: NativeAcceptance, document: str, code: str, **params: Any) -> dict[str, Any]:
    choices = {
        "storage": "copy",
        "on_existing": "refuse",
        "dpi_policy": "source",
        "exposure": "none",
        "name": "Refused",
    }
    request = case.write_params(document, {**choices, **params})
    return case.handler_failure("intake.import", request, code)


def _provenance(case: NativeAcceptance, document: str, level: str) -> dict[str, Any]:
    return case.success("intake.provenance", {"document_id": document, "level_id": level})


@acceptance_case
def intake_preflight_reports_frames_gaps_alpha_dpi_and_collisions(case: NativeAcceptance) -> None:
    """Preflight a noncontiguous sequence and name what an import would meet (PRD-06 #1, F2).

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Preflight")
    source = case.inputs / "preflight"
    _sequence(source, [1, 2, 3, 6, 10])
    revision = case.revisions[document]
    taken = case.mutate("level.create", document, name="Shot", format="vector")
    report = _preflight(case, document, **_source(source), name="Shot", limit=2)
    case.assertEqual(report["revision"], case.revisions[document])
    case.assertNotEqual(report["revision"], revision)
    case.assertEqual([row["name"] for row in report["files"]], ["shot.0001.png", "shot.0002.png"])
    case.assertEqual(report["next_offset"], 2)
    row = report["files"][0]
    case.assertEqual((row["format"], row["width"], row["height"]), ("png", 4, 4))
    case.assertEqual(
        (row["pixel_format"], row["alpha"], row["status"]), ("rgba8", "channel", "supported")
    )
    case.assertAlmostEqual(row["dpi_x"], DPI_300 * 0.0254)
    sequence = report["sequence"]
    case.assertEqual((sequence["first"], sequence["last"], sequence["count"]), (1, 10, 5))
    case.assertFalse(sequence["contiguous"])
    case.assertEqual(sequence["missing_frames"], 5)
    case.assertEqual(sequence["gaps"], [{"first": 4, "last": 5}, {"first": 7, "last": 9}])
    case.assertEqual(report["summary"]["files_with_alpha"], 5)
    case.assertTrue(report["summary"]["uniform_dpi"])
    case.assertEqual(report["frame_rate"]["scene_frame_rate"], 24)
    case.assertIsNone(report["frame_rate"]["source_rate"])
    destination = report["destination"]
    case.assertEqual(destination["collision"], "level_exists")
    case.assertEqual(destination["existing_level_id"], taken["level_id"])
    case.assertTrue(report["link_available"])
    case.assertTrue(report["importable"])
    case.assertRegex(report["plan_digest"], "^[0-9a-f]{64}$")
    again = _preflight(case, document, **_source(source), name="Shot", offset=4)
    case.assertEqual(again["plan_digest"], report["plan_digest"])
    case.assertIsNone(again["next_offset"])
    case.assertEqual(case.revisions[document], report["revision"])
    stale = case.failure(
        "intake.preflight",
        {"document_id": document, "expected_revision": revision, **_source(source)},
    )
    case.assertEqual(stale["code"], "stale_revision")


@acceptance_case
def intake_names_and_refuses_unsupported_formats(case: NativeAcceptance) -> None:
    """Name each format by its signature and refuse what intake cannot import (PRD-06 F3).

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Formats")
    source = case.inputs / "formats"
    source.mkdir()
    files = {
        "photo.png": (b"\xff\xd8\xff\xe0\x00\x10JFIF\x00", "jpeg", "unsupported_format"),
        "scan.png": (b"II*\x00\x08\x00\x00\x00", "tiff", "unsupported_format"),
        "layers.png": (b"8BPS\x00\x01\x00\x00", "psd", "unsupported_format"),
        "vector.png": (b'<?xml version="1.0"?><svg xmlns="x"/>', "svg", "unsupported_format"),
        "deep.png": (png(depth=16), "png", "unsupported_precision"),
        "broken.png": (SIGNATURE + b"\x00\x00\x00\x0dIHDX", "png", "invalid_asset"),
    }
    for name, (data, _format, _code) in files.items():
        (source / name).write_bytes(data)
    report = _preflight(case, document, **_source(source, file_names=sorted(files)))
    rows = {row["name"]: row for row in report["files"]}
    for name, (_data, kind, code) in files.items():
        with case.subTest(file=name):
            case.assertEqual((rows[name]["format"], rows[name]["status"]), (kind, code))
            _refused(case, document, code, **_source(source, file_names=[name]))
    case.assertFalse(report["importable"])
    case.assertEqual(report["blocker_total"], len(files))


@acceptance_case
def intake_copy_import_reports_manifest_drawings_and_exposure(case: NativeAcceptance) -> None:
    """Import a gapped sequence as private copies held over the gaps (PRD-06 #2, #3, F2).

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Copy")
    source = case.inputs / "copy"
    digests = _sequence(source, [1, 2, 3, 6])
    plan = _preflight(case, document, **_source(source), name="Shot")["plan_digest"]
    result = _import(
        case,
        document,
        **_source(source),
        name="Shot",
        on_gap="hold",
        exposure="new_column",
        start_frame=10,
        plan_digest=plan,
    )
    case.assertEqual(result["outcome"], "created")
    level = result["level"]
    case.assertEqual([d["number"] for d in level["drawings"]], [1, 2, 3, 6])
    case.assertEqual(level["format"], "rgba8")
    case.assertAlmostEqual(result["dpi_x"], DPI_300 * 0.0254)
    copies = result["copied_files"]
    case.assertEqual([c["sha256"] for c in copies], [digests[n] for n in (1, 2, 3, 6)])
    case.assertEqual([d["sha256"] for d in result["drawings"]], [c["sha256"] for c in copies])
    case.assertTrue(all(c["copy_name"].endswith(".png") for c in copies))
    exposure = result["exposure"]
    case.assertEqual(
        (exposure["start_frame"], exposure["length"], exposure["gap_rows"]), (10, 6, 2)
    )
    cells = case.success(
        "column.cells",
        {
            "document_id": document,
            "column_id": exposure["column_id"],
            "start_frame": 9,
            "length": 8,
        },
    )["cells"]
    shown = [cell["drawing_id"]["number"] if not cell["empty"] else None for cell in cells]
    case.assertEqual(shown, [None, 1, 2, 3, 3, 3, 6, None])
    (source / "shot.0002.png").write_bytes(png(shade=99, size=(4, 4)))
    status = _provenance(case, document, level["level_id"])
    case.assertEqual(
        (status["storage"], status["level_path"], status["copies"]), ("copy", "private_store", 4)
    )
    case.assertEqual(status["source_check"]["counts"]["modified"], 1)
    case.assertEqual(case.preview(document), case.preview(document))


@acceptance_case
def intake_link_and_copy_keep_different_provenance(case: NativeAcceptance) -> None:
    """Import one sequence as a link and as a copy; provenance survives a package (SCN-05 #1).

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Provenance")
    source = case.inputs / "shared"
    _sequence(source, [1, 2, 3])
    copied = _import(case, document, **_source(source), name="Copied")
    linked = _import(case, document, **_source(source), name="Linked", storage="link")
    case.assertEqual(len(copied["copied_files"]), 3)
    case.assertEqual(linked["copied_files"], [])
    case.assertEqual(linked["drawings"], copied["drawings"])
    before = {
        name: _provenance(case, document, result["level"]["level_id"])
        for name, result in (("Copied", copied), ("Linked", linked))
    }
    case.assertEqual(before["Copied"]["level_path"], "private_store")
    case.assertEqual(before["Linked"]["level_path"], "linked_source")
    case.assertEqual(before["Linked"]["copies"], 0)
    package = case.workspace / "provenance-package"
    case.mutate("scene.save_copy", document, directory=str(package))
    manifest = json.loads((package / "manifest.json").read_text())
    case.assertEqual(manifest["intake"]["Linked"]["storage"], "link")
    opened = case.mutate("scene.open", None, path=str(package))["document_id"]
    levels = case.success("scene.summary", {"document_id": opened})["levels"]
    for level in levels:
        after = _provenance(case, opened, level["level_id"])
        with case.subTest(level=level["name"]):
            case.assertEqual(after["level_path"], "package")
            for key in ("storage", "sources", "dpi_x", "source_directory"):
                case.assertEqual(after[key], before[level["name"]][key])
    listed = _source(case.inputs / "shared", file_names=["shot.0001.png"])
    error = _refused(case, document, "invalid_params", **listed, storage="link")
    case.assertEqual(error["details"]["reason"], "link_unavailable")


@acceptance_case
def intake_retries_and_reuse_never_duplicate_assets(case: NativeAcceptance) -> None:
    """Exact retries replay, new retries reuse the identical level, changes refuse (PRD-06 #4).

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Retry")
    source = case.inputs / "retry"
    _sequence(source, [1, 2])
    params = case.write_params(
        document,
        {
            **_source(source),
            "name": "Shot",
            "storage": "copy",
            "on_existing": "reuse_identical",
            "dpi_policy": "value",
            "dpi": 150,
            "exposure": "new_column",
        },
    )
    first = case.success("intake.import", params)
    case.assertEqual(case.success("intake.import", params), first)
    case.revisions[document] = first["revision"]
    summary = case.success("scene.summary", {"document_id": document})
    retry = {key: value for key, value in params.items() if key != "operation_id"}
    reused = case.mutate("intake.import", document, **retry)
    case.assertEqual(reused["outcome"], "reused")
    case.assertEqual(reused["level"]["level_id"], first["level"]["level_id"])
    case.assertEqual((reused["copied_files"], reused["exposure"]), ([], None))
    case.assertEqual(reused["revision"], first["revision"])
    case.assertEqual(case.success("scene.summary", {"document_id": document}), summary)
    refuse = {**retry, "on_existing": "refuse"}
    del refuse["document_id"], refuse["expected_revision"]
    case.assertEqual(
        _refused(case, document, "name_conflict", **refuse)["details"]["reason"], "exists"
    )
    (source / "shot.0002.png").write_bytes(png(shade=77))
    changed = {**refuse, "on_existing": "reuse_identical"}
    error = _refused(case, document, "name_conflict", **changed)
    case.assertEqual(error["details"]["differences"], ["sources"])
    plan = _preflight(case, document, **_source(source), name="Other")["plan_digest"]
    (source / "shot.0001.png").write_bytes(png(shade=78, size=(4, 4)))
    _refused(case, document, "source_changed", **_source(source), name="Other", plan_digest=plan)


@acceptance_case
def intake_requires_explicit_choices_and_honest_headers(case: NativeAcceptance) -> None:
    """Refuse silent defaults: gaps, missing DPI, mixed sizes, size limits and lying headers.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Choices")
    gapped = case.inputs / "gapped"
    _sequence(gapped, [1, 3])
    error = _refused(case, document, "invalid_params", **_source(gapped))
    case.assertEqual((error["details"]["reason"], error["details"]["missing_frames"]), ("gaps", 1))
    flat = case.inputs / "flat"
    _sequence(flat, [1, 2], density=None)
    error = _refused(case, document, "invalid_asset", **_source(flat))
    case.assertEqual(error["details"]["reason"], "dpi_missing")
    error = _refused(case, document, "invalid_params", **_source(flat), dpi_policy="value")
    case.assertEqual(error["details"]["reason"], "dpi")
    mixed = case.inputs / "mixed"
    mixed.mkdir()
    (mixed / "a.png").write_bytes(png(size=(4, 4)))
    (mixed / "b.png").write_bytes(png(size=(5, 4)))
    error = _refused(
        case, document, "invalid_params", **_source(mixed, file_names=["a.png", "b.png"])
    )
    case.assertEqual(error["details"]["reason"], "mixed_size")
    many = case.inputs / "many"
    _sequence(many, list(range(1, 66)), size=(1, 1))
    error = _refused(case, document, "resource_limit", **_source(many))
    case.assertEqual(error["details"]["limit"], "inline_import")
    lying = case.inputs / "lying"
    lying.mkdir()
    (lying / "big.png").write_bytes(png(size=(4, 4), header_size=(64, 64)))
    error = _refused(case, document, "invalid_asset", **_source(lying, file_names=["big.png"]))
    case.assertIn(error["details"]["reason"], {"header_mismatch", "decode_failed"})
    os.symlink(case.fixture, lying / "linked.png")
    _refused(case, document, "path_denied", **_source(lying, file_names=["linked.png"]))
    report = _preflight(case, document, **_source(lying, file_names=["linked.png", "gone.png"]))
    case.assertEqual([row["status"] for row in report["files"]], ["path_denied", "not_found"])


@acceptance_case
def vector_svg_export_states_its_verified_subset(case: NativeAcceptance) -> None:
    """Export a vector drawing to SVG and check the file against the stated subset (DRW-03 #4).

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document, target = _vector_fixture(case)
    for mode in ("outline", "centerline"):
        path = case.workspace / f"stroke-{mode}.svg"
        result = case.mutate(
            "vector.export_svg", document, **target, path=str(path), stroke_mode=mode
        )
        data = path.read_bytes()
        case.assertEqual(result["sha256"], hashlib.sha256(data).hexdigest())
        root = ElementTree.fromstring(data)
        paths = list(root.iter(f"{SVG}path"))
        case.assertEqual(len(paths), result["strokes_written"] + result["regions_written"])
        case.assertEqual(list(root.iter(f"{SVG}image")), [])
        case.assertFalse(result["visual_only"])
        subset = result["verified_subset"]
        case.assertEqual((subset["styles"], subset["opacity_envelopes"]), ("solid", "refused"))
        style = paths[-1].get("style", "")
        case.assertIn("stroke:rgb" if mode == "centerline" else "fill:rgb", style)
    existing = case.workspace / "stroke-outline.svg"
    params = {**target, "path": str(existing), "stroke_mode": "outline"}
    case.handler_failure("vector.export_svg", case.write_params(document, params), "already_exists")
    write = {**target, "sharing": "all_exposures", "stroke_indices": [0]}
    knots = [{"position": 0, "opacity": 0.2}, {"position": 1, "opacity": 0.9}]
    case.mutate("vector.opacity.profile.set", document, **write, knots=knots)
    envelope = {**params, "path": str(case.workspace / "envelope.svg")}
    error = case.handler_failure(
        "vector.export_svg", case.write_params(document, envelope), "unsupported_content"
    )
    case.assertEqual(error["details"]["reason"], "opacity_envelope")
    case.assertFalse((case.workspace / "envelope.svg").exists())
