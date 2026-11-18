"""Bounded NDJSON client for acceptance tests of the native automation worker."""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path
from queue import Empty, Queue
from threading import Thread
from typing import Any, BinaryIO

MAX_RESPONSE_BYTES = 16 * 1024 * 1024


class Worker:
    """Own one native process and retain its stderr for CI failure diagnosis."""

    def __init__(
        self,
        executable: Path,
        workspace: Path,
        inputs: Path,
        log: Path,
        *,
        media_tools: Path | None = None,
        shared_roots: dict[str, Path] | None = None,
        shared_write: tuple[str, ...] = (),
    ) -> None:
        self.sequence = 0
        self.log_path = log
        self.errors = log.open("wb")
        self.responses: Queue[bytes | Exception] = Queue()
        arguments = [
            str(executable),
            "--automation-worker",
            "--workspace",
            str(workspace),
            "--input-root",
            str(inputs),
        ]
        if media_tools is not None:
            arguments.extend(["--media-tools", str(media_tools)])
        for role, root in (shared_roots or {}).items():
            arguments.extend(["--shared-root", f"{role}={root}"])
        for role in shared_write:
            arguments.extend(["--shared-write", role])
        self.process = subprocess.Popen(
            arguments,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.errors,
        )
        assert self.process.stdout is not None
        self.reader = Thread(target=self._read, args=(self.process.stdout,), daemon=True)
        self.reader.start()

    def _read(self, stream: BinaryIO) -> None:
        try:
            while True:
                line = stream.readline(MAX_RESPONSE_BYTES + 1)
                if not line:
                    raise EOFError("Native worker closed stdout")
                if len(line) > MAX_RESPONSE_BYTES:
                    raise ValueError("Native worker exceeded the response size bound")
                self.responses.put(line)
        except (OSError, ValueError, EOFError) as error:
            self.responses.put(error)

    def exchange(self, payload: bytes, *, timeout: float = 30) -> dict[str, Any]:
        """Send one line and require one bounded JSON object within the deadline."""
        assert self.process.stdin is not None
        self.process.stdin.write(payload + b"\n")
        self.process.stdin.flush()
        try:
            response = self.responses.get(timeout=timeout)
        except Empty as error:
            raise TimeoutError(
                f"Native worker did not respond within {timeout}s: {self._diagnostic_tail()}"
            ) from error
        if isinstance(response, Exception):
            raise RuntimeError(f"Worker transport failed: {response}; {self._diagnostic_tail()}")
        result = json.loads(response)
        if not isinstance(result, dict):
            raise AssertionError(f"Worker response must be an object: {result!r}")
        if ("result" in result) == ("error" in result):
            raise AssertionError(f"Worker response must contain exactly one outcome: {result!r}")
        return result

    def _diagnostic_tail(self) -> str:
        with self.log_path.open("rb") as log:
            log.seek(0, 2)
            log.seek(max(0, log.tell() - 4096))
            return log.read().decode("utf-8", errors="replace")

    def call(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        """Call a native operation and require its matching request identifier."""
        self.sequence += 1
        identifier = f"acceptance-{self.sequence}"
        payload = json.dumps({"id": identifier, "method": method, "params": params}).encode()
        response = self.exchange(payload)
        if response.get("id") != identifier:
            raise AssertionError(f"Worker returned an unrelated request id: {response!r}")
        return response

    def _record_stalled_exit(self) -> None:
        """Save a stack sample of a worker that did not exit, where the platform has one."""
        sampler = shutil.which("sample")
        if sampler is None:
            return
        report = self.log_path.with_suffix(".stalled-exit.txt")
        subprocess.run(
            [sampler, str(self.process.pid), "2", "-file", str(report)],
            capture_output=True,
            check=False,
            timeout=30,
        )

    def close(self) -> None:
        """Close the transport and reap the original native process."""
        if self.process.stdin is not None:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self._record_stalled_exit()
            self.process.kill()
            self.process.wait(timeout=5)
        self.reader.join(timeout=5)
        if self.process.stdout is not None:
            self.process.stdout.close()
        self.errors.close()
