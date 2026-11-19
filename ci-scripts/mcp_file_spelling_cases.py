"""Hold the file boundary to the file system's own answer on how a name is spelled.

A name can be stored composed (one code point for an accented letter) or decomposed (the letter
and a combining mark). A volume that treats the two as one name resolves either spelling to the
same entry, and so must the worker: it reports one spelling for the entry and opens it by both.
Nothing else is admitted along the way: a case variant, a link under either spelling and a path
outside the roots stay refused.
"""

from __future__ import annotations

import os
import unicodedata
from pathlib import Path
from typing import TYPE_CHECKING, Any, Literal

from mcp_case import acceptance_case
from mcp_file_tools import invoke
from mcp_png import write_fixture
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

COMPOSED = "Caf\u00e9"
DECOMPOSED = "Cafe\u0301"
Form = Literal["NFC", "NFD"]
FORMS: tuple[Form, Form] = ("NFD", "NFC")


def _spell(path: Path, form: Form) -> str:
    return unicodedata.normalize(form, str(path))


def _stored(case: NativeAcceptance, parent: Path, name: str) -> Path:
    """Create a directory under exactly this name, or skip where the volume respells it."""
    path = parent / name
    path.mkdir()
    if name not in os.listdir(parent):
        case.skipTest("this volume does not keep a name as it was given")
    other: Form = "NFC" if name == unicodedata.normalize("NFD", name) else "NFD"
    if not os.path.exists(_spell(path, other)):
        case.skipTest("this volume keeps different spellings as different names")
    return path


def _imports(case: NativeAcceptance, document: str, path: str) -> dict[str, Any]:
    name = f"Level{case.operations + 1}"
    return invoke(case, "level.import", document, path=path, name=name, dpi=120)


def _opens_as_saved(case: NativeAcceptance, path: str, reported: str) -> None:
    """Open the package; the worker names it as the save did, whichever spelling was given."""
    opened = case.mutate("scene.open", None, path=path)
    case.assertEqual(opened["package_directory"], reported)
    summary = case.success("scene.summary", {"document_id": opened["document_id"]})
    case.assertEqual([item["name"] for item in summary["levels"]], ["Quadrants"], path)
    case.mutate("scene.discard", opened["document_id"])


def _saves_and_reopens(case: NativeAcceptance, target: Path, spelled: str) -> None:
    """Save a package at the spelled path; open it by the reported path and by both spellings."""
    document, _level, _column = case.populated_scene()
    saved = case.mutate("scene.save_copy", document, directory=spelled)
    reported = saved["package_directory"]
    case.assertTrue((target / "scene.tnz").is_file(), spelled)
    case.assertTrue(os.path.samefile(reported, target), reported)
    for path in (reported, _spell(target, "NFD"), _spell(target, "NFC")):
        with case.subTest(opened=path.encode("unicode_escape").decode()):
            _opens_as_saved(case, path, reported)
    again = invoke(case, "scene.save_copy", document, directory=_spell(target, "NFC"))
    case.assertEqual(again["error"]["code"], "already_exists", again)
    again = invoke(case, "scene.save_copy", document, directory=_spell(target, "NFD"))
    case.assertEqual(again["error"]["code"], "already_exists", again)
    case.mutate("scene.discard", document)


@acceptance_case
def names_inside_a_root_resolve_in_either_spelling(case: NativeAcceptance) -> None:
    """Read and write below stored-decomposed and stored-composed names by both spellings.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    for stored in (DECOMPOSED, f"{COMPOSED}-composed"):
        folder = _stored(case, case.inputs, f"in-{stored}")
        write_fixture(folder / f"{stored}.png")
        document = case.create("Spelling")
        for form in FORMS:
            with case.subTest(stored=stored.encode("unicode_escape").decode(), given=form):
                imported = _imports(case, document, _spell(folder / f"{stored}.png", form))
                case.assertIn("result", imported, imported)
        case.mutate("scene.discard", document)
        home = _stored(case, case.workspace, f"out-{stored}")
        _saves_and_reopens(case, home / "kept", str(home / "kept"))
        for index, form in enumerate(FORMS):
            target = home / f"{index}-{DECOMPOSED}"
            _saves_and_reopens(case, target, _spell(target, form))


@acceptance_case
def a_respelled_name_admits_nothing_the_stored_name_refuses(case: NativeAcceptance) -> None:
    """Refuse a link, a case variant and an outside folder under either spelling of a name.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    folder = _stored(case, case.inputs, f"in-{DECOMPOSED}")
    write_fixture(folder / "art.png")
    outside = _stored(case, case.directory, f"outside-{DECOMPOSED}")
    write_fixture(outside / "art.png")
    sibling = _stored(case, case.directory, f"{case.inputs.name}{DECOMPOSED}")
    write_fixture(sibling / "art.png")
    os.symlink(outside, case.inputs / f"link-{DECOMPOSED}")
    os.symlink(folder / "art.png", folder / f"alias-{DECOMPOSED}.png")
    refused = [
        ("folder link to the outside", case.inputs / f"link-{DECOMPOSED}" / "art.png"),
        ("file link inside the root", folder / f"alias-{DECOMPOSED}.png"),
        ("folder outside the roots", outside / "art.png"),
        ("sibling that starts with the root's name", sibling / "art.png"),
    ]
    shouted = folder.parent / folder.name.upper() / "art.png"
    if shouted.exists():
        refused.append(("case variant of a decomposed name", shouted))
    document = case.create("Refusals")
    for label, path in refused:
        for form in FORMS:
            with case.subTest(refused=label, given=form):
                answer = _imports(case, document, _spell(path, form))
                case.assertEqual(answer.get("error", {}).get("code"), "path_denied", answer)
    case.mutate("scene.discard", document)


@acceptance_case
def a_decomposed_root_name_serves_every_spelling(case: NativeAcceptance) -> None:
    """Grant roots below a decomposed folder name and use them by both spellings.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    home = _stored(case, case.directory, f"studio-{DECOMPOSED}")
    workspace = _stored(case, home, f"work-{DECOMPOSED}")
    inputs = _stored(case, home, f"art-{DECOMPOSED}")
    write_fixture(inputs / "quadrants.png")
    worker = Worker(case.executable, workspace, inputs, case.directory / "decomposed-root.log")
    case.addCleanup(worker.close)
    case.worker = worker
    case.fixture = inputs / "quadrants.png"
    document = case.create("Rooted")
    for form in FORMS:
        with case.subTest(given=form):
            imported = _imports(case, document, _spell(inputs / "quadrants.png", form))
            case.assertIn("result", imported, imported)
            outside = _imports(case, document, _spell(case.inputs / "quadrants.png", form))
            case.assertEqual(outside.get("error", {}).get("code"), "path_denied", outside)
    case.mutate("scene.discard", document)
    _saves_and_reopens(case, workspace / "relative", "relative")
    for index, form in enumerate(FORMS):
        target = workspace / f"{index}-{DECOMPOSED}"
        _saves_and_reopens(case, target, _spell(target, form))
