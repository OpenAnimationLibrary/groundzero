"""Exercise native helper ownership using real isolated POSIX process groups."""

from __future__ import annotations

import argparse
import json
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


def emit(value: dict[str, Any]) -> None:
    print(json.dumps(value), flush=True)


def relay(executable: str) -> int:
    return subprocess.call([executable], pass_fds=(3,))


def receive_frame(channel: socket.socket, expected: bytes) -> None:
    value = b""
    deadline = time.monotonic() + 5
    while len(value) < 44:
        channel.settimeout(max(0.001, deadline - time.monotonic()))
        chunk = channel.recv(45 - len(value))
        if not chunk:
            raise EOFError("Native closed before completing its echo")
        value += chunk
    if value != expected:
        raise ValueError("Native echo differs from the private frame")


def startup(channel: socket.socket, scenario: str) -> None:
    frame = bytearray(struct.pack("<III", 0x475A4153, 1, os.getpid()) + os.urandom(32))
    edits = {"wrong-version": (4, 2), "wrong-frame-pid": (8, os.getpid() + 1), "bad-magic": (0, 0)}
    if scenario in edits:
        offset, value = edits[scenario]
        struct.pack_into("<I", frame, offset, value)
    if scenario == "timeout":
        return
    if scenario == "truncated":
        channel.sendall(frame[:-1])
        channel.shutdown(socket.SHUT_WR)
        return
    if scenario == "oversized":
        channel.sendall(frame + b"x")
        return
    channel.sendall(frame)
    if scenario in edits or scenario in {"wrong-peer", "wrong-session"}:
        return
    receive_frame(channel, bytes(frame))
    acknowledgement = {"missing-ack": b"", "wrong-ack": b"\x02"}.get(scenario, b"\x01")
    channel.sendall(acknowledgement)


def launch_native(executable: str, scenario: str, channel: socket.socket) -> subprocess.Popen[str]:
    command = [executable]
    if scenario in {"guardian-eof", "guardian-extra", "anchor-death", "native-death"}:
        command.append("--block")
    if scenario == "kill-failure":
        command.append("--kill-failure")
    if scenario == "wrong-peer":
        command = [sys.executable, __file__, "--relay", "--executable", executable]
    environment = {**os.environ, "GROUNDZERO_AUTOMATION_CONTROL_FD": "3"}
    os.dup2(channel.fileno(), 3, inheritable=True)
    try:
        return subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
            pass_fds=(3,),
            start_new_session=scenario == "wrong-session",
        )
    finally:
        os.close(3)


def read_identity(child: subprocess.Popen[str]) -> dict[str, Any]:
    assert child.stdout is not None
    if not select.select([child.stdout], [], [], 5)[0]:
        raise TimeoutError("Native fixture did not report its helper")
    line = child.stdout.readline()
    if not line:
        raise RuntimeError("Native fixture exited before reporting its helper")
    identity = json.loads(line)
    if identity.get("contained") is not True or identity.get("helper", -1) < 1:
        raise ValueError("Fixture did not establish helper containment")
    return identity


def exercise_guardian(child: subprocess.Popen[str], channel: socket.socket, scenario: str) -> None:
    emit({"event": "ready", **read_identity(child)})
    command = sys.stdin.readline().strip()
    if command == "stop":
        os.killpg(os.getpgrp(), signal.SIGKILL)
    if scenario == "guardian-extra":
        channel.sendall(b"x")
    elif scenario == "native-death":
        child.kill()
    else:
        channel.close()
    child.wait(timeout=6)
    emit({"event": "native-exit", "code": child.returncode})
    os.killpg(os.getpgrp(), signal.SIGKILL)


def anchor(executable: str, scenario: str) -> int:
    original, inherited = socket.socketpair()
    require(original.fileno() == 3, "Private test anchor descriptor3 is unavailable")
    parent = original.dup()
    original.close()
    child = launch_native(executable, scenario, inherited)
    inherited.close()
    blocking = {"guardian-eof", "guardian-extra", "anchor-death", "native-death", "kill-failure"}
    try:
        startup(parent, scenario)
        if scenario in blocking:
            exercise_guardian(child, parent, scenario)
        stdout, stderr = child.communicate(timeout=7)
        emit({"event": "native-exit", "code": child.returncode, "stdout": stdout, "stderr": stderr})
        return 0
    except (OSError, EOFError, ValueError, TimeoutError, subprocess.TimeoutExpired) as error:
        emit({"event": "anchor-error", "message": str(error)})
        child.kill()
        child.communicate(timeout=5)
        return 3
    finally:
        parent.close()


def start_anchor(executable: str, scenario: str) -> subprocess.Popen[str]:
    return subprocess.Popen(
        [sys.executable, __file__, "--anchor", scenario, "--executable", executable],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def startup_case(executable: str, scenario: str) -> dict[str, Any]:
    child = start_anchor(executable, scenario)
    try:
        stdout, stderr = child.communicate(timeout=10)
        result = json.loads(stdout)
        require(child.returncode == 0 and not stderr, f"Anchor failed: {stdout} {stderr}")
        require(result["event"] == "native-exit", str(result))
        expected = 0 if scenario == "valid" else 2
        require(result["code"] == expected, f"Unexpected native outcome: {result}")
        if scenario == "valid":
            identity = json.loads(result["stdout"])
            require(identity["contained"] is True, "Valid handshake did not establish ownership")
            require(not result["stderr"], str(result))
        else:
            require(not result["stdout"], "Rejected startup emitted a success identity")
        return {"case": scenario, "passed": True, "native": result}
    finally:
        if child.poll() is None:
            os.killpg(child.pid, signal.SIGKILL)
            child.communicate(timeout=5)


def ready_anchor(executable: str, scenario: str) -> tuple[subprocess.Popen[str], dict[str, Any]]:
    child = start_anchor(executable, scenario)
    assert child.stdout is not None
    if not select.select([child.stdout], [], [], 7)[0]:
        os.killpg(child.pid, signal.SIGKILL)
        child.communicate(timeout=5)
        raise TimeoutError("Anchor did not establish its native helper")
    identity = json.loads(child.stdout.readline())
    require(identity["event"] == "ready", str(identity))
    require(identity["group"] == child.pid, "Native fixture escaped its anchor group")
    return child, identity


def stop_anchor(child: subprocess.Popen[str], command: str = "stop") -> tuple[str, str]:
    assert child.stdin is not None
    child.stdin.write(command + "\n")
    child.stdin.flush()
    return child.communicate(timeout=8)


def owned_state(pid: int, group: int) -> str:
    try:
        if os.getpgid(pid) != group:
            return "identity_changed"
    except ProcessLookupError:
        return "absent"
    result = subprocess.run(
        ["ps", "-p", str(pid), "-o", "stat="],
        capture_output=True,
        text=True,
        check=False,
    )
    if not result.stdout.strip() and not result.stderr:
        return "absent"
    require(result.returncode == 0, result.stderr)
    return "zombie" if result.stdout.strip().startswith("Z") else "running"


def stopped_owned(identity: dict[str, Any]) -> dict[str, str]:
    deadline = time.monotonic() + 3
    while True:
        states = {
            str(identity[key]): owned_state(identity[key], identity["group"])
            for key in ("pid", "helper")
        }
        require("identity_changed" not in states.values(), f"Owned process changed group: {states}")
        if all(state in {"absent", "zombie"} for state in states.values()):
            return states
        require(time.monotonic() < deadline, f"Owned native/helper processes remain: {states}")
        time.sleep(0.02)


def cleanup_owned(child: subprocess.Popen[str], identity: dict[str, Any]) -> None:
    if child.poll() is None:
        os.killpg(child.pid, signal.SIGKILL)
    else:
        for key in ("pid", "helper"):
            if owned_state(identity[key], identity["group"]) == "running":
                os.kill(identity[key], signal.SIGKILL)
    child.communicate(timeout=5)


def guardian_case(executable: str, scenario: str) -> dict[str, Any]:
    child, identity = ready_anchor(executable, scenario)
    try:
        if scenario == "anchor-death":
            child.kill()
            stdout, stderr = child.communicate(timeout=8)
        else:
            stdout, stderr = stop_anchor(child, "exercise")
        require(child.returncode == -signal.SIGKILL, f"Owned group survived: {child.returncode}")
        require(not stderr, stderr)
        if scenario == "kill-failure":
            result = json.loads(stdout)
            require(result["code"] == 125, f"Guardian returned after failed group kill: {result}")
        return {
            "case": scenario,
            "passed": True,
            "identity": identity,
            "stdout": stdout,
            "process_states": stopped_owned(identity),
        }
    finally:
        cleanup_owned(child, identity)


def independent_groups(executable: str) -> dict[str, Any]:
    first, first_identity = ready_anchor(executable, "guardian-eof")
    second, second_identity = ready_anchor(executable, "guardian-eof")
    try:
        stop_anchor(first, "exercise")
        require(first.returncode == -signal.SIGKILL, "First group did not terminate")
        require(second.poll() is None, "Independent anchor was terminated")
        for key in ("pid", "helper"):
            require(
                owned_state(second_identity[key], second_identity["group"]) == "running",
                "Independent native or helper stopped or changed group",
            )
        stop_anchor(second)
        first_states = stopped_owned(first_identity)
        second_states = stopped_owned(second_identity)
        return {
            "case": "independent-groups",
            "passed": True,
            "identities": [first_identity, second_identity],
            "process_states": [first_states, second_states],
        }
    finally:
        cleanup_owned(first, first_identity)
        cleanup_owned(second, second_identity)


def raw_cases(executable: str) -> list[dict[str, Any]]:
    results = []
    for marker in (None, "3", "4"):
        environment = dict(os.environ)
        environment.pop("GROUNDZERO_AUTOMATION_CONTROL_FD", None)
        if marker is not None:
            environment["GROUNDZERO_AUTOMATION_CONTROL_FD"] = marker
        run = subprocess.run(
            [executable], capture_output=True, text=True, env=environment, timeout=7, check=False
        )
        if marker is None:
            require(run.returncode == 0, run.stderr)
            require(
                json.loads(run.stdout)["contained"] is False, "Raw process claimed helper ownership"
            )
        else:
            require(
                run.returncode == 2 and not run.stdout, "Marker-only launch established ownership"
            )
        results.append({"case": f"raw-marker-{marker}", "passed": True, "stderr": run.stderr})
    return results


def save_results(output: Path | None, results: list[dict[str, Any]]) -> None:
    if output:
        output.write_text(json.dumps(results, indent=2) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--anchor")
    parser.add_argument("--relay", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.relay:
        return relay(args.executable)
    if args.anchor:
        return anchor(args.executable, args.anchor)
    results = raw_cases(args.executable)
    save_results(args.output, results)
    for scenario in (
        "valid",
        "wrong-version",
        "bad-magic",
        "wrong-frame-pid",
        "wrong-peer",
        "wrong-session",
        "truncated",
        "oversized",
        "missing-ack",
        "wrong-ack",
        "timeout",
    ):
        results.append(startup_case(args.executable, scenario))
        save_results(args.output, results)
    for scenario in (
        "guardian-eof",
        "guardian-extra",
        "anchor-death",
        "native-death",
        "kill-failure",
    ):
        results.append(guardian_case(args.executable, scenario))
        save_results(args.output, results)
    results.append(independent_groups(args.executable))
    save_results(args.output, results)
    emit({"passed": True, "cases": len(results)})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
