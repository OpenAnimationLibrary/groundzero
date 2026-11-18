"""Change or replace files that a running job has pinned, and require the job to ignore it."""

from __future__ import annotations

import base64
import os
import shutil
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_png import decode_png, fixture_pixel, write_fixture

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

SIZE = 256
FRAMES = 120
RUNNING = {"queued", "preparing", "running", "finalizing"}


def _mutations(case: NativeAcceptance, source: Path) -> list[tuple[str, Any]]:
    outside = case.directory / "outside-source.png"
    write_fixture(outside, size=64)
    return [
        ("overwritten with another image", lambda: write_fixture(source, size=64)),
        ("truncated to nothing", lambda: source.write_bytes(b"")),
        ("replaced by a link outside", lambda: (source.unlink(), os.symlink(outside, source))),
        ("deleted", lambda: source.unlink(missing_ok=True)),
        ("replaced by a folder", lambda: source.mkdir()),
    ]


def _check_pixels(case: NativeAcceptance, artifact: dict[str, Any]) -> None:
    result = case.success("artifact.read", {"artifact_id": artifact["artifact_id"]})
    data = base64.b64decode(result["image"]["data"], validate=True)
    width, height, pixels = decode_png(data)
    case.assertEqual((width, height), (SIZE, SIZE))
    scale = SIZE // 32
    for x, y in ((8, 8), (24, 8), (8, 24), (24, 24)):
        expected = fixture_pixel(x, y, 32)
        case.assertEqual(pixels[(y * scale) * width + x * scale], expected, (x, y))


def linked_input_changes_while_a_render_runs(case: NativeAcceptance) -> None:
    """Keep a running render on the bytes it pinned however its source file is changed.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document, _level, _column = case.populated_scene()
    job = case.mutate(
        "render.submit",
        document,
        start_frame=0,
        end_frame=FRAMES - 1,
        step=1,
        width=SIZE,
        height=SIZE,
    )
    for label, change in _mutations(case, case.fixture):
        status = case.success("render.status", {"job_id": job["job_id"]})
        with case.subTest(change=label):
            case.assertIn(status["status"], RUNNING | {"completed"}, status)
            change()
    final = case.wait_job(job["job_id"])
    case.assertEqual(final["status"], "completed", final)
    case.assertEqual(final["failed_frames"], 0, final)
    artifacts = case.success("render.artifacts", {"job_id": job["job_id"]})["artifacts"]
    case.assertEqual(len(artifacts), FRAMES)
    _check_pixels(case, artifacts[0])


def import_keeps_its_own_copy_when_the_source_changes(case: NativeAcceptance) -> None:
    """Render from the private copy an import made, not from the file it came from.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document, _level, _column = case.populated_scene()
    write_fixture(case.fixture, size=64)
    job = case.mutate("render.preview", document, frame=0, width=SIZE, height=SIZE)
    case.assertEqual(case.wait_job(job["job_id"])["status"], "completed")
    artifact = case.success("render.artifacts", {"job_id": job["job_id"]})["artifacts"][0]
    _check_pixels(case, artifact)


def artifact_swapped_for_a_link_is_refused(case: NativeAcceptance) -> None:
    """Refuse an artifact whose file was replaced by a link to an identical copy elsewhere.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Artifact link")
    job = case.mutate("render.preview", document, frame=0, width=8, height=8)
    case.assertEqual(case.wait_job(job["job_id"])["status"], "completed")
    artifact = case.success("render.artifacts", {"job_id": job["job_id"]})["artifacts"][0]
    path = case.workspace / "renders" / job["job_id"] / "frames" / "frame.0001.png"
    copy = case.directory / "identical-copy.png"
    shutil.copyfile(path, copy)
    path.unlink()
    os.symlink(copy, path)
    error = case.failure("artifact.read", {"artifact_id": artifact["artifact_id"]})
    case.assertIn(error["code"], {"invalid_artifact", "path_denied"}, error)
