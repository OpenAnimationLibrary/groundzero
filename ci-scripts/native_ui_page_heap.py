#!/usr/bin/env python3
"""Collect a bounded CDB diagnostic under verified standard Windows page heap.

Only this replay uses page heap and the selected diagnostic suite. Matching OS
symbols are prefetched for two modules; the debugger then uses local symbols
only. The original acceptance failure remains authoritative, and this helper
always returns zero after reporting diagnostic setup, timeout or cleanup errors.
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


def annotation(text):
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("executable", type=Path)
    parser.add_argument("stuff", type=Path)
    parser.add_argument("--symbols", type=Path, required=True)
    parser.add_argument("--suite", default="workflow-switch")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--windows-mesa", type=Path)
    parser.add_argument("--timeout", type=int, default=1200)
    args = parser.parse_args()

    if os.name != "nt":
        print("The page heap diagnostic runs on Windows only.")
        return 0

    output, status = "", None
    notes = []
    state = {"safe_to_release": True}
    enable_attempted = False
    gflags, image = None, None

    def setup_command(command, timeout):
        # Use the same bounded capture and process-tree containment as the
        # replay. Keep partial setup output even when a tool times out.
        tool_state = {"safe_to_release": True}
        try:
            result = run_captured_process(
                command, cwd=args.output.parent.resolve(), environment=os.environ.copy(),
                timeout=timeout, runtime_state=tool_state)
            text = decode_output(result.stdout)
            if result.returncode:
                raise RuntimeError(f"{Path(command[0]).name} exited with status {result.returncode}")
            return text
        finally:
            notes.append(decode_output(tool_state.get("output", b"")))
            if not tool_state["safe_to_release"]:
                notes.append("Diagnostic setup process-tree cleanup was not confirmed.")
                raise RuntimeError("Diagnostic setup tool process-tree exit was not confirmed")

    def run_gflags(*arguments):
        return setup_command([str(gflags), *arguments], 30)

    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        executable = args.executable.resolve(strict=True)
        source = args.stuff.resolve(strict=True)
        symbols = args.symbols.resolve(strict=True)
        image = executable.name
        debugger = Path(os.environ["ProgramFiles(x86)"]) / "Windows Kits/10/Debuggers/x64/cdb.exe"
        gflags = debugger.with_name("gflags.exe")
        symchk = debugger.with_name("symchk.exe")
        for tool in (debugger, gflags, symchk):
            if not tool.is_file():
                raise RuntimeError(f"Windows SDK diagnostic tool is unavailable: {tool.name}")
        cache = args.output.parent.resolve() / "page-heap-os-symbols"
        cache.mkdir(exist_ok=True)
        server = f"srv*{cache}*https://msdl.microsoft.com/download/symbols"
        # Two exact files, at most 120 seconds each; never scan all OS modules.
        # /pt additionally requires the ntdll type information used by !gflag.
        for module in ("ntdll.dll", "verifier.dll"):
            binary = Path(os.environ["SystemRoot"]) / "System32" / module
            command = [str(symchk), "/v", str(binary), "/s", server]
            if module == "ntdll.dll":
                command.append("/pt")
            setup_command(command, 120)

        enable_attempted = True
        run_gflags("/p", "/enable", image)
        query = run_gflags("/p")
        if not any(image.lower() in line.lower() and
                   re.search(r"\(\s*traces\s*\)", line, re.IGNORECASE)
                   for line in query.splitlines()):
            raise RuntimeError("GFlags did not confirm standard page heap (traces)")

        commands = (".reload /f ntdll.dll; .reload /f verifier.dll; "
                    "lmv m ntdll; lmv m verifier; dt ntdll!_PEB NtGlobalFlag; "
                    ".echo Runtime page-heap activation evidence; !gflag; !heap -p; "
                    "sxe av; sxe bpe; sxe 0xc0000374; sxe 0xc0000421; g; "
                    ".echo Native workflow verifier stop; .lastevent; .exr -1; "
                    "r; u @rip-20 L30; ub @rip L20; kv 60; !avrf; !heap -p; "
                    "lm; ~* kb 8; q")
        # Preserve the event thread's live state before switching threads. No
        # network symbol lookups or saved exception context are needed here.
        command = [str(debugger), "-G", "-hd", "-sins", "-netsyms:no", "-noshell", "-lines",
                   "-y", f"{symbols};{executable.parent};srv*{cache}",
                   "-c", commands, str(executable), "--test-native-ui"]
        with isolated_fixture(source, executable, args.windows_mesa,
                              runtime_state=state) as (directory, environment):
            select_diagnostic_suite(environment, args.suite)
            try:
                result = run_captured_process(command, cwd=directory, environment=environment,
                                              timeout=args.timeout, runtime_state=state)
                output, status = decode_output(result.stdout), result.returncode
            except subprocess.TimeoutExpired as error:
                output = decode_output(error.stdout)
                notes.append(f"Page heap replay timed out after {args.timeout} seconds.")
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as error:
        output = output or decode_output(state.get("output", b""))
        notes.append(f"Page heap diagnostic setup, launch or cleanup failed: {type(error).__name__}: {error}")
    finally:
        if enable_attempted:
            try:
                run_gflags("/p", "/disable", image)
                query = run_gflags("/p")
                if any(image.lower() in line.lower() and "enabled" in line.lower()
                       for line in query.splitlines()):
                    raise RuntimeError("GFlags still reports page heap enabled for the replay")
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
                notes.append(f"Page heap cleanup failed: {type(error).__name__}: {error}")
        if not state["safe_to_release"]:
            notes.append("Replay process-tree cleanup was not confirmed; fixture resources were retained.")

    summary = (f"Page heap diagnostic of {args.suite}: "
               + ("no exit status" if status is None else
                  f"CDB status {status} (0x{status & 0xffffffff:08X})"))
    if re.search(r"^\s*hpa\s+-\s+\S", output, re.IGNORECASE | re.MULTILINE):
        summary += ". Runtime !gflag confirmed the page-heap flag."
    else:
        summary += ". Runtime !gflag did not confirm page-heap activation."
    summary += " This is diagnostic evidence only; the original native acceptance failure remains authoritative."
    output = "\n".join(notes) + "\n" + output + "\n" + summary + "\n"
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="utf-8")
    except (OSError, ValueError) as error:
        print(f"Could not save the page heap report: {error}")
    print(output, flush=True)
    print(f"::warning title=Page heap diagnostic ({args.suite})::" + annotation(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
