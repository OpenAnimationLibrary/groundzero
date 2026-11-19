"""Hold every launch option that names a root to the file boundary."""

from __future__ import annotations

import os
import re
import subprocess
import unicodedata
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_file_matrix import attach_volume
from mcp_file_tools import invoke
from mcp_png import write_fixture
from mcp_worker import FULL_AUTHORITY, Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

ROOT_OPTIONS = {"--workspace", "--input-root", "--media-tools", "--shared-root"}
USAGE_OPTION = re.compile(r"(--[a-z-]+) (?:ROLE=)?DIR")


def _launch(case: NativeAcceptance, arguments: list[str]) -> tuple[int, str]:
    """Start the worker or its adapter with exactly these grants, then close its input."""
    result = subprocess.run(
        [str(case.executable), "--automation-worker", *arguments, *FULL_AUTHORITY],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=60,
        check=False,
    )
    return result.returncode, result.stderr.decode("utf-8", errors="replace")


def _refused(case: NativeAcceptance, label: str, arguments: list[str], mention: str) -> None:
    """Require a refusal before the protocol starts that names what it refused."""
    code, diagnostics = _launch(case, arguments)
    expected = 1 if case.through_sdk else 2
    case.assertEqual(code, expected, f"{label}: {diagnostics}")
    option = [item for item in arguments if item.startswith("--")][-1]
    named = mention in diagnostics.lower() or option in diagnostics
    case.assertTrue(named, f"{label}: the refusal does not say what it refused: {diagnostics}")


@acceptance_case
def launch_options_naming_a_root_are_all_qualified(case: NativeAcceptance) -> None:
    """Fail when the worker accepts a directory option these cases do not cover.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    refused = subprocess.run(
        [str(case.native_executable), "--automation-worker", "--no-such-option"],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=60,
        check=False,
    )
    usage = refused.stderr.decode("utf-8", errors="replace")
    found = set(USAGE_OPTION.findall(usage))
    case.assertTrue(found, f"the worker usage names no directory option: {usage!r}")
    unqualified = sorted(found - ROOT_OPTIONS)
    case.assertEqual(
        unqualified,
        [],
        f"{unqualified} now names a directory at launch: add its boundary cases to "
        "ci-scripts/mcp_file_launch_cases.py and its name to ROOT_OPTIONS",
    )


def _bad_roots(case: NativeAcceptance) -> list[tuple[str, str, str]]:
    base = case.directory / "launch"
    base.mkdir()
    (base / "plain.txt").write_text("not a directory")
    (base / "real").mkdir()
    os.symlink(base / "real", base / "link")
    os.symlink(base / "nothing", base / "dangling")
    return [
        ("relative", "relative/dir", "root"),
        ("missing", str(base / "missing"), "root"),
        ("a regular file", str(base / "plain.txt"), "root"),
        ("a link to a directory", str(base / "link"), "root"),
        ("a dangling link", str(base / "dangling"), "root"),
        ("an empty value", "", "root"),
        ("a name over 255 bytes", str(base / ("n" * 256)), "root"),
    ]


@acceptance_case
def launch_roots_refuse_unsafe_paths(case: NativeAcceptance) -> None:
    """Refuse every kind of unusable directory for each launch option that names one.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    workspace = ["--workspace", str(case.workspace)]
    for label, path, mention in _bad_roots(case):
        for option, value in (
            ("--input-root", path),
            ("--shared-root", f"library={path}"),
            ("--media-tools", path),
        ):
            with case.subTest(option=option, root=label):
                _refused(case, f"{option} {label}", [*workspace, option, value], mention)
        with case.subTest(option="--workspace", root=label):
            _refused(case, f"--workspace {label}", ["--workspace", path], mention)
    _repeated_grants(case, workspace)


def _repeated_grants(case: NativeAcceptance, workspace: list[str]) -> None:
    with case.subTest(rule="more than sixteen input roots"):
        folders = []
        for number in range(17):
            folder = case.directory / f"extra-{number}"
            folder.mkdir()
            folders.append(str(folder))
        roots = [item for folder in folders for item in ("--input-root", folder)]
        _refused(case, "17 input roots", [*workspace, *roots], "input root")
    with case.subTest(rule="a second workspace"):
        _refused(case, "two workspaces", [*workspace, *workspace], "--workspace")


def _admitted(case: NativeAcceptance, spelling: str, log: Path) -> Worker:
    worker = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        log,
        test_arguments=("--input-root", spelling),
    )
    case.addCleanup(worker.close)
    return worker


def _spellings(case: NativeAcceptance, real: Path) -> list[tuple[str, str, bool]]:
    """Return (label, spelling, whether the adapter refuses it) for one existing directory."""
    folded = str(real.parent / real.name.upper())
    return [
        ("trailing slash", f"{real}/", False),
        ("dot component", f"{real.parent}/./{real.name}", False),
        ("dot-dot through a sibling", f"{real.parent}/{case.workspace.name}/../{real.name}", False),
        *([("case alias", folded, True)] if Path(folded).exists() else []),
    ]


def _import_through(case: NativeAcceptance, worker: Worker, path: str) -> dict[str, Any]:
    case.worker = worker
    document = case.create("Launch")
    return invoke(case, "level.import", document, path=path, name="Launched", dpi=120)


@acceptance_case
def launch_roots_are_canonical_and_never_widen(case: NativeAcceptance) -> None:
    """Admit a non-canonical spelling of a root only as its real directory, never as an alias.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    real = case.directory / "granted"
    real.mkdir()
    write_fixture(real / "granted.png")
    for label, spelling, adapter_refuses in _spellings(case, real):
        with case.subTest(spelling=label):
            if case.through_sdk and adapter_refuses:
                _refused(
                    case,
                    label,
                    ["--workspace", str(case.workspace), "--input-root", spelling],
                    "canonical",
                )
                continue
            worker = _admitted(case, spelling, case.directory / f"spelled-{len(label)}.log")
            admitted = _import_through(case, worker, str(real / "granted.png"))
            case.assertIn("result", admitted, admitted)
            alias = f"{spelling.rstrip('/')}/granted.png"
            if os.path.normpath(alias) != str(real / "granted.png"):
                refused = _import_through(case, worker, alias)
                case.assertEqual(refused["error"]["code"], "path_denied", refused)


@acceptance_case
def launch_roots_on_another_volume_work_and_stay_separate(case: NativeAcceptance) -> None:
    """Admit a root that lives on its own volume, and deny a link from the other roots into it.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    volume, why = attach_volume(case)
    if volume is None:
        case.skipTest(f"another-volume roots need a disk image: {why}")
    write_fixture(volume / "away.png")
    worker = _admitted(case, str(volume), case.directory / "volume.log")
    imported = _import_through(case, worker, str(volume / "away.png"))
    case.assertIn("result", imported, imported)
    os.symlink(volume / "away.png", case.inputs / "viaduct.png")
    refused = _import_through(case, worker, str(case.inputs / "viaduct.png"))
    case.assertEqual(refused["error"]["code"], "path_denied", refused)


@acceptance_case
def launch_roots_that_cannot_be_read_stay_unreadable(case: NativeAcceptance) -> None:
    """Admit an unreadable or read-only input root, and read nothing through the unreadable one.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    if os.geteuid() == 0:
        case.skipTest("permission bits do not bind the superuser this run uses")
    sealed = case.directory / "sealed"
    sealed.mkdir()
    write_fixture(sealed / "inside.png")
    sealed.chmod(0)
    case.addCleanup(sealed.chmod, 0o755)
    worker = _admitted(case, str(sealed), case.directory / "sealed.log")
    refused = _import_through(case, worker, str(sealed / "inside.png"))
    case.assertIn("error", refused, refused)
    shelf = case.directory / "shelf"
    shelf.mkdir()
    write_fixture(shelf / "shelved.png")
    shelf.chmod(0o555)
    case.addCleanup(shelf.chmod, 0o755)
    worker = _admitted(case, str(shelf), case.directory / "shelf.log")
    case.assertIn("result", _import_through(case, worker, str(shelf / "shelved.png")))


@acceptance_case
def launch_roots_with_odd_names_are_admitted_as_names(case: NativeAcceptance) -> None:
    """Treat a newline, a control character or a 255-byte name in a root as ordinary name text.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    names = ["line\nbreak", "esc\x1b[31m", "n" * 255, unicodedata.normalize("NFC", "café")]
    for index, name in enumerate(names):
        with case.subTest(name=repr(name[:20])):
            root = case.directory / name
            root.mkdir()
            write_fixture(root / "named.png")
            worker = _admitted(case, str(root), case.directory / f"odd-{index}.log")
            result = _import_through(case, worker, str(root / "named.png"))
            case.assertIn("result", result, result)
