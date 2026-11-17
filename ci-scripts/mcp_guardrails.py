"""Keep MCP implementation changes free of planning and agent review artifacts."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import PurePosixPath


def main() -> int:
    """Check the actual proposal diff against its base without reading file contents."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, help="Trusted base commit or branch")
    args = parser.parse_args()
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
        return 0
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
    return int(bool(forbidden))


if __name__ == "__main__":
    sys.exit(main())
