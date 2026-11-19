"""Rename the acceptance evidence entries whose names the artifact upload refuses.

Cases create files and folders named with newlines and other characters that
actions/upload-artifact rejects, which fails the whole upload. Each refused character is
percent-encoded in place, so every entry is kept.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

REFUSED = frozenset('"*:<>?|\r\n')


def uploadable(name: str) -> str:
    """Return the name with every refused character percent-encoded."""
    if not REFUSED.intersection(name):
        return name
    return "".join(f"%{ord(char):02X}" if char in REFUSED else char for char in name)


def rename_refused(root: Path) -> list[tuple[Path, Path]]:
    """Rename every entry under root whose name the upload refuses, deepest first.

    Raises:
        FileExistsError: An encoded name is already taken.
    """
    renamed = []
    for directory, folders, files in os.walk(root, topdown=False):
        for name in [*files, *folders]:
            target = uploadable(name)
            if target == name:
                continue
            source = Path(directory) / name
            destination = Path(directory) / target
            if destination.exists() or destination.is_symlink():
                raise FileExistsError(f"cannot rename {source!r}: {destination!r} exists")
            source.rename(destination)
            renamed.append((source, destination))
    return renamed


def main() -> int:
    """Rename the refused entries under the given folder and list them."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    if not args.root.is_dir():
        print(f"{args.root}: no evidence folder; nothing to rename")
        return 0
    for source, destination in rename_refused(args.root):
        print(f"renamed {str(source)!r} to {destination.name!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
