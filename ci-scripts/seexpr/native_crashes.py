"""Collect bounded text backtraces from original native-test Linux cores.

The workflow deletes every core after this collector returns, including on a
collector failure. Core files are never retained as workflow artifacts.
"""
import argparse
from itertools import islice
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import time


TOTAL_SECONDS = 105
MAX_CORES = 8
MAX_COMMAND_BYTES = 128 * 1024
MAX_REPORT_BYTES = 256 * 1024


def capture(command, deadline, seconds):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        return "Diagnostic collection reached its total time limit.\n", False
    # The debugger's bounded commands normally produce little text. Spooling
    # avoids an unbounded in-memory pipe if a damaged core produces warnings.
    with tempfile.TemporaryFile() as output:
        try:
            result = subprocess.run(command, stdout=output,
                                    stderr=subprocess.STDOUT,
                                    timeout=min(seconds, remaining), check=False)
            status = result.returncode
            ending = f"Diagnostic command status: {status}.\n"
        except subprocess.TimeoutExpired:
            status = None
            ending = "Diagnostic command reached its time limit.\n"
        output.seek(0)
        raw = output.read(MAX_COMMAND_BYTES + 1)
    text = raw[:MAX_COMMAND_BYTES].decode("utf-8", errors="replace")
    if len(raw) > MAX_COMMAND_BYTES:
        text += "\nDiagnostic text truncated at 128 KiB.\n"
    return text + "\n" + ending, status == 0


def executable(description, binary_directory):
    # Linux file(1) obtains execfn from the core's auxiliary-vector note; its
    # separate PRPSINFO command name can be truncated and is not authoritative.
    names = re.findall(r"\bexecfn:\s*'([^'\r\n]+)'", description)
    if len(names) != 1:
        raise ValueError("Core metadata has no unambiguous executable filename")
    candidate = Path(names[0])
    if not candidate.is_absolute():
        raise ValueError("Core executable filename is not an absolute path")
    candidate = candidate.resolve(strict=True)
    if candidate.parent != binary_directory:
        raise ValueError("Core executable is outside the native build binary directory")
    if not re.fullmatch(r"seexpr_[A-Za-z0-9_+-]+|glbsurface_test|OpenToonz|tcomposer",
                        candidate.name):
        raise ValueError("Core executable is not a known native SeExpr test or host")
    if not candidate.is_file() or not os.access(candidate, os.X_OK):
        raise ValueError("Core executable is not an available executable file")
    with candidate.open("rb") as binary:
        header = binary.read(18)
    if len(header) != 18 or header[:4] != b"\x7fELF" or header[5] not in (1, 2):
        raise ValueError("Core executable is not a recognized ELF binary")
    endian = "<" if header[5] == 1 else ">"
    if struct.unpack(endian + "H", header[16:18])[0] not in (2, 3):
        raise ValueError("Core executable is neither an ELF executable nor a PIE")
    return candidate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-directory", required=True, type=Path)
    parser.add_argument("--binary-directory", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    deadline = time.monotonic() + TOTAL_SECONDS
    report = ["Original native SeExpr test crash diagnostics\n"]
    try:
        directory = args.core_directory.resolve(strict=True)
        binaries = args.binary_directory.resolve(strict=True)
        cores = sorted(islice(directory.glob("core.*"), MAX_CORES + 1))
        if not cores:
            report.append("No retained native test core files were produced.\n")
        for index, core in enumerate(cores):
            if index >= MAX_CORES or time.monotonic() >= deadline:
                report.append("Further cores omitted by the eight-core/105-second diagnostic bound.\n")
                break
            if (not re.fullmatch(r"core\.[0-9]+", core.name) or core.is_symlink()
                    or not core.is_file() or core.resolve().parent != directory):
                report.append("Skipped an unexpected entry in the isolated core directory.\n")
                continue
            report.append(f"\nFAIL: original crash {core.name}\n")
            try:
                description, identified = capture(["file", "-b", "--", str(core)], deadline, 5)
                if not identified:
                    raise ValueError("file(1) could not identify the core within its time limit")
                binary = executable(description, binaries)
                report.append(f"Executable: {binary.name}\n")
                command = ["gdb", "--batch", "--quiet", "--nx", "--nh",
                           "-iex", "set auto-load off",
                           "-iex", "set debuginfod enabled off",
                           "-iex", "set pagination off",
                           "-iex", "set print frame-arguments none",
                           "-iex", "set print entry-values no",
                           "-iex", "set print elements 32",
                           "-iex", "set print max-depth 4",
                           "-ex", "bt 48",
                           "-ex", "thread apply 1-16 bt 16",
                           "--se=" + str(binary), "--core=" + str(core)]
                trace, _ = capture(command, deadline, 45)
                report.append(trace)
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                report.append(f"Core diagnostic unavailable: {type(error).__name__}: {error}\n")
    except (OSError, ValueError) as error:
        report.append(f"Core collection unavailable: {type(error).__name__}: {error}\n")
    raw = "".join(report).encode("utf-8", errors="replace")
    if len(raw) > MAX_REPORT_BYTES:
        raw = raw[:MAX_REPORT_BYTES] + b"\nCrash report truncated at 256 KiB.\n"
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(raw.decode("utf-8", errors="ignore"), encoding="utf-8")


if __name__ == "__main__":
    main()
