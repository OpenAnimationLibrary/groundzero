"""Run the acceptance suite in parallel shards balanced by recorded test durations.

The planning follows the local qualification's shard runner: the longest test goes first onto
the least loaded shard, a test without a recorded duration counts as the median, and the tests
that must never run at the same time as each other all go to the first shard, counted as its
load. Each shard is one `mcp_acceptance.py --only` process with its own output folder. The run
fails when a shard fails a test, ends without reporting every test it was given, or has a worker
that missed its exit deadline.
"""

from __future__ import annotations

import heapq
import json
import os
import statistics
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import IO

from run_harness_tests import annotation

DURATIONS = Path(__file__).resolve().parent / "mcp_suite_durations.json"
DURATIONS_NAME = "durations.json"
STALLED_EXIT = "*.stalled-exit*.txt"

# Tests that use the host's disk-image service (hdiutil) run in one shard, one after another:
# concurrent hdiutil work timed out and stalled a worker exit in the local qualification.
SERIAL_TESTS: dict[str, str] = {
    "test_read_tools_hold_the_file_boundary": "attaches an APFS disk image with hdiutil",
    "test_library_case_fold_collisions_are_reported": "attaches a disk image with hdiutil",
    "test_render_disk_full_fails_cleanly_and_recovers": "fills a disk image from hdiutil",
    "test_render_survives_its_volume_being_removed": "force-detaches an hdiutil disk image",
}


def shard_count(value: str) -> int:
    """Return the number of shards: an integer, or `auto` for one per processor."""
    count = (os.cpu_count() or 1) if value == "auto" else int(value)
    if count < 1:
        raise ValueError(f"--shards must be at least 1, not {value!r}")
    return count


def load_durations(path: Path = DURATIONS) -> dict[str, float]:
    """Return the recorded seconds per test, or nothing when no record exists."""
    if not path.exists():
        return {}
    return {name: float(seconds) for name, seconds in json.loads(path.read_text()).items()}


def plan(names: list[str], durations: dict[str, float], count: int) -> list[list[str]]:
    """Split names into at most count non-empty shards; every name lands in exactly one.

    The serial tests all go to the first shard, after its other tests.
    """
    serial = [name for name in names if name in SERIAL_TESTS]
    others = [name for name in names if name not in SERIAL_TESTS]
    known = [durations[name] for name in names if name in durations]
    fallback = statistics.median(known) if known else 1.0
    weight = {name: durations.get(name, fallback) for name in names}
    count = max(1, min(count, len(others) + (1 if serial else 0)))
    shards: list[list[str]] = [[] for _ in range(count)]
    heap = [
        (sum(weight[name] for name in serial) if index == 0 else 0.0, index)
        for index in range(count)
    ]
    heapq.heapify(heap)
    for name in sorted(others, key=lambda item: (-weight[item], item)):
        load, index = heapq.heappop(heap)
        shards[index].append(name)
        heapq.heappush(heap, (load + weight[name], index))
    shards[0].extend(serial)
    planned = sorted(name for shard in shards for name in shard)
    if planned != sorted(names):
        raise RuntimeError("the shard plan lost or repeated a test")
    return [shard for shard in shards if shard]


@dataclass
class Shard:
    """One acceptance process, its tests and how it ended."""

    index: int
    tests: list[str]
    output: Path
    process: subprocess.Popen[bytes] | None = None
    seconds: float = 0.0

    def problems(self) -> list[str]:
        """Name what makes this shard fail beyond the failures its tests reported."""
        found = []
        code = self.process.returncode if self.process else None
        reported = self.output / DURATIONS_NAME
        seen = set(load_durations(reported)) if reported.exists() else set()
        missing = [name for name in self.tests if name not in seen]
        if missing:
            found.append(
                f"ended (exit code {code}) without reporting {len(missing)} test(s), "
                f"first {missing[0]}"
            )
        stalled = sorted(self.output.rglob(STALLED_EXIT))
        if stalled:
            found.append(
                f"{len(stalled)} worker(s) missed the exit deadline: "
                f"{', '.join(path.name for path in stalled[:3])}"
            )
        return found


def _forward(stream: IO[bytes], label: str, lock: threading.Lock) -> None:
    for raw in stream:
        line = raw.decode("utf-8", errors="replace").rstrip("\n")
        with lock:
            # Workflow commands must start the line to be read as annotations.
            print(line if line.startswith("::") else f"[{label}] {line}", flush=True)


def _start(shard: Shard, command: list[str], lock: threading.Lock) -> threading.Thread:
    shard.output.mkdir(parents=True, exist_ok=True)
    names = shard.output / "tests.txt"
    names.write_text("\n".join(shard.tests) + "\n", encoding="utf-8")
    shard.process = subprocess.Popen(
        [*command, "--output", str(shard.output), "--only", str(names)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert shard.process.stdout is not None
    reader = threading.Thread(
        target=_forward, args=(shard.process.stdout, f"shard {shard.index}", lock), daemon=True
    )
    reader.start()
    return reader


def run(names: list[str], count: int, output: Path, command: list[str]) -> int:
    """Run the named tests in count shards under output and return the exit status.

    Args:
        names: Every test of the suite.
        count: How many shards to run at once.
        output: The evidence folder; each shard writes to `shard-<n>` in it.
        command: The acceptance command without `--output` and `--only`.
    """
    shards = [
        Shard(index, tests, output / f"shard-{index}")
        for index, tests in enumerate(plan(names, load_durations(), count))
    ]
    print(
        f"running {len(names)} tests in {len(shards)} shard(s): "
        + ", ".join(f"shard {shard.index} {len(shard.tests)}" for shard in shards),
        flush=True,
    )
    lock = threading.Lock()
    started = time.monotonic()
    readers = [_start(shard, command, lock) for shard in shards]
    for shard, reader in zip(shards, readers, strict=True):
        assert shard.process is not None
        shard.process.wait()
        reader.join()
        shard.seconds = time.monotonic() - started
    return _judge(shards, output)


def _judge(shards: list[Shard], output: Path) -> int:
    failed = False
    durations: dict[str, float] = {}
    for shard in shards:
        assert shard.process is not None
        print(
            f"shard {shard.index}: {len(shard.tests)} tests, exit code "
            f"{shard.process.returncode}, done after {shard.seconds:.0f} s",
            flush=True,
        )
        reported = shard.output / DURATIONS_NAME
        if reported.exists():
            durations.update(load_durations(reported))
        problems = shard.problems()
        for problem in problems:
            annotation(f"acceptance shard {shard.index}", problem)
        failed = failed or bool(problems) or shard.process.returncode != 0
    # The record a later plan can be refreshed from.
    (output / DURATIONS_NAME).write_text(json.dumps(durations, indent=1, sort_keys=True) + "\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit("run mcp_acceptance.py --shards N")
