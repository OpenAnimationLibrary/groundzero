"""Bounded, non-executing diagnostics for the original Linux native UI crash.

The workflow owns kernel.core_pattern and removes the private directory on exit.
Only the primary application launch tree temporarily enables core dumps. No replay process,
raw core, locals, or argument values are published by this helper.
"""

import os
import re
import stat
import subprocess
import tempfile
import time
from contextlib import contextmanager
from pathlib import Path

CORE_BYTES = 2 * 1024 * 1024 * 1024
TOTAL_SECONDS = 60
MAX_TEXT_BYTES = 128 * 1024
# file(1) stops parsing a core past 2048 program headers or 256 notes, before it
# reaches the auxv note that names the executable. A Qt application core has more
# mappings than that; 65535 is the largest count e_phnum can express.
FILE_ELF_LIMITS = ("-P", "elf_phnum=65535", "-P", "elf_notes=65535")


def private_directory(directory):
    directory = Path(directory).absolute()
    info = directory.lstat()
    if (
        not stat.S_ISDIR(info.st_mode)
        or directory.resolve() != directory
        or info.st_uid != os.getuid()
        or stat.S_IMODE(info.st_mode) != 0o700
    ):
        raise ValueError(
            "Original core directory must be an owned, private real directory"
        )
    return directory


@contextmanager
def original_core_limit(directory):
    """Enable capped cores for the primary launch tree, then restore."""
    import resource  # Linux-only opt-in; other supported platforms never import it.

    directory = private_directory(directory)
    pattern = Path("/proc/sys/kernel/core_pattern").read_text().strip()
    if pattern != str(directory / "core.%p"):
        raise ValueError(
            "Kernel core pattern does not match the private original-core directory"
        )
    previous = resource.getrlimit(resource.RLIMIT_CORE)
    hard = previous[1]
    if hard != resource.RLIM_INFINITY and hard < CORE_BYTES:
        raise ValueError("Original native core capture requires a 2 GiB hard limit")
    # The workflow sets the inherited soft limit to zero. Keeping that contract
    # prevents later clear/diagnostic children from producing unrelated cores.
    if previous[0] != 0:
        raise ValueError(
            "Only the original native launch tree may inherit an enabled core limit"
        )
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
            result = subprocess.run(
                command,
                stdout=output,
                stderr=subprocess.STDOUT,
                timeout=min(seconds, remaining),
                check=False,
            )
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


def executable_identity_error(names, binary):
    """Classify an execfn rejection without exposing paths or command arguments."""
    if not names:
        return "execfn-absent"
    if len(names) != 1:
        return "execfn-ambiguous"
    candidate = Path(names[0])
    if not candidate.is_absolute():
        return "execfn-relative"
    try:
        resolved = candidate.resolve(strict=True)
    except OSError:
        return "execfn-unavailable"
    if resolved != binary:
        return "execfn-mismatch"
    return None


def validate_original_files(executable, core):
    """Validate the bounded core and expected ELF binary before reading metadata."""
    info = core.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
        raise ValueError("Original core is not an owned regular file")
    if not 0 < info.st_size <= CORE_BYTES:
        raise ValueError("Original core is empty or exceeds the 2 GiB cap")
    binary = Path(executable).resolve(strict=True)
    if (
        binary.name != "OpenToonz"
        or not binary.is_file()
        or not os.access(binary, os.X_OK)
    ):
        raise ValueError(
            "Original native executable is not the expected OpenToonz binary"
        )
    with binary.open("rb") as source:
        if source.read(4) != b"\x7fELF":
            raise ValueError("Original native executable is not an ELF binary")
    return binary, info


def original_backtrace(binary, core, deadline):
    """Read bounded stacks without loading debugger scripts or argument values."""
    command = [
        "gdb",
        "--batch",
        "--quiet",
        "--nx",
        "--nh",
        "-iex",
        "set auto-load off",
        "-iex",
        "set debuginfod enabled off",
        "-iex",
        "set pagination off",
        "-iex",
        "set print frame-arguments none",
        "-iex",
        "set print entry-values no",
        "-iex",
        "set print elements 32",
        "-iex",
        "set print max-depth 4",
        "-ex",
        "bt 48",
        "-ex",
        "thread apply 1-16 bt 16",
        "--se=" + str(binary),
        "--core=" + str(core),
    ]
    trace, _ = capture(command, deadline, 45)
    return trace


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
        binary, info = validate_original_files(executable, core)
        description, identified = capture(
            ["file", "-b", *FILE_ELF_LIMITS, "--", str(core)], deadline, 5
        )
        names = re.findall(r"\bexecfn:\s*'([^'\r\n]+)'", description)
        limits = re.findall(
            r"too many (program headers|notes) \(([0-9]+)\)", description
        )
        limit_summary = (
            ", ".join(f"{kind}={count}" for kind, count in limits) or "none reported"
        )
        report.append(
            f"Core identity metadata: bytes={info.st_size}; cap={CORE_BYTES}; "
            f"file_command_success={identified}; execfn_count={len(names)}; "
            f"metadata_limits={limit_summary}.\n"
        )
        rejection = (
            executable_identity_error(names, binary)
            if identified
            else "file-command-failed-or-timed-out"
        )
        if rejection:
            raise ValueError(
                "Core metadata does not identify the exact original executable "
                f"({rejection})"
            )
        report.append(
            "Executable: OpenToonz; exact original PID and executable verified.\n"
        )
        report.append(original_backtrace(binary, core, deadline))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report.append(
            f"Original core diagnostics unavailable: {type(error).__name__}: {error}\n"
        )
    finally:
        if core is not None:
            try:
                # Unlinking a rejected symlink never follows its target. Do not
                # touch other PIDs; the workflow's final cleanup owns those.
                core.unlink(missing_ok=True)
            except OSError as error:
                report.append(
                    f"Original raw core cleanup failed: {type(error).__name__}: {error}\n"
                )
    report.append("The original native process result remains authoritative.\n")
    raw = "".join(report).encode("utf-8", errors="replace")
    if len(raw) > MAX_TEXT_BYTES:
        raw = raw[:MAX_TEXT_BYTES] + b"\nOriginal core report truncated at 128 KiB.\n"
    return raw.decode("utf-8", errors="ignore")
