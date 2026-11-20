"""Observe what a denied file operation left behind: files, processes and log lines."""

from __future__ import annotations

import os
import platform
import shutil
import stat
import subprocess
import time
from collections.abc import Iterable
from pathlib import Path

HOME_LOGS = Path.home() / "Library" / "Logs"


def tree_state(roots: Iterable[Path]) -> dict[str, tuple[str, int, int]]:
    """Return every entry under the roots as name -> (kind, size, mtime); links are not followed.

    Args:
        roots: Directories to walk. A missing root contributes nothing.
    Returns:
        A mapping that differs after any file or directory is created, removed or rewritten.
    """
    state: dict[str, tuple[str, int, int]] = {}
    for root in roots:
        if not root.is_dir():
            continue
        for current, directories, files in os.walk(root, followlinks=False, onerror=None):
            for name in directories + files:
                path = Path(current) / name
                try:
                    info = path.lstat()
                except OSError:
                    continue
                kind = "link" if path.is_symlink() else "dir" if path.is_dir() else "file"
                size = 0 if kind == "dir" else info.st_size
                state[str(path)] = (kind, size, 0 if kind == "dir" else info.st_mtime_ns)
    return state


def descendants(pid: int) -> set[tuple[int, str]]:
    """Return the (pid, command) pairs of every process below pid, however deep.

    Args:
        pid: The process whose subtree is listed.
    Returns:
        An empty set when the process has exited or the platform has no usable ps.
    """
    if shutil.which("ps") is None:
        return set()
    listing = subprocess.run(
        ["ps", "-axo", "pid=,ppid=,comm="], capture_output=True, text=True, check=False
    ).stdout
    children: dict[int, list[tuple[int, str]]] = {}
    for line in listing.splitlines():
        fields = line.split(None, 2)
        if len(fields) == 3 and fields[0].isdigit() and fields[1].isdigit():
            children.setdefault(int(fields[1]), []).append((int(fields[0]), fields[2]))
    found: set[tuple[int, str]] = set()
    pending = [pid]
    while pending:
        for child in children.get(pending.pop(), []):
            if child not in found:
                found.add(child)
                pending.append(child[0])
    return found


def files_mentioning(marker: bytes, roots: Iterable[Path]) -> list[Path]:
    """Return the regular files under the roots whose bytes contain the marker."""
    hits: list[Path] = []
    for root in roots:
        if not root.is_dir():
            continue
        for current, _directories, files in os.walk(root, followlinks=False):
            for name in files:
                path = Path(current) / name
                try:
                    if stat.S_ISREG(path.lstat().st_mode) and marker in path.read_bytes():
                        hits.append(path)
                except OSError:
                    continue
    return hits


def unified_log_mentions(marker: str, since: float) -> bool | None:
    """Whether the macOS unified log holds a native-process line with the marker.

    Args:
        marker: Text that appears only in the denied paths.
        since: Epoch seconds from which to look.
    Returns:
        None where the platform has no readable unified log.
    """
    if platform.system() != "Darwin" or shutil.which("log") is None:
        return None
    start = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(since - 1))
    predicate = f'process == "OpenToonz" AND eventMessage CONTAINS "{marker}"'
    command = ["log", "show", "--start", start, "--predicate", predicate, "--style", "compact"]
    try:
        result = subprocess.run(command, capture_output=True, text=True, check=False, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    return any(line[:2] == "20" and marker in line for line in result.stdout.splitlines())


def home_logs_mentioning(marker: bytes) -> list[Path]:
    """Return the per-user log files that contain the marker."""
    return files_mentioning(marker, [HOME_LOGS])
