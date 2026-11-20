"""Qualify that the live build identity covers the executable and this project's libraries.

The connector compares a session file's build identity with its own before it connects. The
runner reads the application's build manifest and recomputes its digest over the files it
names. A session file carrying that digest is refused only for its missing endpoint
(live_security), which shows the connector computes the same digest. A copied
application with one byte of a project library changed is refused as build_mismatch; one
with an extra library beside the executable that the project does not build is not. On
macOS, a copy that loads a project library under another name refuses to identify its build.
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

from mcp_live import build_manifest, verified_build_digest
from run_harness_tests import annotation


def application_root(executable: Path) -> Path:
    """Return the folder a copy of the application must include.

    Args:
        executable: The application's main executable.

    Returns:
        The macOS bundle that contains it, or else its directory.
    """
    contents = executable.parent.parent
    return contents.parent if contents.name == "Contents" else executable.parent


def copy_application(executable: Path, root: Path) -> Path:
    """Copy the whole application and return the copy's executable.

    Args:
        executable: The application's main executable.
        root: An empty directory for the copy.

    Returns:
        The copied main executable.
    """
    bundle = application_root(executable)
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
        digest: The build_digest the session file claims.

    Returns:
        The session file.
    """
    session = {
        "protocol": "groundzero-live-v2",
        "build": {"application": "groundzero", "version": "1.8", "build_digest": digest},
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


def copied_library(executable: Path, copy: Path, library: dict[str, Any]) -> Path:
    """Return where a library of the application lies in a copy of it.

    Args:
        executable: The original main executable.
        copy: The copy's main executable.
        library: A manifest entry of the original application.

    Returns:
        The same file inside the copy.
    """
    root = application_root(executable)
    relative = Path(library["path"]).resolve().relative_to(root.resolve())
    return application_root(copy) / relative


def copy_digest(executable: Path, copy: Path) -> str:
    """Verify a copy's build digest and that it covers the copy's own files.

    Args:
        executable: The original main executable.
        copy: The copy's main executable.

    Returns:
        The copy's verified build_digest.
    """
    root = application_root(copy).resolve()
    for entry in build_manifest(copy)["files"]:
        require(Path(entry["path"]).resolve().is_relative_to(root), "A copy used outside files")
    return verified_build_digest(copy)


def load_renamed(executable: Path, library: str, renamed: str) -> None:
    """Make a copied macOS application load one project library under another name.

    Args:
        executable: The copy's main executable.
        library: The library's file name, loaded only by the main executable.
        renamed: The file name the copy loads it from instead.
    """
    folder = executable.parent
    (folder / library).rename(folder / renamed)
    tool = ["install_name_tool"]
    subprocess.run([*tool, "-id", f"@executable_path/{renamed}", str(folder / renamed)], check=True)
    old, new = f"@executable_path/{library}", f"@executable_path/{renamed}"
    subprocess.run([*tool, "-change", old, new, str(executable)], check=True, capture_output=True)
    for path in (folder / renamed, executable):
        subprocess.run(["codesign", "-f", "-s", "-", str(path)], check=True, capture_output=True)


def missing_library(executable: Path, scratch: Path, session: Path) -> dict[str, Any]:
    """Require a build whose project library loaded under another name to refuse to identify.

    Args:
        executable: The original main executable.
        scratch: A private directory for the copy.
        session: A session file the connector reads.

    Returns:
        The manifest mode's exit code and diagnostic and the connector's refusal.
    """
    copy = copy_application(executable, scratch / "missing")
    load_renamed(copy, "libtfarm.dylib", "libtfrm.dylib")
    manifest = subprocess.run(
        [str(copy), "--automation-build-manifest"],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=60,
    )
    report = {
        "manifest_exit": manifest.returncode,
        "manifest_stderr": manifest.stderr.decode("utf-8", errors="replace").strip(),
        **refusal(copy, session),
    }
    require(manifest.returncode == 1 and not manifest.stdout, "A missing library was identified")
    require("not loaded" in report["manifest_stderr"], "The missing library was not reported")
    require(report["code"] == "connector_io", "A connector without its library connected")
    return report


def qualify(executable: Path, scratch: Path) -> dict[str, Any]:
    """Run the three identity cases against copies of the application.

    Args:
        executable: The built application's main executable.
        scratch: A private directory for the copies and the session file.

    Returns:
        The digests and refusals observed.
    """
    libraries = build_manifest(executable)["files"][1:]
    require(bool(libraries), "The build manifest names no project library")
    digest = verified_build_digest(executable)
    session = grant(scratch, digest)
    report: dict[str, Any] = {
        "digest": digest,
        "libraries": [entry["name"] for entry in libraries],
        "original": refusal(executable, session),
    }
    require(report["original"]["code"] == "live_security", "The native identity differs")
    changed = copy_application(executable, scratch / "changed")
    change_one_byte(copied_library(executable, changed, libraries[-1]))
    report["project_library"] = {
        "changed": libraries[-1]["name"],
        "digest": copy_digest(executable, changed),
        **refusal(changed, session),
    }
    require(report["project_library"]["digest"] != digest, "A library change kept the digest")
    require(report["project_library"]["code"] == "build_mismatch", "A changed build connected")
    extra = copy_application(executable, scratch / "extra")
    source = copied_library(executable, extra, libraries[0])
    foreign = source.with_name(f"libthirdparty{source.suffix}")
    shutil.copyfile(source, foreign)
    change_one_byte(foreign)
    report["third_party_library"] = {
        "digest": copy_digest(executable, extra),
        **refusal(extra, session),
    }
    require(report["third_party_library"]["digest"] == digest, "A foreign library counted")
    require(report["third_party_library"]["code"] == "live_security", "A foreign library counted")
    if sys.platform == "darwin":
        report["missing_project_library"] = missing_library(executable, scratch, session)
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
