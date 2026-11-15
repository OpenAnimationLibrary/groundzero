#!/usr/bin/env python3
"""Capture the existing Windows full-page-heap diagnostic under bounded CDB.

Verifier stops can bypass the application's crash reporter. This diagnostic
intercepts those stops in the same replay, retains text only, and restores the
two IFEO values it changes. It never replaces the original acceptance result.
"""

import argparse
import importlib
import os
import re
import subprocess
import sys
from pathlib import Path

from run_native_ui_test import (
    decode_output,
    isolated_fixture,
    run_captured_process,
    select_diagnostic_suite,
)

IFEO = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options"
FLG_HEAP_PAGE_ALLOCS = 0x02000000
PAGE_HEAP_FULL = 0x3
STOP_MARKER = "Page heap diagnostic exception (separate from acceptance)"


class PageHeapSettings:
    """Temporarily enable full page heap without deleting unrelated IFEO state."""

    def __init__(self, image, notes, registry=None):
        self.registry = registry if registry is not None else importlib.import_module("winreg")
        self.image = image
        self.notes = notes
        self.key = None
        self.saved = {}
        self.changed = []

    def __enter__(self):
        registry = self.registry
        access = registry.KEY_QUERY_VALUE | registry.KEY_SET_VALUE | registry.KEY_WOW64_64KEY
        self.key = registry.CreateKeyEx(
            registry.HKEY_LOCAL_MACHINE, IFEO + "\\" + self.image, 0, access
        )
        try:
            for name in ("GlobalFlag", "PageHeapFlags"):
                try:
                    self.saved[name] = registry.QueryValueEx(self.key, name)
                except FileNotFoundError:
                    self.saved[name] = None
            for name, flags in (
                ("GlobalFlag", FLG_HEAP_PAGE_ALLOCS),
                ("PageHeapFlags", PAGE_HEAP_FULL),
            ):
                value, kind = self.saved[name] or (0, registry.REG_DWORD)
                if kind == registry.REG_SZ:
                    value = f"0x{int(str(value), 16) | flags:08x}"
                elif kind == registry.REG_DWORD:
                    value |= flags
                else:
                    raise ValueError(f"Unsupported IFEO value type for {name}")
                self.changed.append(name)
                registry.SetValueEx(self.key, name, 0, kind, value)
            self.notes.append(
                "Full page-heap IFEO values configured; runtime activation unverified."
            )
        except BaseException:
            self.__exit__(*sys.exc_info())
            raise
        return self

    def __exit__(self, exception_type, exception, traceback):
        registry = self.registry
        key = self.key
        if key is None:
            return False
        failed = []
        try:
            for name in reversed(self.changed):
                try:
                    previous = self.saved[name]
                    if previous is None:
                        try:
                            registry.DeleteValue(key, name)
                        except FileNotFoundError:
                            pass
                    else:
                        value, kind = previous
                        registry.SetValueEx(key, name, 0, kind, value)
                except OSError as error:
                    failed.append(f"{name}: {type(error).__name__}")
        finally:
            registry.CloseKey(key)
            self.key = None
        if failed:
            self.notes.append("Page heap IFEO restoration failed: " + ", ".join(failed))
        else:
            self.notes.append("Page heap IFEO values restored; unrelated registry state retained.")
        # Retain even an empty key: deleting a whole key could remove another
        # tool's values or subkeys created while this diagnostic was running.
        return False


def debugger_command(executable, symbols):
    """Return a local-symbol CDB command that stops before native crash handlers."""
    kits = os.environ.get("ProgramFiles(x86)")
    if not kits:
        raise RuntimeError("Windows SDK debugger location is unavailable")
    debugger = Path(kits) / "Windows Kits/10/Debuggers/x64/cdb.exe"
    if not debugger.is_file():
        raise RuntimeError("Windows SDK CDB is unavailable; no page-heap replay was launched")
    paths = [str(path.resolve(strict=True)) for path in symbols]
    paths.append(str(executable.parent))
    # Initial !gflag/!heap queries previously stalled before the application ran.
    # Capture the stopped thread before switching to the other thread stacks.
    commands = (
        "sxe av; sxe bpe; sxe 0xc0000374; sxe 0xc0000421; g; "
        f".echo {STOP_MARKER}; .lastevent; .exr -1; r; kv 60; "
        "lm; ~* kb 8; q"
    )
    return [
        str(debugger),
        "-G",
        "-hd",
        "-sins",
        "-netsyms:no",
        "-noshell",
        "-lines",
        "-y",
        ";".join(paths),
        "-c",
        commands,
        str(executable),
        "--test-native-ui",
    ]


def collect_diagnostic(args, registry=None):
    """Run one contained diagnostic and retain timeout or setup evidence."""
    notes, output, status = [], "", None
    state = {"safe_to_release": True}
    try:
        executable = args.executable.resolve(strict=True)
        source = args.stuff.resolve(strict=True)
        command = debugger_command(executable, args.symbols)
        with PageHeapSettings(executable.name, notes, registry):
            with isolated_fixture(source, executable, args.windows_mesa, runtime_state=state) as (
                directory,
                environment,
            ):
                select_diagnostic_suite(environment, args.suite)
                for name in ("_NT_SYMBOL_PATH", "_NT_ALT_SYMBOL_PATH", "_NT_EXECUTABLE_IMAGE_PATH"):
                    environment.pop(name, None)
                try:
                    result = run_captured_process(
                        command,
                        cwd=directory,
                        environment=environment,
                        timeout=args.timeout,
                        runtime_state=state,
                    )
                    output, status = decode_output(result.stdout), result.returncode
                except subprocess.TimeoutExpired as error:
                    output = decode_output(error.output or state.get("output", b""))
                    notes.append(f"Page heap CDB timed out after {args.timeout} seconds.")
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        output = output or decode_output(state.get("output", b""))
        notes.append(
            f"Page heap diagnostic setup or launch failed: {type(error).__name__}: {error}"
        )
    if not state["safe_to_release"]:
        notes.append("Diagnostic process-tree retirement unconfirmed; fixture resources retained.")
    summary = f"Page heap diagnostic of {args.suite}: " + (
        "no debugger exit status" if status is None else f"CDB status {status}"
    )
    summary += ". Debugger exit is not an application acceptance result."
    summary += " The original native acceptance failure remains authoritative."
    return "\n".join(notes) + "\n" + output + "\n" + summary + "\n", summary


def annotation(text):
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def exception_excerpt(output):
    """Select actual debugger output, excluding its echoed command string."""
    match = re.search(r"(?m)^" + re.escape(STOP_MARKER) + r"\r?$", output)
    if not match:
        return "No exception stop marker was captured; inspect the retained diagnostic text."
    text = output[match.end() :].encode("utf-8")[:2800]
    return text.decode("utf-8", errors="ignore")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("executable", type=Path)
    parser.add_argument("stuff", type=Path)
    parser.add_argument("--symbols", type=Path, action="append", default=[])
    parser.add_argument("--suite", default="workflow-switch")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--windows-mesa", type=Path)
    parser.add_argument("--timeout", type=int, default=1500)
    args = parser.parse_args()
    if not 1 <= args.timeout <= 1500:
        parser.error("--timeout must be between 1 and 1500 seconds")
    if os.name != "nt":
        print("The page heap diagnostic runs on Windows only.")
        return 0
    output, summary = collect_diagnostic(args)
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="utf-8")
    except (OSError, ValueError) as error:
        print(f"Could not save the page heap report: {error}")
    print(output, flush=True)
    print(
        "::warning title=Page heap diagnostic::"
        + annotation(summary + "\n" + exception_excerpt(output))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
