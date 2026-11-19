"""Qualify project creation, duplication and explicit project binding of drafts."""

from __future__ import annotations

import os
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

ALPHANUMERIC = {
    "use_standard": False,
    "accept_non_alphabet_suffix": True,
    "letter_count_for_suffix": 4,
    "no_separator_format_allowed": False,
}
SCENE_DEPENDENT = {"alias": "extras", "path": "$scenefolder/extras", "use_scene_path": True}


def _folders(drawings: str) -> list[dict[str, Any]]:
    return [
        {"alias": "drawings", "path": drawings},
        {"alias": "scenes", "path": "scenes"},
        {"alias": "outputs", "path": "outputs"},
        SCENE_DEPENDENT,
    ]


def _create(case: NativeAcceptance, name: str, drawings: str, fps: float) -> dict[str, Any]:
    return case.mutate(
        "project.create",
        None,
        directory=str(case.workspace / "projects" / name),
        folders=_folders(drawings),
        file_path_rules=ALPHANUMERIC,
        scene_defaults={
            "frame_rate": fps,
            "camera_width_px": 640,
            "camera_height_px": 360,
            "camera_width_in": 8,
            "camera_height_in": 4.5,
        },
    )


def _inspect(case: NativeAcceptance, project: str) -> dict[str, Any]:
    return case.success("project.inspect", {"project_id": project})


def _folder(record: dict[str, Any], alias: str) -> dict[str, Any]:
    return next(folder for folder in record["folders"] if folder["alias"] == alias)


@acceptance_case
def project_create_verifies_and_reloads(case: NativeAcceptance) -> None:
    """A project made from explicit settings reloads with its aliases, alphanumeric rules
    and scene-dependent folder, and leaves another project unchanged; names never
    collide."""
    (case.workspace / "projects").mkdir()
    first = _create(case, "ShowA", "art", 25)
    second = _create(case, "ShowB", "drawings", 12)
    reread = _inspect(case, first["project_id"])
    case.assertEqual(reread["file_path_rules"], ALPHANUMERIC)
    case.assertEqual(reread["scene_defaults"]["frame_rate"], 25)
    drawings = _folder(reread, "drawings")
    case.assertEqual(drawings["authority"], "workspace")
    case.assertTrue(drawings["resolved_path"].endswith("projects/ShowA/art"), drawings)
    case.assertTrue(drawings["exists"])
    case.assertTrue(_folder(reread, "extras")["scene_dependent"])
    listed = case.success("project.list", {"limit": 64})
    case.assertIn(first["project_id"], {row["project_id"] for row in listed["projects"]})
    case.assertEqual(_inspect(case, second["project_id"])["file_revision"], second["file_revision"])
    taken = case.failure(
        "project.create",
        case.write_params(None, {"directory": str(case.workspace / "projects" / "ShowA")}),
    )
    case.assertEqual(taken["code"], "already_exists", taken)
    (case.workspace / "elsewhere").mkdir()
    named = case.failure(
        "project.create",
        case.write_params(None, {"directory": str(case.workspace / "elsewhere" / "showa")}),
    )
    case.assertEqual(named["code"], "name_conflict", named)
    case.assertFalse((case.workspace / "elsewhere" / "showa").exists())


@acceptance_case
def project_create_publishes_nothing_on_failure(case: NativeAcceptance) -> None:
    """A parent folder that refuses writes ends the creation before anything appears."""
    locked = case.workspace / "locked"
    locked.mkdir()
    os.chmod(locked, 0o555)
    try:
        error = case.failure(
            "project.create", case.write_params(None, {"directory": str(locked / "Show")})
        )
    finally:
        os.chmod(locked, 0o755)
    case.assertIs(error["details"].get("published"), False, error)
    case.assertEqual(list(locked.iterdir()), [])


@acceptance_case
def project_duplicate_copies_the_definition_only(case: NativeAcceptance) -> None:
    """A duplicate carries folders, rules and defaults but no asset, and names the
    folders that still point outside it."""
    (case.workspace / "projects").mkdir()
    source = _create(case, "Source", "art", 24)
    art = case.workspace / "projects" / "Source" / "art"
    (art / "hero.png").write_bytes(case.fixture.read_bytes())
    copy = case.mutate(
        "project.duplicate",
        None,
        project_id=source["project_id"],
        directory=str(case.workspace / "projects" / "Copy"),
    )
    case.assertEqual(copy["source_project_id"], source["project_id"])
    case.assertIs(copy["assets_copied"], False)
    case.assertEqual(copy["shared_folders"], [])
    case.assertEqual(copy["file_path_rules"], source["file_path_rules"])
    case.assertEqual(copy["scene_defaults"], source["scene_defaults"])
    copied_art = case.workspace / "projects" / "Copy" / "art"
    case.assertTrue(copied_art.is_dir())
    # TProject::save leaves a scenes.xml pointer in each folder it creates; the
    # source's assets stay where they are.
    case.assertEqual([path.name for path in copied_art.iterdir()], ["scenes.xml"])


@acceptance_case
def drafts_bind_explicit_projects(case: NativeAcceptance) -> None:
    """Two drafts bound to two projects at once take each project's defaults unless
    told otherwise, report their binding, and an edit of one changes neither the
    other, the project files nor the worker's private project."""
    (case.workspace / "projects").mkdir()
    show_a = _create(case, "DraftA", "art", 25)
    show_b = _create(case, "DraftB", "drawings", 12)
    private = case.success("project.inspect", {})
    first = case.mutate("scene.create", None, name="A", project_id=show_a["project_id"])
    second = case.mutate(
        "scene.create", None, name="B", project_id=show_b["project_id"], frame_rate=30
    )
    case.assertEqual(first["frame_rate"], 25)
    case.assertEqual(first["camera"]["width_px"], 640)
    case.assertEqual(second["frame_rate"], 30)
    case.assertEqual(first["project"]["project_id"], show_a["project_id"])
    case.assertIs(first["project"]["file_path_rules_applied"], False)
    case.assertEqual(second["project"]["name"], "DraftB")
    case.mutate("scene.settings", first["document_id"], frame_rate=48)
    summary = case.success("scene.summary", {"document_id": second["document_id"]})
    case.assertEqual(summary["frame_rate"], 30)
    case.assertEqual(summary["project"], second["project"])
    for project in (show_a, show_b):
        case.assertEqual(
            _inspect(case, project["project_id"])["file_revision"], project["file_revision"]
        )
    case.assertEqual(case.success("project.inspect", {})["mode"], private["mode"])
    level = case.mutate("level.create", first["document_id"], name="Ink", format="vector")
    case.mutate(
        "drawing.create",
        first["document_id"],
        level_id=level["level_id"],
        drawing_id={"number": 1, "suffix": ""},
        format="vector",
    )
    case.mutate("scene.save_copy", first["document_id"], directory=str(case.workspace / "draft-a"))
    art = case.workspace / "projects" / "DraftA" / "art"
    case.assertEqual([path.name for path in art.iterdir()], ["scenes.xml"])
    plain = case.mutate("scene.create", None, name="Plain")
    case.assertIsNone(plain["project"])
    unknown = case.failure(
        "scene.create", case.write_params(None, {"name": "Lost", "project_id": "project_x"})
    )
    case.assertEqual(unknown["code"], "not_found", unknown)
