"""Retain MCP command output and expose its actual failure in GitHub annotations."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from run_harness_tests import annotation


def main() -> int:
    """Run one bounded CI check without dropping diagnostics on nonzero exit."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--title", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        result = subprocess.run(
            args.command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=180,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        message = f"{type(error).__name__}: {error}"
        args.output.write_text(message, encoding="utf-8")
        annotation(args.title, message)
        return 1
    args.output.write_text(result.stdout, encoding="utf-8")
    print(result.stdout, end="", flush=True)
    if result.returncode:
        annotation(args.title, result.stdout or f"Command exited with status {result.returncode}")
    return int(result.returncode != 0)


if __name__ == "__main__":
    sys.exit(main())
