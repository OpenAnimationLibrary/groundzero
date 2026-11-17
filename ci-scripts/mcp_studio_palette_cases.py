"""Qualify shared roots and read-only studio palette discovery on the native worker."""

from __future__ import annotations

import hashlib
import os
import subprocess
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

PALETTE_ID = "1700000000_1234567890123456789"
SHARED_TOOLS = {
    "shared.roots",
    "studio_palette.list",
    "studio_palette.inspect",
    "studio_palette.find",
    "library.list",
    "library.inspect",
    "library.thumbnail",
}


def palette_text(palette_id: str | None, styles: list[str], extra: str = "") -> bytes:
    """Write the native studio palette envelope around TPalette::saveData content."""
    head = "<palette>" if palette_id is None else f'<palette name="{palette_id}">'
    indices = " ".join(str(index) for index in range(len(styles)))
    body = "".join(f"<style>{style}</style>" for style in styles)
    return (
        f"{head}\n<version>71 0</version><styles>{body}</styles><stylepages><page>"
        f"<name>colors</name><indices>{indices}</indices></page></stylepages>{extra}"
        "</palette>\n"
    ).encode()


def tree_digest(root: Path) -> str:
    """Hash every name, kind and byte under a root so any change is visible."""
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        digest.update(str(path.relative_to(root)).encode() + b"\0")
        digest.update(path.read_bytes() if path.is_file() else b"<dir>")
    return digest.hexdigest()


def shared_worker(
    case: NativeAcceptance, roots: dict[str, Path], write: tuple[str, ...] = ()
) -> Worker:
    """Start a second worker with explicit shared roots; it closes with the case."""
    worker = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / f"shared-{len(roots)}-{os.getpid()}.stderr.log",
        shared_roots=roots,
        shared_write=write,
    )
    case.addCleanup(worker.close)
    return worker


def call(case: NativeAcceptance, worker: Worker, method: str, params: dict[str, Any]) -> Any:
    response = worker.call(method, params)
    case.assertNotIn("error", response, f"{method}: {response}")
    return response["result"]


def refuse(
    case: NativeAcceptance, worker: Worker, method: str, params: dict[str, Any], code: str
) -> dict[str, Any]:
    response = worker.call(method, params)
    case.assertIn("error", response, f"{method} unexpectedly succeeded: {response}")
    case.assertEqual(response["error"]["code"], code, response)
    return response["error"]["details"]


def _launch_refused(case: NativeAcceptance, extra: list[str]) -> str:
    result = subprocess.run(
        [str(case.executable), "--automation-worker", "--workspace", str(case.workspace), *extra],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=60,
        check=False,
    )
    case.assertEqual(result.returncode, 2, extra)
    return result.stderr.decode("utf-8", errors="replace")


def shared_root_flags_are_explicit_and_read_only(case: NativeAcceptance) -> None:
    """Refuse unsafe grants at launch; granted roots stay read-only unless named."""
    studio = case.directory / "studio"
    library = case.directory / "library"
    (studio / "Global Palettes").mkdir(parents=True)
    (library / "templates").mkdir(parents=True)
    alias = case.directory / "alias"
    alias.symlink_to(studio, target_is_directory=True)
    inside = case.workspace / "inside"
    inside.mkdir()
    refusals = [
        ["--shared-root", "studio_palettes=relative"],
        ["--shared-root", f"studio_palettes={case.directory / 'missing'}"],
        ["--shared-root", f"studio_palettes={alias}"],
        ["--shared-root", f"studio_palettes={inside}"],
        ["--shared-root", f"elsewhere={studio}"],
        ["--shared-root", f"library={library}", "--shared-root", f"library={studio}"],
        ["--shared-root", f"library={library}", "--shared-root", f"project_palettes={library}"],
        ["--shared-write", "library"],
        ["--shared-root", f"library={library}", "--shared-write", "elsewhere"],
    ]
    for extra in refusals:
        with case.subTest(extra=extra):
            case.assertIn("shared", _launch_refused(case, extra).lower())
    tools = {tool["name"]: tool for tool in case.success("automation.capabilities", {})["tools"]}
    case.assertTrue(SHARED_TOOLS.issubset(tools))
    for name in SHARED_TOOLS:
        case.assertEqual(tools[name]["toolset"], "shared-library")
        case.assertTrue(tools[name]["annotations"]["readOnlyHint"])
    details = case.failure("library.list", {"kind": "template"})
    case.assertEqual(details["code"], "root_unavailable")
    worker = shared_worker(case, {"studio_palettes": studio, "library": library}, ("library",))
    rows = {row["root"]: row for row in call(case, worker, "shared.roots", {})["roots"]}
    case.assertEqual(list(rows), ["studio_palettes", "project_palettes", "library"])
    case.assertEqual(
        (rows["studio_palettes"]["writable"], rows["library"]["writable"]), (False, True)
    )
    case.assertFalse(rows["project_palettes"]["configured"])
    case.assertIsNone(rows["project_palettes"]["path"])
    case.assertEqual(rows["studio_palettes"]["layout"]["global_palettes_folder"], True)
    case.assertEqual(rows["library"]["layout"]["poses_folder"], False)


def _build_studio(studio: Path, project: Path) -> None:
    global_palettes = studio / "Global Palettes"
    (global_palettes / "Warm" / "Deep").mkdir(parents=True)
    (global_palettes / "Warm.tpl").write_bytes(
        palette_text(
            PALETTE_ID,
            [
                f'"|-{PALETTE_ID}-0" color_0 3 255 255 255 0',
                '"|+Other_9-1" "@Sky" sky 3 10 20 30 255',
                "line 2 1 2 3 255 0 1",
            ],
            "<lock>1</lock>",
        )
    )
    (global_palettes / "Warm" / "Plain.tpl").write_bytes(palette_text(None, ["color_0 3 0 0 0 0"]))
    frozen = global_palettes / "Warm" / "Deep" / "Frozen.tpl"
    frozen.write_bytes(palette_text("frozen_1", ["color_0 3 9 9 9 255"]))
    frozen.chmod(0o444)
    (global_palettes / "Notes.txt").write_text("not a palette")
    project.mkdir()
    (project / "Copy.tpl").write_bytes(palette_text(PALETTE_ID, ["color_0 3 0 0 0 0"]))


def _inspect_warm(case: NativeAcceptance, worker: Worker) -> None:
    params = {"root": "studio_palettes", "path": "Global Palettes/Warm.tpl", "limit": 2}
    first = call(case, worker, "studio_palette.inspect", params)
    case.assertEqual(first["palette_id"], PALETTE_ID)
    case.assertEqual((first["locked"], first["all_styles_solid"]), (True, False))
    case.assertEqual(first["style_count"], 3)
    case.assertEqual(first["next_offset"], 2)
    links = [style["link"] for style in first["styles"]]
    case.assertEqual(links[0]["state"], "source")
    case.assertEqual(
        (links[1]["state"], links[1]["palette_id"], links[1]["source_style_id"]),
        ("on", "Other_9", 1),
    )
    case.assertEqual(links[1]["original_name"], "Sky")
    rest = call(case, worker, "studio_palette.inspect", {**params, "offset": 2})
    case.assertEqual(rest["styles"][0]["solid"], False)
    case.assertIsNone(rest["next_offset"])
    stale = {**params, "expected_file_revision": "sha256:" + "0" * 64}
    details = refuse(case, worker, "studio_palette.inspect", stale, "revision_conflict")
    case.assertEqual(details["file_revision"], first["file_revision"])
    frozen = {"root": "studio_palettes", "path": "Global Palettes/Warm/Deep/Frozen.tpl"}
    case.assertTrue(call(case, worker, "studio_palette.inspect", frozen)["read_only"])


def studio_palettes_list_inspect_find_preserve_roots(case: NativeAcceptance) -> None:
    """Browse a studio tree with identities, links and duplicates without changing it."""
    studio, project = case.directory / "studio", case.directory / "project"
    _build_studio(studio, project)
    before = (tree_digest(studio), tree_digest(project))
    worker = shared_worker(case, {"studio_palettes": studio, "project_palettes": project})
    listing = call(
        case,
        worker,
        "studio_palette.list",
        {"root": "studio_palettes", "folder": "Global Palettes"},
    )
    case.assertEqual([entry["name"] for entry in listing["entries"]], ["Warm", "Warm.tpl"])
    case.assertEqual(listing["entries"][1]["palette_id"], PALETTE_ID)
    case.assertEqual(listing["skipped"]["other_files"], 1)
    nested = call(
        case,
        worker,
        "studio_palette.list",
        {"root": "studio_palettes", "folder": "Global Palettes/Warm"},
    )
    case.assertEqual(nested["entries"][1]["id_status"], "missing")
    pinned = {
        "root": "studio_palettes",
        "folder": "Global Palettes",
        "expected_listing_revision": listing["listing_revision"],
    }
    case.assertEqual(call(case, worker, "studio_palette.list", pinned)["total"], 2)
    _inspect_warm(case, worker)
    found = call(case, worker, "studio_palette.find", {"palette_id": PALETTE_ID})
    case.assertEqual(
        sorted((match["root"], match["path"]) for match in found["matches"]),
        [("project_palettes", "Copy.tpl"), ("studio_palettes", "Global Palettes/Warm.tpl")],
    )
    case.assertFalse(found["unique"])
    case.assertEqual(found["completeness"]["status"], "complete")
    unique = call(case, worker, "studio_palette.find", {"palette_id": "frozen_1"})
    case.assertTrue(unique["unique"])
    refuse(
        case,
        worker,
        "studio_palette.list",
        {"root": "studio_palettes", "folder": "../project"},
        "invalid_path",
    )
    case.assertEqual((tree_digest(studio), tree_digest(project)), before)
