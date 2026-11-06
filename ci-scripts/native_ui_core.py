"""Bounded, non-executing diagnostics for the original Linux native UI crash.

The workflow owns kernel.core_pattern and removes the private directory on exit.
Only the primary application launch tree temporarily enables core dumps. No replay process,
raw core, locals, or argument values are published by this helper.
"""
from contextlib import contextmanager
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile
import time


CORE_BYTES = 2 * 1024 * 1024 * 1024
TOTAL_SECONDS = 60
MAX_TEXT_BYTES = 128 * 1024


def private_directory(directory):
    directory = Path(directory).absolute()
    info = directory.lstat()
    if (not stat.S_ISDIR(info.st_mode) or directory.resolve() != directory
            or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o700):
        raise ValueError("Original core directory must be an owned, private real directory")
    return directory


@contextmanager
def original_core_limit(directory):
    """Enable capped cores for the primary launch tree, then restore."""
    import resource  # Linux-only opt-in; other supported platforms never import it.
    directory = private_directory(directory)
    pattern = Path("/proc/sys/kernel/core_pattern").read_text().strip()
    if pattern != str(directory / "core.%p"):
        raise ValueError("Kernel core pattern does not match the private original-core directory")
    previous = resource.getrlimit(resource.RLIMIT_CORE)
    hard = previous[1]
    if hard != resource.RLIM_INFINITY and hard < CORE_BYTES:
        raise ValueError("Original native core capture requires a 2 GiB hard limit")
    # The workflow sets the inherited soft limit to zero. Keeping that contract
    # prevents later clear/diagnostic children from producing unrelated cores.
    if previous[0] != 0:
        raise ValueError("Only the original native launch tree may inherit an enabled core limit")
    resource.setrlimit(resource.RLIMIT_CORE, (CORE_BYTES, hard))
    try:
        yield
    finally:
        resource.setrlimit(resource.RLIMIT_CORE, previous)


def capture(command, deadline, seconds):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        return "Original core diagnostics reached the total time limit.\n", False
    with tempfile.TemporaryFile() as output:
        try:
            result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                    timeout=min(seconds, remaining), check=False)
            status = result.returncode
            ending = f"Diagnostic command status: {status}.\n"
        except subprocess.TimeoutExpired:
            status = None
            ending = "Original core diagnostic command reached its time limit.\n"
        output.seek(0)
        raw = output.read(MAX_TEXT_BYTES + 1)
    text = raw[:MAX_TEXT_BYTES].decode("utf-8", errors="replace")
    if len(raw) > MAX_TEXT_BYTES:
        text += "\nOriginal core diagnostic text truncated at 128 KiB.\n"
    return text + "\n" + ending, status == 0


def collect_original_core(executable, core_directory, pid):
    """Read only core.<original Popen PID>; never execute or attach a process."""
    deadline = time.monotonic() + TOTAL_SECONDS
    report = [f"Original native application core diagnostics (PID {pid})\n"]
    core = None
    try:
        directory = private_directory(core_directory)
        if isinstance(pid, bool) or not isinstance(pid, int) or pid <= 0:
            raise ValueError("Original application PID was not recorded")
        core = directory / f"core.{pid}"
        info = core.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
            raise ValueError("Original core is not an owned regular file")
        if not 0 < info.st_size <= CORE_BYTES:
            raise ValueError("Original core is empty or exceeds the 2 GiB cap")
        binary = Path(executable).resolve(strict=True)
        if binary.name != "OpenToonz" or not binary.is_file() or not os.access(binary, os.X_OK):
            raise ValueError("Original native executable is not the expected OpenToonz binary")
        with binary.open("rb") as source:
            if source.read(4) != b"\x7fELF":
                raise ValueError("Original native executable is not an ELF binary")
        description, identified = capture(["file", "-b", "--", str(core)], deadline, 5)
        names = re.findall(r"\bexecfn:\s*'([^'\r\n]+)'", description)
        if (not identified or len(names) != 1 or not Path(names[0]).is_absolute()
                or Path(names[0]).resolve(strict=True) != binary):
            raise ValueError("Core metadata does not identify the exact original executable")
        report.append("Executable: OpenToonz; exact original PID and executable verified.\n")
        command = ["gdb", "--batch", "--quiet", "--nx", "--nh",
                   "-iex", "set auto-load off", "-iex", "set debuginfod enabled off",
                   "-iex", "set pagination off", "-iex", "set print frame-arguments none",
                   "-iex", "set print entry-values no", "-iex", "set print elements 32",
                   "-iex", "set print max-depth 4", "-ex", "bt 48",
                   "-ex", "thread apply 1-16 bt 16",
                   "--se=" + str(binary), "--core=" + str(core)]
        trace, _ = capture(command, deadline, 45)
        report.append(trace)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report.append(f"Original core diagnostics unavailable: {type(error).__name__}: {error}\n")
    finally:
        if core is not None:
            try:
                # Unlinking a rejected symlink never follows its target. Do not
                # touch other PIDs; the workflow's final cleanup owns those.
                core.unlink(missing_ok=True)
            except OSError as error:
                report.append(f"Original raw core cleanup failed: {type(error).__name__}: {error}\n")
    report.append("The original native process result remains authoritative.\n")
    raw = "".join(report).encode("utf-8", errors="replace")
    if len(raw) > MAX_TEXT_BYTES:
        raw = raw[:MAX_TEXT_BYTES] + b"\nOriginal core report truncated at 128 KiB.\n"
    return raw.decode("utf-8", errors="ignore")
