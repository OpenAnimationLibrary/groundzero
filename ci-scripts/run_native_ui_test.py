"""Launch the native application against a disposable copied data directory."""
import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


@contextmanager
def isolated_fixture(source):
    """Give every launch its own untouched data and identical isolation rules."""
    with tempfile.TemporaryDirectory(prefix="groundzero-native-ui-") as root:
        directory = Path(root).resolve(strict=True)
        shutil.copytree(source, directory / "portablestuff")
        (directory / ".groundzero-native-ui-test").write_bytes(b"groundzero-native-ui-test\n")
        environment = os.environ.copy()
        # Python and Qt/Windows consult temporary-directory variables in
        # different orders. Preserve the native strict-descendant check.
        for key in ("TMPDIR", "TEMP", "TMP"):
            environment[key] = str(directory.parent)
        for key, leaf in (("XDG_CONFIG_HOME", "config"), ("XDG_CACHE_HOME", "cache"),
                          ("XDG_DATA_HOME", "data"), ("APPDATA", "appdata"),
                          ("LOCALAPPDATA", "localappdata")):
            path = directory / leaf
            path.mkdir()
            environment[key] = str(path)
        environment["LIBGL_ALWAYS_SOFTWARE"] = "1"
        yield directory, environment


def decode_output(output):
    return output.decode("utf-8", errors="replace") if isinstance(output, bytes) else output or ""


def windows_crash_diagnostics(executable, source, native_status, symbol_directories):
    kits = os.environ.get("ProgramFiles(x86)")
    if not kits:
        return "Windows SDK debugger location is unavailable.\n"
    debugger = Path(kits) / "Windows Kits" / "10" / "Debuggers" / "x64" / "cdb.exe"
    if not debugger.is_file():
        return "Windows SDK debugger is unavailable; use the flushed native checkpoints.\n"
    symbols = [str(path.resolve()) for path in symbol_directories if path.is_dir()]
    symbols.append(str(executable.parent))
    # Stop before the application's vectored exception handler can show a
    # dialog. The initial exit code is evidence; never substitute this rerun
    # for the original test result. Keep symbol lookup local and bounded.
    commands = (f"sxe av; sxe 0x{native_status & 0xffffffff:08x}; g; "
                ".echo Native integration exception; .lastevent; .exr -1; "
                "lm; ~* kb 8; .ecxr; kv 60; q")
    command = [str(debugger), "-G", "-lines", "-y", ";".join(symbols),
               "-c", commands, str(executable), "--test-native-ui"]
    with isolated_fixture(source) as (directory, environment):
        process = subprocess.Popen(command, cwd=directory, env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            output, _ = process.communicate(timeout=120)
            return (decode_output(output) +
                    f"\nDiagnostic debugger exited with status {process.returncode}. "
                    "The original native test failure remains authoritative.\n")
        except subprocess.TimeoutExpired as error:
            # Terminating only CDB can leave the debuggee holding fixture files.
            # Kill its entire process tree before removing the disposable root.
            notes = "\nNative crash debugger timed out after 120 seconds.\n"
            try:
                subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               timeout=15, check=False)
            except (OSError, subprocess.TimeoutExpired) as cleanup_error:
                notes += f"Debugger process-tree cleanup failed: {type(cleanup_error).__name__}.\n"
            if process.poll() is None:
                process.kill()
            try:
                output, _ = process.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                output = error.output
                process.stdout.close()
                notes += "Debugger output did not close after termination.\n"
            return decode_output(output) + notes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("stuff", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--windows-crash-debugger", action="store_true",
                        help="Capture a separate CDB stack after a Windows native exception")
    parser.add_argument("--debugger-symbols", type=Path, action="append", default=[],
                        help="Local PDB directory for optional Windows crash diagnostics")
    args = parser.parse_args()
    output, status = "", 1
    native_status = None
    try:
        executable = args.executable.resolve(strict=True)
        source = args.stuff.resolve(strict=True)
        with isolated_fixture(source) as (directory, environment):
            try:
                result = subprocess.run([str(executable), "--test-native-ui"],
                                        cwd=directory, env=environment,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        timeout=180, encoding="utf-8", errors="replace")
                output, status = result.stdout, result.returncode
                native_status = status
                if status:
                    output += (f"\nNative application exited with status {status} "
                               f"(0x{status & 0xffffffff:08X}).\n")
            except subprocess.TimeoutExpired as error:
                output = decode_output(error.stdout)
                output += "\nNative application integration test timed out after 180 seconds.\n"
                status = 1
    except (OSError, RuntimeError, ValueError) as error:
        output += ("\nNative application integration setup, launch or cleanup failed: "
                   f"{type(error).__name__}: {error}\n")
        status = 1
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="utf-8")
    except (OSError, ValueError) as error:
        message = f"Could not write native UI integration report: {error}"
        message = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        print(f"::error title=Native UI report::{message}")
        status = 1
    print(output, end="", flush=True)
    if (args.windows_crash_debugger and os.name == "nt" and native_status is not None
            and (native_status & 0xc0000000) == 0xc0000000):
        try:
            diagnostics = windows_crash_diagnostics(executable, source, native_status,
                                                    args.debugger_symbols)
        except (OSError, RuntimeError, ValueError) as error:
            diagnostics = ("Native crash diagnostics could not complete: "
                           f"{type(error).__name__}: {error}\n")
        report = args.output.with_name(args.output.stem + "-debugger.txt")
        try:
            report.write_text(diagnostics, encoding="utf-8")
        except (OSError, ValueError) as error:
            diagnostics += f"Could not save native debugger report: {error}\n"
        print(diagnostics, end="")
    return 0 if status == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
