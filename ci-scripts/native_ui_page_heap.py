#!/usr/bin/env python3
"""Rerun one native UI diagnostic suite under the Windows full page heap.

The native UI integration test crashes on Windows inside unrelated destructors
(QTextEngine, QItemSelectionModel) while the rooms of a workflow are torn down:
memory is damaged earlier, by code the crash stack does not show. With the
full page heap (Image File Execution Options, as gflags /p /enable /full sets
it) every heap block ends at a guard page and freed blocks stay inaccessible,
so the first write past a block, or any use of a freed one, stops the process
where it happens. The application's unattended crash report then names the
code at fault.

This is a diagnostic: it never changes the original test result. It prints
the crash report as a GitHub annotation and exits 0 in every case. The page
heap setting is removed again before the script returns.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_native_ui_test import (decode_output, isolated_fixture,  # noqa: E402
                                run_captured_process, select_diagnostic_suite)

IFEO = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options"
FLG_HEAP_PAGE_ALLOCS = 0x02000000
PAGE_HEAP_FULL = 0x3  # full page heap, with stack traces of the allocations


def enable_page_heap(image):
    import winreg
    key = winreg.CreateKeyEx(winreg.HKEY_LOCAL_MACHINE, IFEO + "\\" + image, 0,
                             winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY)
    with key:
        winreg.SetValueEx(key, "GlobalFlag", 0, winreg.REG_DWORD, FLG_HEAP_PAGE_ALLOCS)
        winreg.SetValueEx(key, "PageHeapFlags", 0, winreg.REG_DWORD, PAGE_HEAP_FULL)


def disable_page_heap(image):
    import winreg
    try:
        winreg.DeleteKeyEx(winreg.HKEY_LOCAL_MACHINE, IFEO + "\\" + image,
                           winreg.KEY_WOW64_64KEY, 0)
    except FileNotFoundError:
        pass


def annotation(text):
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("executable", type=Path)
    parser.add_argument("stuff", type=Path)
    parser.add_argument("--suite", default="workflow-switch")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--windows-mesa", type=Path)
    parser.add_argument("--timeout", type=int, default=1500)
    args = parser.parse_args()

    if os.name != "nt":
        print("The page heap diagnostic runs on Windows only.")
        return 0

    executable = args.executable.resolve(strict=True)
    source = args.stuff.resolve(strict=True)
    image = executable.name
    output, status = "", None
    state = {"safe_to_release": True}
    try:
        enable_page_heap(image)
        with isolated_fixture(source, executable, args.windows_mesa,
                              runtime_state=state) as (directory, environment):
            select_diagnostic_suite(environment, args.suite)
            try:
                result = run_captured_process([str(executable), "--test-native-ui"],
                                              cwd=directory, environment=environment,
                                              timeout=args.timeout, runtime_state=state)
                output, status = decode_output(result.stdout), result.returncode
            except subprocess.TimeoutExpired as error:
                output = decode_output(error.stdout)
                output += f"\nPage heap diagnostic timed out after {args.timeout} seconds.\n"
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        output += f"\nPage heap diagnostic setup or launch failed: {type(error).__name__}: {error}\n"
    finally:
        disable_page_heap(image)

    summary = (f"Page heap diagnostic of {args.suite}: "
               + ("no exit status" if status is None else
                  f"status {status} (0x{status & 0xffffffff:08X})"))
    output += "\n" + summary + "\n"
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="utf-8")
    except (OSError, ValueError) as error:
        print(f"Could not save the page heap report: {error}")
    print(output)

    # The crash report of the application: its reason and its first frames,
    # where the page heap stopped the faulting access
    report = ""
    match = re.search(r"Crash Reason:.*", output)
    if match:
        frames = re.findall(r"^\s*\d+> .*$", output[match.start():], re.M)
        report = match.group(0) + "\n" + "\n".join(frames[:45])
    print(f"::warning title=Page heap diagnostic ({args.suite})::"
          + annotation(summary + ("\n" + report if report else
                                  "\nNo crash report in the output; see the step log.")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
