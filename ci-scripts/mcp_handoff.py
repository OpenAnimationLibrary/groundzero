"""Qualify a relocated MCP shot through normal app load, edit, save, and rendering."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import traceback
from pathlib import Path
from typing import Any

from run_harness_tests import annotation
from run_native_ui_test import isolated_fixture, run_captured_process

MARKERS = (
    "mcp-handoff-native: normal-load PASS",
    "mcp-handoff-native: original-render PASS",
    "mcp-handoff-native: edit-undo-save-reload PASS",
    "mcp-handoff-native: edited-render PASS",
    "native-ui-diagnostic: PASS mcp-handoff",
)


def package_hashes(directory: Path) -> dict[str, str]:
    """Fingerprint a bounded producer package without accepting linked assets."""
    files = sorted(directory.rglob("*"))
    if len(files) > 256 or any(path.is_symlink() for path in files):
        raise ValueError("Handoff package must contain at most 256 unlinked entries")
    total = 0
    result = {}
    for path in files:
        if not path.is_file():
            continue
        total += path.stat().st_size
        if total > 64 * 1024 * 1024:
            raise ValueError("Handoff package exceeds 64 MiB")
        result[path.relative_to(directory).as_posix()] = hashlib.sha256(
            path.read_bytes()
        ).hexdigest()
    if "scene.tnz" not in result:
        raise ValueError("Handoff package has no scene.tnz")
    return result


def stage_fixture(arguments: argparse.Namespace, directory: Path) -> Path:
    """Copy the completed shot and its oracle into the disposable native profile."""
    fixture = directory / "mcp-handoff"
    fixture.mkdir()
    shutil.copytree(arguments.package, fixture / "package")
    shutil.copyfile(arguments.reference, fixture / "reference.png")
    proof = arguments.package.parent.parent / "proof.json"
    if proof.stat().st_size > 4 * 1024 * 1024:
        raise ValueError("Shot proof exceeds 4 MiB")
    data = json.loads(proof.read_text(encoding="utf-8"))
    if data.get("status") != "passed" or data.get("title") != "Moonlight courier":
        raise ValueError("Normal app handoff requires the completed fixed Moonlight courier shot")
    shutil.copyfile(proof, fixture / "proof.json")
    return fixture


def launch(
    arguments: argparse.Namespace,
    directory: Path,
    environment: dict[str, str],
    state: dict[str, Any],
) -> subprocess.CompletedProcess[bytes]:
    """Run only the fixed diagnostic in the normal application process."""
    environment["GROUNDZERO_NATIVE_UI_SUITE"] = "mcp-handoff"
    return run_captured_process(
        [str(arguments.executable), "--test-native-ui"],
        cwd=directory,
        environment=environment,
        timeout=720,
        runtime_state=state,
    )


def qualify(arguments: argparse.Namespace, report: dict[str, Any]) -> None:
    """Retain the original process evidence and fail on any missing native stage."""
    original = package_hashes(arguments.package)
    state: dict[str, Any] = {"safe_to_release": True}
    with isolated_fixture(arguments.stuff, arguments.executable, arguments.windows_mesa, state) as (
        directory,
        environment,
    ):
        fixture = stage_fixture(arguments, directory)
        try:
            result = launch(arguments, directory, environment, state)
            report["native_exit_code"] = result.returncode
            output = result.stdout.decode("utf-8", errors="replace")
            present = output.splitlines()
            report["missing_markers"] = [marker for marker in MARKERS if marker not in present]
            if result.returncode != 0 or report["missing_markers"]:
                raise AssertionError("Normal app handoff failed; inspect native-output.txt")
        finally:
            output = state.get("output", b"")
            (arguments.output / "native-output.txt").write_bytes(output)
            report["process_tree_stopped"] = state["safe_to_release"]
            if state["safe_to_release"]:
                shutil.copytree(fixture, arguments.output / "native-evidence")
    if package_hashes(arguments.package) != original:
        raise AssertionError("Normal app handoff changed the original producer package")
    report["original_package_unchanged"] = True
    report["status"] = "passed"


def parse_arguments() -> argparse.Namespace:
    """Accept explicit local build, copied-data, producer, and evidence paths."""
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("executable", "stuff", "package", "reference", "output"):
        parser.add_argument(f"--{option}", type=Path, required=True)
    parser.add_argument("--windows-mesa", type=Path)
    arguments = parser.parse_args()
    for name in ("executable", "stuff", "package", "reference"):
        setattr(arguments, name, getattr(arguments, name).resolve(strict=True))
    arguments.output = arguments.output.resolve()
    if arguments.windows_mesa:
        arguments.windows_mesa = arguments.windows_mesa.resolve(strict=True)
    arguments.output.mkdir(parents=True, exist_ok=False)
    return arguments


def main() -> int:
    """Emit retained evidence and an actionable CI annotation for every failure."""
    try:
        arguments = parse_arguments()
    except (OSError, ValueError) as error:
        annotation("Normal app MCP handoff setup failed", str(error))
        return 1
    report: dict[str, Any] = {
        "status": "failed",
        "profile": "normal-app-rgba-shot-handoff",
    }
    try:
        qualify(arguments, report)
    except (
        AssertionError,
        OSError,
        ValueError,
        RuntimeError,
        subprocess.TimeoutExpired,
    ) as error:
        report["error"] = str(error)
        (arguments.output / "failure.txt").write_text(traceback.format_exc(), encoding="utf-8")
        annotation("Normal app MCP handoff failed", str(error))
    (arguments.output / "report.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, sort_keys=True))
    return int(report["status"] != "passed")


if __name__ == "__main__":
    raise SystemExit(main())
