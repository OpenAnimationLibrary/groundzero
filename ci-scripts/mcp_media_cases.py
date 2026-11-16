"""Reject invalid media-tool startup grants before the native protocol begins."""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


def _binary(directory: Path, name: str) -> Path:
    return directory / (f"{name}.exe" if os.name == "nt" else name)


def _pair(case: NativeAcceptance, name: str) -> Path:
    directory = case.directory / name
    directory.mkdir()
    for tool in ("ffmpeg", "ffprobe"):
        path = _binary(directory, tool)
        path.write_bytes(b"Filesystem-only startup rejection fixture; never an encoder.\n")
        path.chmod(0o700)
    return directory


def _arguments(case: NativeAcceptance) -> list[str]:
    return [str(case.executable), "--automation-worker", "--workspace", str(case.workspace)]


def _reject(case: NativeAcceptance, name: str, options: list[str]) -> None:
    result = subprocess.run(
        [*_arguments(case), *options],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
        check=False,
    )
    diagnostics = result.stderr.decode("utf-8", errors="replace")
    (case.directory / f"media-{name}.stderr.log").write_bytes(result.stderr)
    case.assertEqual(result.returncode, 2, diagnostics)
    case.assertEqual(result.stdout, b"", "Invalid startup grant must not start the protocol")
    case.assertIn("automation-worker: invalid_argument:", diagnostics)
    case.assertIn("--media-tools", diagnostics)


def media_directories_require_explicit_canonical_arguments(case: NativeAcceptance) -> None:
    """Reject incomplete, relative, missing, duplicate, or aliased directory grants."""
    directory = _pair(case, "argument-tools")
    invalid = {
        "missing-value": ["--media-tools"],
        "relative": ["--media-tools", "relative-media-tools"],
        "nonexistent": ["--media-tools", str(case.directory / "missing-media-tools")],
        "not-directory": ["--media-tools", str(case.fixture)],
        "duplicate": ["--media-tools", str(directory), "--media-tools", str(directory)],
    }
    for name, arguments in invalid.items():
        with case.subTest(argument=name):
            _reject(case, name, arguments)
    alias = case.directory / "aliased-media-tools"
    alias.symlink_to(directory, target_is_directory=True)
    _reject(case, "linked-directory", ["--media-tools", str(alias)])
    ancestor = case.directory / "aliased-parent"
    ancestor.symlink_to(case.directory, target_is_directory=True)
    _reject(case, "linked-ancestor", ["--media-tools", str(ancestor / directory.name)])


def media_tools_require_both_regular_unlinked_helpers(case: NativeAcceptance) -> None:
    """Reject missing helpers, directories used as executables, and helper symlinks."""
    for helper in ("ffmpeg", "ffprobe"):
        for defect in ("missing", "directory", "symlink"):
            with case.subTest(helper=helper, defect=defect):
                name = f"{helper}-{defect}"
                directory = _pair(case, name)
                path = _binary(directory, helper)
                path.unlink()
                if defect == "directory":
                    path.mkdir()
                elif defect == "symlink":
                    target = case.directory / f"{name}-target"
                    target.write_bytes(b"Not an admitted media helper.\n")
                    target.chmod(0o700)
                    path.symlink_to(target)
                _reject(case, name, ["--media-tools", str(directory)])


def media_tools_require_posix_execute_permission(case: NativeAcceptance) -> None:
    """Exercise POSIX execute-bit admission independently for each configured helper."""
    if os.name == "nt":
        case.skipTest("POSIX execute permission bits are not a Windows executable contract")
    for helper in ("ffmpeg", "ffprobe"):
        with case.subTest(helper=helper):
            directory = _pair(case, f"nonexecutable-{helper}")
            _binary(directory, helper).chmod(0o600)
            _reject(case, f"nonexecutable-{helper}", ["--media-tools", str(directory)])


def media_tools_on_path_do_not_enable_movies(case: NativeAcceptance) -> None:
    """Keep the capability disabled when real media binaries are only on PATH."""
    directory = case.media_tools
    if directory is None:
        case.fail("Media PATH qualification requires --media-tools with real binaries")
    environment = os.environ.copy()
    environment["PATH"] = str(directory) + os.pathsep + environment.get("PATH", "")
    request = {"id": "media-path", "method": "automation.capabilities", "params": {}}
    result = subprocess.run(
        _arguments(case),
        input=(json.dumps(request) + "\n").encode(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
        timeout=30,
        check=False,
    )
    (case.directory / "media-path.stderr.log").write_bytes(result.stderr)
    case.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", errors="replace"))
    response = json.loads(result.stdout)
    case.assertEqual(response["id"], request["id"])
    case.assertIs(response["result"]["media_tools_configured"], False)
