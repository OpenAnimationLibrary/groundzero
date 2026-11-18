"""Qualify that the live build identity covers the executable and this project's libraries.

The connector compares a session file's build identity with its own before it connects. A
session file carrying the identity this runner computes is refused only for its missing
endpoint (live_security), which shows the native and runner digests agree. A copied
application with one byte of a project library changed is refused as build_mismatch; one
with an extra library beside the executable that the project does not build is not.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import traceback
import uuid
from pathlib import Path
from typing import Any

from mcp_live import PROJECT_LIBRARIES, build_digest, library_file_name
from run_harness_tests import annotation

LIBRARY_SUFFIX = {"darwin": ".dylib", "win32": ".dll"}.get(sys.platform, ".so")


def beside(executable: Path) -> set[str]:
    """Return the library file names beside an executable.

    Args:
        executable: The application's main executable.

    Returns:
        Every file name there with this platform's library suffix.
    """
    return {path.name for path in executable.parent.iterdir() if path.suffix == LIBRARY_SUFFIX}


def copy_application(executable: Path, root: Path) -> Path:
    """Copy the whole application and return the copy's executable.

    Args:
        executable: The application's main executable.
        root: An empty directory for the copy.

    Returns:
        The copied main executable.
    """
    contents = executable.parent.parent
    bundle = contents.parent if contents.name == "Contents" else executable.parent
    target = root / bundle.name
    shutil.copytree(bundle, target, symlinks=True)
    return target / executable.relative_to(bundle)


def change_one_byte(library: Path) -> None:
    """Change one byte of a library so that it still loads.

    On macOS the byte is padding after the Mach-O load commands, and the library is
    signed again ad hoc, as the linker signed it. Elsewhere a byte is appended.

    Args:
        library: A copied library file.
    """
    if sys.platform != "darwin":
        with library.open("ab") as stream:
            stream.write(b"\0")
        return
    data = bytearray(library.read_bytes())
    (commands,) = struct.unpack_from("<I", data, 20)
    offset = 32 + commands
    if data[offset] != 0:
        raise AssertionError(f"{library.name} has no padding after its load commands")
    data[offset] = 1
    library.write_bytes(bytes(data))
    subprocess.run(["codesign", "-f", "-s", "-", str(library)], check=True, capture_output=True)


def grant(directory: Path, digest: str) -> Path:
    """Write a private session file that names a build and an endpoint that does not exist.

    Args:
        directory: A private directory for the session file.
        digest: The executable_sha256 the session file claims.

    Returns:
        The session file.
    """
    session = {
        "protocol": "groundzero-live-v2",
        "build": {"application": "groundzero", "version": "1.8", "executable_sha256": digest},
        "instance_id": str(uuid.uuid4()),
        "endpoint": str(directory / "absent"),
        "pid": os.getpid(),
        "mode": "inspect",
        "token": "a" * 64,
    }
    path = directory / "session.json"
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        stream.write(json.dumps(session))
    return path


def refusal(executable: Path, session: Path) -> dict[str, Any]:
    """Run the connector against the session file and return its refusal.

    Args:
        executable: The application whose connector runs.
        session: The session file it reads.

    Returns:
        The refusal code and the connector's run time in milliseconds.
    """
    started = time.monotonic()
    result = subprocess.run(
        [str(executable), "--automation-live-connect", str(session)],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=30,
    )
    elapsed = round((time.monotonic() - started) * 1000)
    if result.returncode != 2 or result.stderr:
        raise AssertionError(f"The connector did not refuse with one frame: {result!r}")
    frame = json.loads(result.stdout[4:].decode("utf-8"))
    return {"code": frame["refusal"]["code"], "connector_ms": elapsed}


def require(condition: bool, message: str) -> None:
    """Raise an AssertionError with the message when the condition is false."""
    if not condition:
        raise AssertionError(message)


def qualify(executable: Path, scratch: Path) -> dict[str, Any]:
    """Run the three identity cases against copies of the application.

    Args:
        executable: The built application's main executable.
        scratch: A private directory for the copies and the session file.

    Returns:
        The digests and refusals observed.
    """
    expected = {library_file_name(name) for name in PROJECT_LIBRARIES}
    require(
        beside(executable) == expected, f"Libraries beside the executable: {beside(executable)}"
    )
    digest = build_digest(executable)
    session = grant(scratch, digest)
    report: dict[str, Any] = {"digest": digest, "original": refusal(executable, session)}
    require(report["original"]["code"] == "live_security", "The native identity differs")
    changed = copy_application(executable, scratch / "changed")
    change_one_byte(changed.parent / library_file_name("toonzlib"))
    report["project_library"] = {"digest": build_digest(changed), **refusal(changed, session)}
    require(report["project_library"]["digest"] != digest, "A library change kept the digest")
    require(report["project_library"]["code"] == "build_mismatch", "A changed build connected")
    extra = copy_application(executable, scratch / "extra")
    foreign = extra.parent / f"libthirdparty{LIBRARY_SUFFIX}"
    shutil.copyfile(extra.parent / library_file_name("tnzext"), foreign)
    change_one_byte(foreign)
    report["third_party_library"] = {"digest": build_digest(extra), **refusal(extra, session)}
    require(report["third_party_library"]["digest"] == digest, "A foreign library counted")
    require(report["third_party_library"]["code"] == "live_security", "A foreign library counted")
    return report


def main() -> int:
    """Write the report and an annotation for any failed check.

    Returns:
        Zero only when all three cases behave as described.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=False)
    report: dict[str, Any] = {"status": "failed"}
    try:
        with tempfile.TemporaryDirectory(
            prefix="gzi-", dir=None if os.name == "nt" else "/tmp"
        ) as scratch:
            report.update(
                qualify(arguments.executable.resolve(strict=True), Path(scratch).resolve())
            )
        report["status"] = "passed"
    except (AssertionError, KeyError, OSError, ValueError, subprocess.SubprocessError):
        report["error"] = traceback.format_exc()
        annotation("Live build identity qualification failed", report["error"])
    (arguments.output / "report.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
