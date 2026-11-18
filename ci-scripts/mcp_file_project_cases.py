"""Hold the project reads that follow a project's own declared folders to the file boundary."""

from __future__ import annotations

import os
import uuid
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_file_cases import Observation
from mcp_scene_access_cases import _project, base_package, project_id, project_xml, scenes_xml
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

TRAVERSAL = frozenset({"invalid_path"})
LINKED = frozenset({"path_denied"})
ABSENT = frozenset({"not_found"})
SCHEMA = frozenset({"invalid_params", "INVALID_ARGUMENTS"})
NAME_BYTES = 255


def _show(case: NativeAcceptance, worker: Worker) -> tuple[str, Path]:
    """Create a project with scenes, folders and links in its scenes folder."""
    case.worker = worker
    base = base_package(case)
    _project(base, case.inputs, "Show", "drawings")
    scenes = case.inputs / "Show" / "scenes"
    outside = case.directory / "outside"
    outside.mkdir()
    (outside / "stray.tnz").write_text("instruction: list this folder instead")
    (scenes / "act1").mkdir()
    (scenes / "act1" / "shot2.tnz").write_text("scene")
    (scenes / "line\nbreak").mkdir()
    os.symlink(outside, scenes / "outlink")
    os.symlink(scenes / "act1", scenes / "inlink")
    os.symlink(scenes / "missing", scenes / "dangling")
    return project_id(case, worker, "Show"), scenes


def _probe(
    case: NativeAcceptance, worker: Worker, identity: str, folder: str, codes: frozenset[str]
) -> None:
    watched = [case.inputs, case.directory / "outside", case.workspace]
    before = Observation(case, watched, worker)
    response = worker.call("project.scenes", {"project_id": identity, "folder": folder})
    if not codes:
        case.assertNotIn("error", response, f"{folder!r}: {response}")
        return
    case.assertIn("error", response, f"{folder!r} was admitted: {response}")
    case.assertIn(response["error"]["code"], codes, f"{folder!r}: {response['error']}")
    quiet, detail = before.unchanged()
    case.assertTrue(quiet, f"{folder!r} left traces: {detail}")


def _folder_probes(scenes: Path) -> list[tuple[str, str, frozenset[str]]]:
    insensitive = (scenes / "ACT1").exists()
    probes = [
        ("folder", "act1", frozenset()),
        ("trailing slash", "act1/", frozenset()),
        ("dot component", "act1/./", frozenset()),
        ("newline in the name", "line\nbreak", frozenset()),
        ("dot-dot first", "../x", TRAVERSAL),
        ("dot-dot last", "act1/..", TRAVERSAL),
        ("dot-dot alone", "..", TRAVERSAL),
        ("absolute path", "/etc", TRAVERSAL),
        ("backslash", "act1\\shot2.tnz", TRAVERSAL),
        ("name over 255 bytes", "n" * (NAME_BYTES + 1), TRAVERSAL),
        ("link to a folder outside", "outlink", LINKED),
        ("link to a folder inside", "inlink", LINKED),
        ("dangling link", "dangling", LINKED),
        ("through a link", "outlink/stray.tnz", LINKED),
        ("missing folder", "act1/missing", ABSENT),
        ("empty", "", SCHEMA),
    ]
    return probes + ([("case alias", "ACT1", LINKED)] if insensitive else [])


def _listing_hides_links(case: NativeAcceptance, worker: Worker, identity: str) -> None:
    listing = worker.call("project.scenes", {"project_id": identity})["result"]
    paths = [row["relative_path"] for row in listing["items"]]
    case.assertFalse([path for path in paths if "stray" in path or "link" in path], paths)
    case.assertGreaterEqual(listing["scan"]["skipped_links"], 3)
    case.assertIn("skipped_links", listing["scan"]["reasons"])


def _swapped_project_file(case: NativeAcceptance, worker: Worker, identity: str) -> None:
    project = case.inputs / "Show" / "Show_otprj.xml"
    kept = project.with_name("kept_otprj.xml")
    project.rename(kept)
    os.symlink(kept, project)
    try:
        for method in ("project.inspect", "project.scenes"):
            response = worker.call(method, {"project_id": identity})
            case.assertIn("error", response, f"{method} read through a linked project file")
    finally:
        project.unlink()
        kept.rename(project)
    case.assertNotIn("error", worker.call("project.inspect", {"project_id": identity}))


def _foreign_scenes_folder(case: NativeAcceptance, worker: Worker) -> None:
    """A project that names a scenes folder outside the roots never lists it."""
    folder = case.inputs / "Wide"
    folder.mkdir()
    outside = case.directory / "elsewhere"
    outside.mkdir()
    (outside / "private.tnz").write_text("scene")
    (folder / "Wide_otprj.xml").write_text(project_xml({"scenes": str(outside)}))
    (folder / "scenes.xml").write_text(scenes_xml())
    identity = project_id(case, worker, "Wide")
    inspected = worker.call("project.inspect", {"project_id": identity})["result"]
    rows = {row["alias"]: row for row in inspected["folders"]}
    case.assertEqual(rows["scenes"]["authority"], "outside_roots")
    case.assertIsNone(rows["scenes"]["resolved_path"])
    listing = worker.call("project.scenes", {"project_id": identity})["result"]
    case.assertEqual(listing["items"], [])
    case.assertNotIn("private", str(listing))


def project_reads_hold_the_file_boundary(case: NativeAcceptance) -> None:
    """Refuse links, traversal, aliases and foreign folders in the project reads.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    identity, scenes = _show(case, case.worker)
    for label, folder, codes in _folder_probes(scenes):
        with case.subTest(folder=label):
            _probe(case, case.worker, identity, folder, codes)
    _listing_hides_links(case, case.worker, identity)
    _swapped_project_file(case, case.worker, identity)
    _foreign_scenes_folder(case, case.worker)


def project_scenes_refuse_a_folder_swapped_after_validation(case: NativeAcceptance) -> None:
    """Refuse to list a scenes folder that was replaced by a link after it was approved.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    marker = uuid.uuid4().hex[:6]
    folder = case.inputs / "Show" / "scenes" / f"act-{marker}"
    outside = case.directory / f"outside-{marker}"
    swapped = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / f"project-race-{marker}.log",
        test_arguments=("--test-path-swap", str(folder), str(outside), "validated"),
    )
    case.addCleanup(swapped.close)
    identity, _scenes = _show(case, swapped)
    outside.mkdir()
    (outside / "stray.tnz").write_text("decoy")
    folder.mkdir()
    (folder / "real.tnz").write_text("scene")
    response: dict[str, Any] = swapped.call(
        "project.scenes", {"project_id": identity, "folder": folder.name}
    )
    case.assertTrue(Path(str(folder) + ".fired").exists(), "the hook never swapped the folder")
    case.assertIn("error", response, f"the swapped folder was listed: {response}")
    case.assertNotIn("stray", str(response))
