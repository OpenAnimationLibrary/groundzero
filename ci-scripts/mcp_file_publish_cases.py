"""Publish outputs when the exclusive rename is refused or another party takes the name."""

from __future__ import annotations

import ctypes
import os
import platform
import uuid
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_file_matrix import attach_volume
from mcp_file_tools import PATH_TOOLS, PathTool
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

OTHER_PARTY = b"other party\n"
REFUSED = "publication_unsupported"
STAGING = (
    "package-*",
    "mixdown-*",
    "preset-*",
    "export-*.png",
    "import-*.png",
    "macro-verify-*",
    "preset-check-*",
    "preset-apply-*",
)
# name, faults, whether a file output is published, whether a folder output is
SCENARIOS = (
    ("another party takes the name", ["racing_destination"], "taken", "taken"),
    (
        "another party takes the name, no exclusive rename",
        ["racing_destination", "exclusive_rename_refused"],
        "taken",
        "refused",
    ),
    ("no exclusive rename", ["exclusive_rename_refused"], "published", "refused"),
    (
        "no atomic primitive",
        ["exclusive_rename_refused", "hard_link_refused"],
        "refused",
        "refused",
    ),
)


def staging_left(workspace: Path) -> list[Path]:
    """Return the scratch and staging entries the code creates and a worker must remove."""
    found: list[Path] = []
    for runtime in workspace.glob("runtime_*"):
        for pattern in STAGING:
            found.extend(runtime.rglob(pattern))
    return found


def _worker(case: NativeAcceptance, faults: list[str] | None) -> None:
    """Start a worker on the case's workspace, with test faults unless faults is None."""
    worker = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / f"publish-{uuid.uuid4().hex[:6]}.log",
        test_arguments=() if faults is None else ("--test-publish-fault", ",".join(faults)),
    )
    case.addCleanup(worker.close)
    case.worker = worker


def _destination(case: NativeAcceptance, tool: PathTool) -> Path:
    return case.workspace / f"out-{uuid.uuid4().hex[:6]}{tool.suffix}"


def _check_taken(
    case: NativeAcceptance, tool: PathTool, destination: Path, response: dict[str, Any]
) -> None:
    case.assertIn("error", response, f"{tool.name} replaced a destination: {response}")
    if tool.suffix:
        case.assertEqual(destination.read_bytes(), OTHER_PARTY)
    else:
        case.assertEqual(list(destination.iterdir()), [])


def _check_refused(case: NativeAcceptance, destination: Path, response: dict[str, Any]) -> None:
    case.assertEqual(response.get("error", {}).get("code"), REFUSED, response)
    case.assertIn("file system", response["error"]["message"])
    case.assertFalse(destination.exists())


def _check_published(case: NativeAcceptance, tool: PathTool, destination: Path) -> None:
    case.assertTrue(destination.is_file(), f"{tool.name} published nothing")
    case.assertGreater(destination.stat().st_size, 100)
    case.assertNotEqual(destination.read_bytes(), OTHER_PARTY)


def _one(case: NativeAcceptance, tool: PathTool, faults: list[str], outcome: str) -> None:
    _worker(case, faults)
    state = tool.prepare(case)
    destination = _destination(case, tool)
    response = tool.drive(case, state, str(destination))
    if outcome == "taken":
        _check_taken(case, tool, destination, response)
    elif outcome == "refused":
        _check_refused(case, destination, response)
    else:
        case.assertNotIn("error", response, response)
        _check_published(case, tool, destination)
    case.assertEqual(staging_left(case.workspace), [], "staging was left behind")


def publication_never_replaces_and_refuses_without_an_atomic_primitive(
    case: NativeAcceptance,
) -> None:
    """Never replace a name another party created, whichever primitive publishes.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    for tool in PATH_TOOLS.values():
        if tool.mode != "write":
            continue
        for name, faults, files, folders in SCENARIOS:
            with case.subTest(tool=tool.name, scenario=name):
                _one(case, tool, faults, files if tool.suffix else folders)


def _supports_atomic_create(mount: Path) -> bool:
    """Whether the file system takes an exclusive rename or a hard link, as the kernel says."""
    libc = ctypes.CDLL(None, use_errno=True)
    (mount / "probe-a").write_bytes(b"a")
    (mount / "probe-b").write_bytes(b"b")
    exclusive = libc.renameatx_np(-2, str(mount / "probe-a").encode(), -2, b"probe-c", 4)
    try:
        os.link(mount / "probe-b", mount / "probe-e")
    except OSError:
        return exclusive == 0
    return True


def publication_on_other_file_systems_is_atomic_or_refused(case: NativeAcceptance) -> None:
    """Publish where the file system can create a name atomically; refuse where it cannot.

    The expectation comes from asking the kernel on the same volume, so a file system that
    supports neither primitive must give the typed refusal and nothing at the destination.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    if platform.system() != "Darwin":
        case.skipTest("the disk images and the exclusive-rename probe are macOS tools")
    tried: list[str] = []
    for index, file_system in enumerate(("HFS+", "ExFAT", "MS-DOS")):
        volume, why = attach_volume(case, file_system, f"fs{index}")
        if volume is None:
            tried.append(f"{file_system}: not available ({why})")
            continue
        workspace = volume / "workspace"
        workspace.mkdir()
        expected = "published" if _supports_atomic_create(volume) else "refused"
        case.workspace = workspace
        for name in ("raster.export_png", "audio.mixdown", "scene.save_copy"):
            with case.subTest(file_system=file_system, tool=name):
                outcome = _outcome(case, PATH_TOOLS[name], name != "scene.save_copy")
                if name != "scene.save_copy":
                    case.assertEqual(outcome, expected)
                tried.append(f"{file_system} {name}: {outcome}")
    print("file systems tried:\n  " + "\n  ".join(tried))


def _outcome(case: NativeAcceptance, tool: PathTool, typed: bool) -> str:
    """Publish once; a refusal must leave no staging folder, even with AppleDouble files."""
    _worker(case, None)
    state = tool.prepare(case)
    destination = _destination(case, tool)
    response = tool.drive(case, state, str(destination))
    case.assertEqual(staging_left(case.workspace), [], "a refusal left staging behind")
    if "error" not in response:
        case.assertTrue(destination.exists())
        return "published"
    if typed:
        case.assertEqual(response["error"]["code"], REFUSED, response)
    case.assertFalse(destination.exists())
    return "refused"
