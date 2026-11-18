"""Qualify live MCP edits through two SDK clients attached to the normal editor."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import time
import traceback
from pathlib import Path
from typing import Any

from run_harness_tests import annotation
from run_native_ui_test import isolated_fixture, run_captured_process


def stage_fixture(arguments: argparse.Namespace, directory: Path, session: Path) -> Path:
    """Prepare fixed test paths inside an admitted disposable native profile.

    Args:
        arguments: Explicit native application, Node, adapter, and SDK fixture paths.
        directory: Isolated native UI profile owned by this runner.
        session: A new grant path inside a private temporary directory.

    Returns:
        The directory holding the fixture configuration and retained evidence.
    """
    fixture = directory / "mcp-live"
    fixture.mkdir()
    settings = {
        "node": str(arguments.node),
        "client": str(arguments.client),
        "adapter": str(arguments.adapter),
        "session": str(session),
    }
    (fixture / "config.json").write_text(json.dumps(settings) + "\n", encoding="utf-8")
    return fixture


def file_sha256(path: Path) -> str:
    """Return the SHA-256 of one file, read in bounded chunks.

    Args:
        path: The file to hash.

    Returns:
        The lowercase hexadecimal digest.
    """
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_build(fixture: Path, sdk: dict[str, Any], executable: Path) -> str:
    """Require the hello and live.capabilities to name the executable this run started.

    Args:
        fixture: The retained evidence directory from the completed app process.
        sdk: Evidence written by the official SDK journey process.
        executable: The native application the runner launched.

    Returns:
        The executable's SHA-256 that both handshake records carried.
    """
    expected = file_sha256(executable)
    security = json.loads((fixture / "security-boundaries.json").read_text(encoding="utf-8"))
    for label, build in (
        ("connector hello", security.get("hello_build")),
        ("live.capabilities", sdk.get("capabilities_build")),
    ):
        if not isinstance(build, dict) or build.get("executable_sha256") != expected:
            raise AssertionError(f"The {label} build is not the launched executable's SHA-256")
    return expected


def validate_evidence(fixture: Path, report: dict[str, Any], executable: Path) -> None:
    """Require independent SDK and native completion records from the same run.

    Args:
        fixture: The retained evidence directory from the completed app process.
        report: Runner result receiving validated completion counts.
        executable: The native application the runner launched.
    """
    sdk = json.loads((fixture / "sdk.json").read_text(encoding="utf-8"))
    native = json.loads((fixture / "native.json").read_text(encoding="utf-8"))
    if sdk.get("status") != "passed" or native.get("status") != "passed":
        raise AssertionError("Live SDK and independent native observations must both pass")
    report["executable_sha256"] = validate_build(fixture, sdk, executable)
    validate_clients(sdk)
    validate_phases(native)
    if native.get("sdk_exit_code") != 0 or native.get("sdk_exit_status") != "normal":
        raise AssertionError("The SDK fixture did not exit normally")
    if native.get("service_running") is not False:
        raise AssertionError("The live automation grant was not revoked before completion")
    if native.get("active_shutdown") is not True:
        raise AssertionError("The native application did not exit with an active live grant")
    report["sdk_calls"] = sdk["calls"]
    report["native_phases"] = native["phases"]


def validate_clients(sdk: dict[str, Any]) -> None:
    """Require actual client closure and both Stop/shutdown lifecycle observations.

    Args:
        sdk: Evidence written by the official SDK journey process.
    """
    if sdk.get("clients") != 4 or sdk.get("clients_closed") != 4:
        raise AssertionError("All four sequential SDK clients must close their transports")
    if sdk.get("concurrent_clients") != 2:
        raise AssertionError("The live service must be exercised by two concurrent SDK clients")
    for key in (
        "active_shutdown_observed",
        "stop_during_commit_observed",
        "concurrent_connections_observed",
    ):
        if sdk.get(key) is not True:
            raise AssertionError(f"The SDK journey did not observe {key}")
    tools = {call["tool"] for call in sdk["calls"]}
    expected = {
        "live.capabilities",
        "live.status",
        "live.inspect",
        "live.receipt",
        "live.stage.set_key_value",
        "live.fx.set_scalar_value",
    }
    if tools != expected:
        raise AssertionError("The SDK journey must exercise the exact six-tool catalog")


def validate_phases(native: dict[str, Any]) -> None:
    """Require native evidence for the fixed lifecycle and ownership transitions.

    Args:
        native: Independent native controller observations from the same GUI process.
    """
    phases = {entry["phase"] for entry in native["phases"]}
    expected = {
        "human-undo",
        "human-redo",
        "human-stage-edit",
        "history-reset",
        "peg-reinsert",
        "fx-reinsert",
        "fx-suppressed-edit",
        "fx-parameter-replace",
        "camera-size-edit",
        "excluded-artwork-edit",
        "scene-null",
        "scene-replace",
        "scene-reinstate",
        "inspect-mode",
        "edit-mode",
        "modal-open",
        "modal-close",
        "admission-timeout",
        "stop",
        "restart",
        "arm-stop-on-history",
        "shutdown",
        "camera-lock",
        "camera-unlock",
        "curve-lock",
        "curve-unlock",
        "cycle-enable",
        "cycle-disable",
    }
    if not expected.issubset(phases):
        raise AssertionError(f"Missing native journey phases: {sorted(expected - phases)}")
    if native.get("stop_during_history") is not True:
        raise AssertionError("Native Stop was not observed inside a committing history callback")


def confirm_process_group_exit(state: dict[str, Any]) -> None:
    """Verify POSIX descendants exited after the already reaped native parent.

    Args:
        state: The captured runner's original process identity and cleanup witness.
    """
    if os.name == "nt":
        return
    state["safe_to_release"] = False
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        try:
            os.killpg(state["pid"], 0)
        except ProcessLookupError:
            state["safe_to_release"] = True
            return
        time.sleep(0.05)
    raise AssertionError("The native parent exited but its original process group remains")


def qualify(arguments: argparse.Namespace, report: dict[str, Any], session: Path) -> None:
    """Run the fixed asynchronous native suite and retain evidence after cleanup.

    Args:
        arguments: Parsed paths for the real application and official SDK fixture.
        report: Runner result, updated only from observed process evidence.
        session: A new private grant path outside retained evidence.
    """
    state: dict[str, Any] = {"safe_to_release": True}
    with isolated_fixture(arguments.stuff, arguments.executable, arguments.windows_mesa, state) as (
        directory,
        environment,
    ):
        fixture = stage_fixture(arguments, directory, session)
        environment["GROUNDZERO_NATIVE_UI_SUITE"] = "mcp-live"
        try:
            result = run_captured_process(
                [str(arguments.executable), "--test-native-ui"],
                cwd=directory,
                environment=environment,
                timeout=180,
                runtime_state=state,
            )
            report["native_exit_code"] = result.returncode
            if result.returncode != 0:
                raise AssertionError("Live GUI qualification failed; inspect native-output.txt")
            confirm_process_group_exit(state)
            validate_evidence(fixture, report, arguments.executable)
        finally:
            (arguments.output / "native-output.txt").write_bytes(state.get("output", b""))
            report["process_tree_stopped"] = state["safe_to_release"]
            report["native_process_id"] = state.get("pid")
            report["started_at"] = state.get("started_at")
            report["finished_at"] = state.get("finished_at")
            if state["safe_to_release"]:
                shutil.copytree(fixture, arguments.output / "evidence")
    report["status"] = "passed"


def parse_arguments() -> argparse.Namespace:
    """Accept explicit executable paths without discovering tools on PATH.

    Returns:
        Canonical input paths and a newly created output directory.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = ("executable", "stuff", "node", "adapter", "client")
    for option in (*inputs, "output"):
        parser.add_argument(f"--{option}", type=Path, required=True)
    parser.add_argument("--windows-mesa", type=Path)
    arguments = parser.parse_args()
    for name in inputs:
        setattr(arguments, name, getattr(arguments, name).resolve(strict=True))
    arguments.output = arguments.output.resolve()
    if arguments.windows_mesa:
        arguments.windows_mesa = arguments.windows_mesa.resolve(strict=True)
    arguments.output.mkdir(parents=True, exist_ok=False)
    return arguments


def main() -> int:
    """Emit retained evidence and CI annotations for every failed qualification.

    Returns:
        Zero only when the native app and every SDK client complete all checks.
    """
    try:
        arguments = parse_arguments()
    except (OSError, ValueError):
        annotation("Live MCP GUI setup failed", traceback.format_exc())
        return 1
    report: dict[str, Any] = {"status": "failed", "profile": "normal-app-live-mcp"}
    try:
        with tempfile.TemporaryDirectory(
            prefix="gzl-", dir=None if os.name == "nt" else "/tmp"
        ) as grant:
            qualify(arguments, report, Path(grant).resolve() / "session.json")
    except (AssertionError, KeyError, OSError, ValueError, RuntimeError, subprocess.TimeoutExpired):
        report["error"] = traceback.format_exc()
        annotation("Live MCP GUI qualification failed", report["error"])
    (arguments.output / "report.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
