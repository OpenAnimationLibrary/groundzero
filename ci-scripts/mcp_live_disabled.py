"""Qualify that a normal launch without --automation-live leaves no live bridge state.

Two launches of the same build run the native mcp-live-disabled suite. The control launch
also starts and stops a live session to prove each observation sees the bridge. Both
launches must record the same windows, scene, sockets and threads with the bridge
disabled, and write the same profile and settings files.
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import traceback
from pathlib import Path
from typing import Any

from mcp_live import confirm_process_group_exit
from run_harness_tests import annotation
from run_native_ui_test import isolated_fixture, run_captured_process

TEMPORARY_NAME = re.compile(r"(<fixture>/[^\s]*?-)[A-Za-z0-9]{6}(?=/)")
COMPARED = (
    "windows",
    "status_bar_visible",
    "live_objects",
    "bridge_threads",
    "grant_files",
    "bridge_sockets",
    "tcp_listeners",
    "scene",
)


def normalized(path: Path, directory: Path) -> bytes:
    """Return a written file without what differs between any two launches.

    Args:
        path: A file the launch wrote.
        directory: The launch's fixture directory, recorded in history and project settings.

    Returns:
        The bytes, with the fixture path and temporary-name suffixes replaced and room
        pane geometry and view transforms (set by window placement) removed from layout
        files.
    """
    data = path.read_bytes()
    if path.suffix not in (".txt", ".env", ".ini"):
        return data
    text = data.decode("utf-8", errors="replace")
    for name in {str(directory), str(directory.resolve())}:
        text = text.replace(name, "<fixture>")
    text = TEMPORARY_NAME.sub(r"\1XXXXXX", text)
    if path.suffix == ".ini" and "layouts" in path.parts:
        lines = text.splitlines()
        text = "\n".join(
            line for line in lines if "\\geometry=" not in line and "\\viewAff" not in line
        )
    return text.encode("utf-8")


def written_files(directory: Path) -> dict[str, str]:
    """Hash every file a launch left in its isolated profile, settings and data folders.

    Args:
        directory: The launch's disposable fixture directory after the app exited.

    Returns:
        Relative path to the SHA-256 of its normalized content, without the evidence
        folder and the output capture.
    """
    files = {}
    for path in sorted(directory.rglob("*")):
        relative = path.relative_to(directory).as_posix()
        if relative.startswith("mcp-live") or relative.startswith("native-process-"):
            continue
        if path.is_file() and not path.is_symlink():
            files[relative] = hashlib.sha256(normalized(path, directory)).hexdigest()
    return files


def launch(arguments: argparse.Namespace, label: str, control: bool) -> dict[str, Any]:
    """Run one launch of the suite and return its evidence and written files.

    Args:
        arguments: The application, its stuff folder and the output directory.
        label: Name of this launch in the retained output.
        control: Whether the launch also starts and stops a live session.

    Returns:
        The native evidence with the launch's written files under "files".
    """
    state: dict[str, Any] = {"safe_to_release": True}
    with tempfile.TemporaryDirectory(prefix="gzd-", dir="/tmp") as grants:
        with isolated_fixture(arguments.stuff, arguments.executable, None, state) as (
            directory,
            environment,
        ):
            fixture = directory / "mcp-live"
            fixture.mkdir()
            settings = {"grant_directory": str(Path(grants).resolve()), "control": control}
            (fixture / "config.json").write_text(json.dumps(settings) + "\n", encoding="utf-8")
            environment["GROUNDZERO_NATIVE_UI_SUITE"] = "mcp-live-disabled"
            try:
                result = run_captured_process(
                    [str(arguments.executable), "--test-native-ui"],
                    cwd=directory,
                    environment=environment,
                    timeout=180,
                    runtime_state=state,
                )
            finally:
                output = arguments.output / f"{label}-output.txt"
                output.write_bytes(state.get("output", b""))
                if (fixture / "disabled.json").is_file():
                    shutil.copyfile(fixture / "disabled.json", arguments.output / f"{label}.json")
            confirm_process_group_exit(state)
            evidence = json.loads((fixture / "disabled.json").read_text(encoding="utf-8"))
            if result.returncode != 0 or evidence.get("status") != "passed":
                raise AssertionError(f"The {label} launch failed; inspect {output.name}")
            evidence["files"] = written_files(directory)
            evidence["settings"] = {
                path: normalized(directory / path, directory).decode("utf-8", errors="replace")
                for path in evidence["files"]
                if path.endswith(".ini")
            }
    return evidence


def compare(control: dict[str, Any], plain: dict[str, Any]) -> None:
    """Require both launches to agree with the bridge disabled and to write the same files.

    Args:
        control: Evidence of the launch that also ran a live session.
        plain: Evidence of the launch that never started one.
    """
    for key in COMPARED:
        if control["disabled"][key] != plain["disabled"][key]:
            raise AssertionError(f"Disabled launches differ in {key}")
    added = sorted(control["files"].keys() ^ plain["files"].keys())
    changed = sorted(
        path
        for path in control["files"].keys() & plain["files"].keys()
        if control["files"][path] != plain["files"][path]
    )
    if added or changed:
        differences = [
            line
            for path in changed
            if path in control["settings"]
            for line in difflib.unified_diff(
                plain["settings"][path].splitlines(),
                control["settings"][path].splitlines(),
                f"plain/{path}",
                f"control/{path}",
                lineterm="",
                n=0,
            )
        ]
        raise AssertionError(
            f"Launches wrote different files: {added + changed}\n" + "\n".join(differences[:40])
        )


def parse_arguments() -> argparse.Namespace:
    """Accept the application, its stuff folder and a new output directory.

    Returns:
        Resolved input paths and a newly created output directory.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    for option in ("executable", "stuff", "output"):
        parser.add_argument(f"--{option}", type=Path, required=True)
    arguments = parser.parse_args()
    arguments.executable = arguments.executable.resolve(strict=True)
    arguments.stuff = arguments.stuff.resolve(strict=True)
    arguments.output = arguments.output.resolve()
    arguments.output.mkdir(parents=True, exist_ok=False)
    return arguments


def main() -> int:
    """Write the report and an annotation for any failed check.

    Returns:
        Zero only when both launches pass and agree.
    """
    if os.name == "nt":
        annotation("Disabled-bridge qualification", "The socket and thread checks are POSIX only")
        return 1
    arguments = parse_arguments()
    report: dict[str, Any] = {"status": "failed"}
    try:
        control = launch(arguments, "control", True)
        plain = launch(arguments, "plain", False)
        report.update(control=control, plain=plain)
        compare(control, plain)
        report["status"] = "passed"
    except (AssertionError, KeyError, OSError, ValueError, RuntimeError, subprocess.TimeoutExpired):
        report["error"] = traceback.format_exc()
        annotation("Disabled-bridge qualification failed", report["error"])
    (arguments.output / "report.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
