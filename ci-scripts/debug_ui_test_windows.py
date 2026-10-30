"""Collect a bounded stack for an already-failed native widget test.

This diagnostic never replaces the original CTest result. It reruns only the
crashed fixture, with the same Windows Qt platform and packaged libraries.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


MAX_REPORT_BYTES = 256 * 1024
TESTS = {
    "ui_functionworkflow": ("ui_functionworkflow_test.exe", "toonzqt"),
    "ui_vector_profilemanager": ("widthprofilemanager_test.exe", "tnztools/tests"),
}


def collect(build, qt, test_name):
    kits = os.environ.get("ProgramFiles(x86)")
    if not kits:
        return "Windows SDK debugger location is unavailable; use the flushed checkpoints.\n"
    debugger = Path(kits) / "Windows Kits" / "10" / "Debuggers" / "x64" / "cdb.exe"
    if not debugger.is_file():
        return "Windows SDK debugger is unavailable; use the flushed checkpoints.\n"
    runtime = build / "RelWithDebInfo"
    executable_name, test_directory = TESTS[test_name]
    executable = runtime / executable_name
    package = build / "Opentoonz"
    if not executable.is_file() or not package.is_dir():
        return "The crashed widget executable or packaged runtime is unavailable.\n"
    environment = os.environ.copy()
    environment["PATH"] = os.pathsep.join((str(package), str(qt / "bin"), environment.get("PATH", "")))
    environment["QT_PLUGIN_PATH"] = str(qt / "plugins")
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(qt / "plugins" / "platforms")
    environment["QT_QPA_PLATFORM"] = "windows"
    # Restrict symbols to this build. Stop at the actual access violation (or
    # heap/stack corruption), before a handler can replace the original stack.
    commands = ("sxe av; sxe 0xc0000374; sxe 0xc0000409; g; "
                ".echo Native widget exception; .lastevent; .exr -1; "
                "lm; ~* kb 8; .ecxr; kv 60; q")
    command = [str(debugger), "-G", "-lines", "-y", str(runtime),
               "-c", commands, str(executable)]
    notes = ""
    with tempfile.TemporaryFile() as output:
        process = subprocess.Popen(command, cwd=build / test_directory, env=environment,
                                   stdout=output, stderr=subprocess.STDOUT)
        try:
            process.wait(timeout=120)
        except subprocess.TimeoutExpired:
            notes += "\nWidget debugger reached its 120-second deadline.\n"
            # Kill the debuggee too, so its isolated fixture storage can close.
            try:
                subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               timeout=15, check=False)
            except (OSError, subprocess.TimeoutExpired) as problem:
                notes += f"Debugger process-tree cleanup failed: {type(problem).__name__}.\n"
            if process.poll() is None:
                process.kill()
            process.wait(timeout=15)
        output.seek(0)
        raw = output.read(MAX_REPORT_BYTES + 1)
    text = raw[:MAX_REPORT_BYTES].decode("utf-8", errors="replace")
    if len(raw) > MAX_REPORT_BYTES:
        notes += "\nDebugger text truncated at 256 KiB.\n"
    return (text + notes + f"\nDiagnostic debugger status: {process.returncode}. "
            "The original CTest failure remains authoritative.\n")


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--qt", required=True, type=Path)
    parser.add_argument("--test", required=True, choices=TESTS)
    parser.add_argument("--test-output", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    report = args.test_output.read_text(encoding="utf-8", errors="replace")
    if not re.search(re.escape(args.test) + r"\s[^\r\n]*(?:Exception|SegFault)", report):
        return
    try:
        text = collect(args.build.resolve(strict=True), args.qt.resolve(strict=True), args.test)
    except (OSError, subprocess.SubprocessError) as problem:
        text = f"Widget debugger could not collect a stack: {type(problem).__name__}.\n"
    text = f"Native widget diagnostic: {args.test}\n" + text
    args.output.write_text(text, encoding="utf-8")
    print(text, end="")


if __name__ == "__main__":
    main()
