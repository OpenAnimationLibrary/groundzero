"""Keep MCP implementation changes free of planning and agent review artifacts."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath

# Source files that run inside a read-only scene host process.
SCENE_HOST_SOURCES = (
    "toonz/sources/toonz/automation_scene_host.cpp",
    "toonz/sources/toonz/automation_scene_content.cpp",
    "toonz/sources/toonz/automation_scene_source.cpp",
)
# Native calls a scene host must never make: they bind or load through the
# editor's project roots, discard load errors, write files, or create stage
# objects on a read path.
SCENE_HOST_DENYLIST = {
    r"\bToonzScene::load\b|\bscene(?:->|\.)load\s*\(": "ToonzScene::load",
    r"\bloadNoResources\s*\(": "ToonzScene::loadNoResources",
    r"\bloadResources\s*\(": "ToonzScene::loadResources",
    r"\bloadSceneProject\s*\(": "TProjectManager::loadSceneProject",
    r"\bgetProjectRoots\s*\(": "TProjectManager::getProjectRoots",
    r"\bcreateSandboxIfNeeded\s*\(": "TProjectManager::createSandboxIfNeeded",
    r"(?:->|\.|::)save(?:Copy|Palette|AsDefault|Data)?\s*\(": "a native save",
    r"\bgetStageObject\s*\([^;]*,\s*true\s*\)": "getStageObject(id, true)",
}


def scene_host_violations(root: Path) -> list[tuple[str, int, str]]:
    """Find denylisted native calls in the scene host sources, ignoring comments."""
    found = []
    for name in SCENE_HOST_SOURCES:
        path = root / name
        if not path.is_file():
            found.append((name, 0, "the scene host source is missing"))
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            code = line.split("//", 1)[0]
            for pattern, call in SCENE_HOST_DENYLIST.items():
                if re.search(pattern, code):
                    found.append((name, number, call))
    return found


def main() -> int:
    """Check the proposal diff against its base, and the scene host sources' calls."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, help="Trusted base commit or branch")
    args = parser.parse_args()
    root = Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], check=True, stdout=subprocess.PIPE, text=True
        ).stdout.strip()
    )
    violations = scene_host_violations(root)
    for filename, line, call in violations:
        print(
            f"::error file={filename},line={line},title=Scene host denylist::"
            f"A scene host must not call {call}"
        )
    diff = subprocess.run(
        ["git", "diff", "--name-only", "--diff-filter=AMR", "-z", f"{args.base}...HEAD", "--"],
        check=True,
        stdout=subprocess.PIPE,
    )
    filenames = diff.stdout.decode("utf-8").split("\0")
    changes_mcp = any(
        name.startswith(("tools/mcp/", "ci-scripts/mcp_")) or "/automation_" in name
        for name in filenames
    )
    if not changes_mcp:
        return int(bool(violations))
    forbidden = []
    for filename in filenames:
        if not filename:
            continue
        path = PurePosixPath(filename)
        directories = {part.lower() for part in path.parts[:-1]}
        planning_path = any(
            re.search(r"(^|[-_.])(plans?|planning|reviews?)([-_.]|$)", part, re.IGNORECASE)
            for part in path.parts
        )
        if directories.intersection({".agents", ".codex"}) or planning_path:
            forbidden.append(filename)
    for filename in forbidden:
        print(
            f"::error file={filename},title=MCP proposal scope::"
            "Keep planning and agent review artifacts outside the implementation diff"
        )
    return int(bool(forbidden or violations))


if __name__ == "__main__":
    sys.exit(main())
