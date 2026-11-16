"""Exercise operating-system transport failures using the real native worker."""

from __future__ import annotations

import os
import subprocess
import sys
from typing import TYPE_CHECKING, BinaryIO

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


def _reap(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is None:
        process.kill()
    process.wait(timeout=5)
    for stream in (process.stdin, process.stdout, process.stderr):
        if stream is not None:
            stream.close()


def _launch(test: NativeAcceptance, log: BinaryIO, stdin: int) -> subprocess.Popen[bytes]:
    workspace = test.directory / "transport-workspace"
    workspace.mkdir()
    process = subprocess.Popen(
        [
            str(test.executable),
            "--automation-worker",
            "--workspace",
            str(workspace),
            "--input-root",
            str(test.inputs),
        ],
        stdin=stdin,
        stdout=subprocess.PIPE,
        stderr=log,
    )
    test.addCleanup(_reap, process)
    return process


def closed_output_has_controlled_exit(test: NativeAcceptance) -> None:
    """Require an output failure to drain the worker instead of receiving SIGPIPE."""
    if sys.platform == "win32":
        test.skipTest("SIGPIPE handling is a POSIX transport contract")
    log_path = test.directory / "closed-output.log"
    with log_path.open("wb") as log:
        process = _launch(test, log, subprocess.PIPE)
        assert process.stdout is not None and process.stdin is not None
        process.stdout.close()
        process.stdin.write(b'{"id":"output","method":"automation.capabilities","params":{}}\n')
        process.stdin.close()
        test.assertEqual(process.wait(timeout=30), 2)
    test.assertIn("protocol output failed", log_path.read_text(encoding="utf-8"))


def unreadable_input_is_not_clean_eof(test: NativeAcceptance) -> None:
    """Require Linux directory input to report its read error with nonzero exit."""
    if sys.platform != "linux":
        test.skipTest("Directory stdin exercises the Linux EISDIR error path")
    log_path = test.directory / "unreadable-input.log"
    descriptor = os.open(test.inputs, os.O_RDONLY)
    try:
        with log_path.open("wb") as log:
            process = _launch(test, log, descriptor)
            process.communicate(timeout=30)
            test.assertEqual(process.returncode, 2)
    finally:
        os.close(descriptor)
    test.assertIn("protocol input failed", log_path.read_text(encoding="utf-8"))
