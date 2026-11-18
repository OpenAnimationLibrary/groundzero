"""Hash directory trees so that a read tool's effect on files can be proven absent."""

from __future__ import annotations

import fnmatch
import hashlib
import os
import stat
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

PRIVATE_STORAGE = "workspace/runtime_*"
FIELDS = ("existence", "kind", "size", "mode", "mtime_ns", "digest")


@dataclass(frozen=True)
class Entry:
    """Everything a read must leave alone about one file, directory or link."""

    kind: str
    size: int
    mode: int
    mtime_ns: int
    digest: str


@dataclass(frozen=True)
class Cache:
    """A path where a read may legitimately write, with the reason it does.

    Attributes:
        pattern: Glob over `<root label>/<relative path>`; it must be the worker's private
            runtime directory or lie inside it, never under an input or shared root.
        reason: Why the native read needs this cache.
        fields: What the read may change about the matching entries: whether they exist, and
            their kind, size, mode, modification time or content. Everything by default.
    """

    pattern: str
    reason: str
    fields: tuple[str, ...] = FIELDS

    def __post_init__(self) -> None:
        if self.pattern != PRIVATE_STORAGE and not self.pattern.startswith(PRIVATE_STORAGE + "/"):
            raise ValueError(
                f"Cache {self.pattern!r} must lie inside the worker's private storage "
                f"({PRIVATE_STORAGE}), never under an input or shared root"
            )
        if not self.reason.strip():
            raise ValueError(f"Cache {self.pattern!r} needs a stated reason")
        unknown = set(self.fields) - set(FIELDS)
        if unknown:
            raise ValueError(f"Cache {self.pattern!r} names unknown fields {sorted(unknown)}")


Tree = dict[str, Entry]


def _entry(path: Path) -> Entry:
    info = path.lstat()
    mode = stat.S_IMODE(info.st_mode)
    if stat.S_ISLNK(info.st_mode):
        return Entry("link", 0, mode, info.st_mtime_ns, os.readlink(path))
    if stat.S_ISDIR(info.st_mode):
        return Entry("dir", 0, mode, info.st_mtime_ns, "")
    with path.open("rb") as handle:
        digest = hashlib.file_digest(handle, "sha256").hexdigest()
    return Entry("file", info.st_size, mode, info.st_mtime_ns, digest)


def snapshot(roots: Mapping[str, Path]) -> Tree:
    """Hash every file, directory and link under each labelled root.

    Args:
        roots: Label to root directory, for example the workspace or a shared root.
    Returns:
        Entries keyed by `<label>/<relative path>`; the root itself is `<label>/`.
    """
    tree: Tree = {}
    for label, root in roots.items():
        tree[f"{label}/"] = _entry(root)
        for directory, names, files in os.walk(root):
            names.sort()
            for name in [*names, *files]:
                path = Path(directory) / name
                tree[f"{label}/{path.relative_to(root).as_posix()}"] = _entry(path)
    return tree


def _differences(key: str, before: Entry | None, after: Entry | None) -> list[tuple[str, str]]:
    if before is None:
        return [("existence", f"new {key}")]
    if after is None:
        return [("existence", f"removed {key}")]
    return [
        (field, f"changed {key}: {'content' if field == 'digest' else field}")
        for field in FIELDS[1:]
        if getattr(before, field) != getattr(after, field)
    ]


def compare(before: Tree, after: Tree, caches: Sequence[Cache] = ()) -> list[str]:
    """List every difference between two snapshots, except what declared caches allow.

    Args:
        before: The earlier snapshot.
        after: The later snapshot.
        caches: Declared cache paths and what each may change.
    Returns:
        One line per added, removed or changed entry; empty when nothing changed.
    """
    notes = []
    for key in sorted(before.keys() | after.keys()):
        allowed = {
            field
            for cache in caches
            if fnmatch.fnmatchcase(key, cache.pattern)
            for field in cache.fields
        }
        notes.extend(
            note
            for field, note in _differences(key, before.get(key), after.get(key))
            if field not in allowed
        )
    return notes


def snapshot_named(directories: Mapping[str, Path], patterns: Sequence[str]) -> Tree:
    """Hash only the entries of each directory whose names match a pattern, recursively.

    Args:
        directories: Label to a directory that is shared with other programs.
        patterns: Case-insensitive globs over entry names directly inside a directory.
    Returns:
        Entries keyed by `<label>/<relative path>`; a missing directory has none.
    """
    tree: Tree = {}
    for label, directory in directories.items():
        if not directory.is_dir():
            continue
        for child in sorted(directory.iterdir()):
            if not any(
                fnmatch.fnmatch(child.name.lower(), pattern.lower()) for pattern in patterns
            ):
                continue
            if child.is_dir() and not child.is_symlink():
                tree.update(snapshot({f"{label}/{child.name}": child}))
            else:
                tree[f"{label}/{child.name}"] = _entry(child)
    return tree
