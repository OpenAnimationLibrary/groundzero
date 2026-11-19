"""Qualify the media_decode scene check: every file decoded in an isolated helper."""

from __future__ import annotations

import os
import shutil
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_scene_access_cases import base_package, open_scene, wait
from mcp_studio_palette_cases import call, refuse

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

SEQUENCE_ID = 2200
MOVIE_ID = 2201
TOO_LARGE = 257 * 1024 * 1024
CRASH_MARKER = b"GROUNDZERO-TEST-CRASH"


def _level(identifier: int, name: str, path: str) -> str:
    return (
        f"<level id='{identifier}'>\n{name} \n"
        '<info dpix="120.000000" dpiy="120.000000"/>\n'
        f'<path> "{path}" </path>\n</level>\n'
    )


def _media_scene(case: NativeAcceptance) -> Path:
    """A scene whose PNG sequence has good, truncated, unreadable, crashing and oversized
    frames, beside a movie level."""
    base = base_package(case)
    folder = case.inputs / "media-corpus"
    shutil.copytree(base / "assets", folder / "assets")
    good = case.fixture.read_bytes()
    assets = folder / "assets"
    (assets / "seq.0001.png").write_bytes(good)
    (assets / "seq.0002.png").write_bytes(good[: len(good) // 2])
    (assets / "seq.0003.png").write_bytes(good)
    (assets / "seq.0004.png").write_bytes(CRASH_MARKER + good)
    with (assets / "seq.0005.png").open("wb") as large:
        large.truncate(TOO_LARGE)
    (assets / "seq.0006.png").write_bytes(good)
    (assets / "clip.mov").write_bytes(b"\0\0\0\x14ftypqt  ")
    os.chmod(assets / "seq.0003.png", 0)
    levels = _level(SEQUENCE_ID, "Seq", "$scenefolder/assets/seq..png") + _level(
        MOVIE_ID, "Clip", "$scenefolder/assets/clip.mov"
    )
    text = (base / "scene.tnz").read_text()
    text = text.replace("</levels>\n    <folder", levels + "</levels>\n    <folder", 1)
    path = folder / "media.tnz"
    path.write_text(text)
    return path


def _rows(case: NativeAcceptance, job: str) -> dict[str, Any]:
    result = call(case, case.worker, "job.result", {"job_id": job, "limit": 256})
    case.assertIsNone(result["next_offset"])
    return result


def _submit(case: NativeAcceptance, document: str, name: str, **extra: Any) -> dict[str, Any]:
    params = {
        "document_id": document,
        "expected_revision": "0",
        "operation_id": name,
        "check": "media_decode",
        **extra,
    }
    return call(case, case.worker, "scene.check.submit", params)


@acceptance_case
def media_decode_classifies_files(case: NativeAcceptance) -> None:
    """Corrupt, unreadable, unsupported, oversized and crashing files are each named,
    while the good ones decode and the worker and the scene host keep serving."""
    scene = _media_scene(case)
    try:
        document = open_scene(case, case.worker, scene, "standalone")["document_id"]
        job = _submit(case, document, "media-all")
        case.assertEqual(job["executor"], "worker")
        case.assertEqual(wait(case, case.worker, job["job_id"])["status"], "completed")
        result = _rows(case, job["job_id"])
    finally:
        os.chmod(scene.parent / "assets" / "seq.0003.png", 0o644)
    by_name = {Path(row["path"]).name: row for row in result["rows"]}
    expected = {
        "seq.0001.png": "decoded",
        "seq.0002.png": "corrupt",
        "seq.0003.png": "unreadable",
        "seq.0004.png": "crashed",
        "seq.0005.png": "too_large",
        "seq.0006.png": "decoded",
        "clip.mov": "unsupported_format",
    }
    for name, outcome in expected.items():
        case.assertEqual(by_name[name]["result"], outcome, by_name[name])
    for name in ("seq.0001.png", "seq.0002.png"):
        case.assertEqual(by_name[name]["role"], "frame")
    first = by_name["seq.0001.png"]
    case.assertEqual((first["width"], first["height"], first["frames_checked"]), (32, 32, 1))
    primary = [row for row in result["rows"] if row["role"] == "primary"]
    case.assertTrue(primary)
    case.assertTrue(all(row["result"] == "decoded" for row in primary if row["format"] != "mov"))
    case.assertIs(result["complete"], True)
    summary = call(case, case.worker, "scene.summary", {"document_id": document})
    case.assertEqual(summary["document_id"], document)
    status = call(case, case.worker, "job.status", {"job_id": job["job_id"]})
    # The batch helper crashes on frame 4; frames 4 and 6, which it did not
    # answer, are decoded again one per helper.
    case.assertGreaterEqual(status["analysis"]["cost"]["helpers_started"], 3)


@acceptance_case
def media_decode_budgets_and_refusals(case: NativeAcceptance) -> None:
    """A file budget ends the check incomplete with skipped rows; an editable document
    is refused before a job starts."""
    scene = _media_scene(case)
    os.chmod(scene.parent / "assets" / "seq.0003.png", 0o644)
    document = open_scene(case, case.worker, scene, "standalone")["document_id"]
    job = _submit(case, document, "media-budget", max_files=1)
    case.assertEqual(wait(case, case.worker, job["job_id"])["status"], "completed")
    result = _rows(case, job["job_id"])
    case.assertIs(result["complete"], False)
    case.assertIn("file_budget", result["reasons"])
    outcomes = [row["result"] for row in result["rows"]]
    case.assertEqual(outcomes.count("decoded"), 1, outcomes)
    case.assertIn("skipped_budget", outcomes)
    editable = case.create("Editable")
    details = refuse(
        case,
        case.worker,
        "scene.check.submit",
        {
            "document_id": editable,
            "expected_revision": case.revisions[editable],
            "operation_id": "media-editable",
            "check": "media_decode",
        },
        "invalid_params",
    )
    case.assertEqual(details["reason"], "editable_document")
