"""Boundary scenarios every path-taking tool must hold, built from the file system outward."""

from __future__ import annotations

import os
import shutil
import subprocess
import threading
import time
import unicodedata
import unittest
import uuid
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

from mcp_file_tools import PathTool

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

DENIED = frozenset({"path_denied"})
TRAVERSAL = frozenset({"invalid_path", "INVALID_PARAMS"})
EXISTS = frozenset({"already_exists", "ALREADY_EXISTS"})
NO_PARENT = frozenset({"missing_parent", "MISSING_PARENT"})
ADMIT, DENY, ALIAS = "admit", "deny", "alias"
NAME_BYTES = 255
PIPE_RELEASE_SECONDS = 20
PIPE_BLOCK_LIMIT = 12


@dataclass
class Layout:
    """The directories one tool's scenarios share and the means to build objects in them."""

    case: NativeAcceptance
    tool: PathTool
    state: dict
    root: Path
    base: Path
    outside: Path
    volume: Path | None
    marker: str
    count: int = 0
    tag: str = ""

    def __post_init__(self) -> None:
        self.tag = uuid.uuid4().hex[:6]

    def leaf(self, stem: str = "item") -> str:
        """Return a unique file name that carries the marker and the tool's suffix."""
        self.count += 1
        return f"{self.marker}-{self.tag}-{stem}-{self.count}{self.tool.suffix}"

    def existing(self, path: Path) -> Path:
        """Create a valid object of the tool at the path."""
        path.parent.mkdir(parents=True, exist_ok=True)
        if self.tool.mode == "published" and not path.is_relative_to(self.case.workspace):
            made = self.case.workspace / f"{self.marker}-{self.tag}-made-{self.count}"
            self.tool.place(self.case, self.state, made)
            shutil.move(made, path)
        else:
            self.tool.place(self.case, self.state, path)
        return path

    def target(self, directory: Path | None = None, stem: str = "item") -> Path:
        """Return a path for the tool: an existing input, or a free path for an output."""
        path = (directory or self.base) / self.leaf(stem)
        return path if self.tool.mode == "write" else self.existing(path)

    def directory(self, parent: Path, name: str) -> Path:
        """Create and return a uniquely named directory."""
        self.count += 1
        path = parent / f"{self.marker}-{self.tag}-{name}-{self.count}"
        path.mkdir(parents=True)
        return path

    def link(self, destination: Path, name: str = "link") -> Path:
        """Create a symbolic link beside the base files and return it."""
        self.count += 1
        link = self.base / f"{self.marker}-{self.tag}-{name}-{self.count}"
        os.symlink(destination, link)
        return link


Builder = Callable[[Layout], str]
Codes = Callable[[PathTool], frozenset[str]]
Scenario = tuple[str, Builder, str, Codes]


def _denied(_tool: PathTool) -> frozenset[str]:
    return DENIED


def _traversal(_tool: PathTool) -> frozenset[str]:
    return TRAVERSAL


def _absent(tool: PathTool) -> frozenset[str]:
    return tool.missing | NO_PARENT


def _unusable(tool: PathTool) -> frozenset[str]:
    return tool.missing | DENIED | TRAVERSAL | NO_PARENT


def _aliased(tool: PathTool) -> frozenset[str]:
    return _unusable(tool) | EXISTS


def _collision(tool: PathTool) -> frozenset[str]:
    return EXISTS | DENIED if tool.mode == "write" else _absent(tool)


def _link_to(layout: Layout, directory: Path, existing: bool = True) -> str:
    object_path = directory / layout.leaf("real")
    if existing:
        layout.existing(object_path)
    return str(layout.link(object_path))


def link_inside(layout: Layout) -> str:
    return _link_to(layout, layout.base)


def link_outside(layout: Layout) -> str:
    return _link_to(layout, layout.outside)


def link_dangling_inside(layout: Layout) -> str:
    return _link_to(layout, layout.base, existing=False)


def link_dangling_outside(layout: Layout) -> str:
    return _link_to(layout, layout.outside, existing=False)


def link_chain(layout: Layout) -> str:
    first = layout.link(layout.existing(layout.base / layout.leaf("real")))
    return str(layout.link(first, "chain"))


def _through_directory_link(layout: Layout, real: Path | None) -> str:
    leaf = layout.leaf("below")
    if real is not None:
        layout.existing(real / leaf)
        return f"{layout.link(real, 'dirlink')}/{leaf}"
    return f"{layout.link(layout.outside / 'nowhere', 'dirlink')}/{leaf}"


def dirlink_inside(layout: Layout) -> str:
    return _through_directory_link(layout, layout.directory(layout.base, "realdir"))


def dirlink_outside(layout: Layout) -> str:
    return _through_directory_link(layout, layout.directory(layout.outside, "realdir"))


def dirlink_dangling(layout: Layout) -> str:
    return _through_directory_link(layout, None)


def dirlink_to_root(layout: Layout) -> str:
    leaf = layout.leaf("below")
    layout.existing(layout.root / leaf)
    return f"{layout.link(layout.root, 'rootlink')}/{leaf}"


def dotdot_middle(layout: Layout) -> str:
    folder = layout.directory(layout.base, "sub")
    return f"{folder}/../{layout.target().name}"


def dotdot_leading(layout: Layout) -> str:
    path = layout.target()
    return "../" + str(path.relative_to(layout.root.parent))


def dotdot_trailing(layout: Layout) -> str:
    return f"{layout.target()}/.."


def dotdot_bare(_layout: Layout) -> str:
    return ".."


def dotdot_directory(layout: Layout) -> str:
    return f"{layout.base}/.."


def dotdot_out_and_back(layout: Layout) -> str:
    path = layout.target()
    return f"{layout.base}/../{layout.base.name}/{path.name}"


def dotdot_to_outside(layout: Layout) -> str:
    path = layout.target(layout.outside)
    return f"{layout.base}/../{layout.outside.name}/{path.name}"


def dotdot_backslash(layout: Layout) -> str:
    path = layout.target()
    return f"{layout.base}\\..\\{path.name}"


def dots_are_names(layout: Layout) -> str:
    layout.target()
    return f"{layout.base}/.../{layout.leaf()}"


def percent_dots(layout: Layout) -> str:
    layout.target()
    return f"{layout.base}/%2e%2e/{layout.leaf()}"


def absolute_outside(layout: Layout) -> str:
    return str(layout.target(layout.outside))


def absolute_root(_layout: Layout) -> str:
    return "/"


def absolute_system_file(_layout: Layout) -> str:
    return "/etc/hosts" if Path("/etc/hosts").exists() else "/bin/sh"


def sibling_prefix(layout: Layout) -> str:
    sibling = Path(str(layout.root) + "-evil")
    sibling.mkdir(exist_ok=True)
    return str(layout.target(sibling))


def file_url(layout: Layout) -> str:
    return "file://" + str(layout.target())


def user_home(layout: Layout) -> str:
    return "~/" + layout.target().name


def nul_byte(layout: Layout) -> str:
    return f"{layout.target()}\x00{layout.tool.suffix}"


def trailing_slash(layout: Layout) -> str:
    return f"{layout.target()}/"


def double_slash(layout: Layout) -> str:
    path = layout.target()
    return f"{path.parent}//{path.name}"


def dot_component(layout: Layout) -> str:
    path = layout.target()
    return f"{path.parent}/./{path.name}"


def name_at_limit(layout: Layout) -> str:
    stem = "n" * (NAME_BYTES - len(layout.tool.suffix))
    return _named(layout, stem)


def _named(layout: Layout, stem: str) -> str:
    path = layout.base / (stem + layout.tool.suffix)
    return str(path if layout.tool.mode == "write" else layout.existing(path))


def name_over_limit(layout: Layout) -> str:
    stem = "n" * (NAME_BYTES + 1 - len(layout.tool.suffix))
    return str(layout.base / (stem + layout.tool.suffix))


def path_over_limit(layout: Layout) -> str:
    return str(layout.base) + ("/" + "d" * 60) * 24 + "/" + layout.leaf()


def path_deep(layout: Layout) -> str:
    folder = layout.base
    for _ in range(60):
        folder = folder / "d"
    folder.mkdir(parents=True)
    return str(layout.target(folder))


def _special(layout: Layout, name: str) -> str:
    path = layout.base / (f"{layout.marker}-{layout.tag}-{name}" + layout.tool.suffix)
    return str(path if layout.tool.mode == "write" else layout.existing(path))


def newline_name(layout: Layout) -> str:
    return _special(layout, "line\nbreak")


def tab_name(layout: Layout) -> str:
    return _special(layout, "tab\tstop")


def escape_name(layout: Layout) -> str:
    return _special(layout, "esc\x1b[31mred")


def delete_name(layout: Layout) -> str:
    return _special(layout, "del\x7fchar")


def bidi_name(layout: Layout) -> str:
    return _special(layout, "bidi‮over")


def locked_directory(layout: Layout) -> str:
    folder = layout.directory(layout.base, "locked")
    path = str(layout.target(folder))
    folder.chmod(0)
    layout.case.addCleanup(folder.chmod, 0o755)
    return path


def read_only_directory(layout: Layout) -> str:
    folder = layout.directory(layout.base, "readonly")
    path = str(layout.target(folder))
    folder.chmod(0o555)
    layout.case.addCleanup(folder.chmod, 0o755)
    return path


def release_pipe_later(path: Path, seconds: float) -> None:
    """Open the pipe's write end after a delay so a worker blocked on it is freed, not wedged."""

    def release() -> None:
        deadline = time.monotonic() + seconds + 20
        time.sleep(seconds)
        while time.monotonic() < deadline:
            try:
                os.close(os.open(path, os.O_WRONLY | os.O_NONBLOCK))
                return
            except OSError:
                time.sleep(0.2)

    threading.Thread(target=release, daemon=True).start()


def fifo(layout: Layout) -> str:
    path = layout.base / layout.leaf("fifo")
    os.mkfifo(path)
    release_pipe_later(path, PIPE_RELEASE_SECONDS)
    return str(path)


def _alias_pair(layout: Layout, spell: Callable[[str], str]) -> str:
    stem = "Café-Mixed"
    path = layout.base / layout.leaf(stem)
    layout.existing(path)
    return str(path.with_name(spell(path.name)))


def case_alias(layout: Layout) -> str:
    return _alias_pair(layout, str.swapcase)


def nfd_alias(layout: Layout) -> str:
    return _alias_pair(layout, lambda name: unicodedata.normalize("NFD", name))


def nfc_alias_of_decomposed(layout: Layout) -> str:
    stem = unicodedata.normalize("NFD", "Café-Decomposed")
    path = layout.base / layout.leaf(stem)
    layout.existing(path)
    return str(path.with_name(unicodedata.normalize("NFC", path.name)))


def hard_link_outside(layout: Layout) -> str:
    original = layout.existing(layout.outside / layout.leaf("original"))
    if not original.is_file():
        raise unittest.SkipTest("only a file can have a second hard link")
    link = layout.base / layout.leaf("hard")
    os.link(original, link)
    return str(link)


def volume_absolute(layout: Layout) -> str:
    assert layout.volume is not None
    return str(layout.target(layout.volume))


def volume_link(layout: Layout) -> str:
    assert layout.volume is not None
    return _link_to(layout, layout.volume)


def volume_directory_link(layout: Layout) -> str:
    assert layout.volume is not None
    return _through_directory_link(layout, layout.directory(layout.volume, "realdir"))


def workspace_file(layout: Layout) -> str:
    return str(layout.target(layout.case.workspace))


def relative_name(layout: Layout) -> str:
    return layout.target().name


def input_root_target(layout: Layout) -> str:
    return str(layout.case.inputs / layout.leaf())


def existing_destination(layout: Layout) -> str:
    return str(layout.existing(layout.base / layout.leaf("taken")))


def missing_parent(layout: Layout) -> str:
    return f"{layout.base}/{layout.marker}-none/{layout.leaf()}"


def swapped_after_publication(layout: Layout) -> str:
    path = layout.target()
    moved = path.with_name(path.name + ".moved")
    path.rename(moved)
    os.symlink(moved, path)
    return str(path)


def unregistered_copy(layout: Layout) -> str:
    original = layout.target()
    copy = original.with_name(original.name + "-copy")
    shutil.copytree(original, copy)
    return str(copy)


def outside_copy(layout: Layout) -> str:
    original = layout.target()
    copy = layout.outside / (original.name + "-copy")
    shutil.copytree(original, copy)
    return str(copy)


def _always(codes: frozenset[str]) -> Codes:
    return lambda _tool: codes


COMMON: list[Scenario] = [
    ("plain path", lambda layout: str(layout.target()), ADMIT, _denied),
    ("link to a file inside", link_inside, DENY, _denied),
    ("link to a file outside", link_outside, DENY, _denied),
    ("dangling link inside", link_dangling_inside, DENY, _denied),
    ("dangling link outside", link_dangling_outside, DENY, _denied),
    ("chain of links", link_chain, DENY, _denied),
    ("parent link to a folder inside", dirlink_inside, DENY, _denied),
    ("parent link to a folder outside", dirlink_outside, DENY, _denied),
    ("parent link that dangles", dirlink_dangling, DENY, _denied),
    ("parent link to the root itself", dirlink_to_root, DENY, _denied),
    ("dot-dot in the middle", dotdot_middle, DENY, _traversal),
    ("dot-dot leading a relative path", dotdot_leading, DENY, _traversal),
    ("dot-dot last", dotdot_trailing, DENY, _traversal),
    ("dot-dot alone", dotdot_bare, DENY, _traversal),
    ("dot-dot after a directory", dotdot_directory, DENY, _traversal),
    ("dot-dot out and back into the root", dotdot_out_and_back, DENY, _traversal),
    ("dot-dot into the outside folder", dotdot_to_outside, DENY, _traversal),
    ("backslash dot-dot", dotdot_backslash, DENY, _traversal),
    ("three dots are an ordinary name", dots_are_names, DENY, _unusable),
    ("percent-encoded dot-dot is an ordinary name", percent_dots, DENY, _unusable),
    ("absolute path outside", absolute_outside, DENY, _denied),
    ("the file system root", absolute_root, DENY, _denied),
    ("a system file", absolute_system_file, DENY, _denied),
    ("sibling that shares the root's prefix", sibling_prefix, DENY, _denied),
    ("file URL", file_url, DENY, _unusable),
    ("home shortcut", user_home, DENY, _unusable),
    ("NUL byte", nul_byte, DENY, _traversal),
    ("name at the 255-byte limit", name_at_limit, ADMIT, _denied),
    ("name one byte over the limit", name_over_limit, DENY, _traversal),
    ("path over the system limit", path_over_limit, DENY, _unusable),
    ("sixty folders deep", path_deep, ADMIT, _denied),
    ("newline in the name", newline_name, ADMIT, _denied),
    ("tab in the name", tab_name, ADMIT, _denied),
    ("escape sequence in the name", escape_name, ADMIT, _denied),
    ("delete character in the name", delete_name, ADMIT, _denied),
    ("bidirectional override in the name", bidi_name, ADMIT, _denied),
    ("unreadable folder", locked_directory, DENY, _unusable),
    ("case alias of an existing name", case_alias, DENY, _denied),
    ("decomposed spelling of a composed name", nfd_alias, DENY, _denied),
    ("composed spelling of a decomposed name", nfc_alias_of_decomposed, ALIAS, _aliased),
    ("trailing slash", trailing_slash, ADMIT, _denied),
    ("doubled slash", double_slash, ADMIT, _denied),
    ("dot component", dot_component, ADMIT, _denied),
]
VOLUME: list[Scenario] = [
    ("absolute path on another volume", volume_absolute, DENY, _denied),
    ("link to a file on another volume", volume_link, DENY, _denied),
    ("parent link to a folder on another volume", volume_directory_link, DENY, _denied),
]
BY_MODE: dict[str, list[Scenario]] = {
    "read": [
        ("read-only folder", read_only_directory, ADMIT, _denied),
        ("hard link to a file outside", hard_link_outside, ADMIT, _denied),
        ("an input in the workspace", workspace_file, ADMIT, _denied),
        ("bare name resolves against the workspace", relative_name, DENY, _absent),
        ("named pipe", fifo, DENY, _absent),
    ],
    "write": [
        ("read-only folder", read_only_directory, DENY, _absent),
        ("the read-only input root", input_root_target, DENY, _denied),
        ("existing destination", existing_destination, DENY, _collision),
        ("hard link to a file outside as destination", hard_link_outside, DENY, _collision),
        ("missing parent folder", missing_parent, DENY, _absent),
        ("named pipe as destination", fifo, DENY, _collision),
    ],
    "published": [
        ("read-only folder", read_only_directory, ADMIT, _denied),
        ("link swapped in after publication", swapped_after_publication, DENY, _denied),
        ("copy of a publication", unregistered_copy, DENY, _absent),
        ("copy of a publication outside the roots", outside_copy, DENY, _denied),
    ],
}


def attach_volume(case: NativeAcceptance) -> tuple[Path | None, str]:
    """Mount a small disk image for the case; return its mount point or why it cannot be had."""
    if shutil.which("hdiutil") is None:
        return None, "disk images need hdiutil, which only macOS provides"
    image = case.directory / "volume.dmg"
    mount = case.directory / "volume"
    mount.mkdir()
    created = subprocess.run(
        ["hdiutil", "create", "-size", "8m", "-fs", "APFS", "-volname", "gzqvolume", str(image)],
        capture_output=True,
        text=True,
        check=False,
        timeout=120,
    )
    if created.returncode != 0:
        return None, f"hdiutil create failed: {created.stderr.strip()[:200]}"
    attached = subprocess.run(
        ["hdiutil", "attach", "-nobrowse", "-mountpoint", str(mount), str(image)],
        capture_output=True,
        text=True,
        check=False,
        timeout=120,
    )
    if attached.returncode != 0:
        return None, f"hdiutil attach failed: {attached.stderr.strip()[:200]}"
    case.addCleanup(detach_volume, mount)
    return mount.resolve(), ""


def detach_volume(mount: Path) -> None:
    """Detach the disk image, forcing it when something still holds a file open."""
    command = ["hdiutil", "detach", str(mount)]
    if subprocess.run(command, capture_output=True, check=False, timeout=120).returncode != 0:
        subprocess.run([*command, "-force"], capture_output=True, check=False, timeout=120)
