"""Launch the native application against a disposable copied data directory."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile


@contextmanager
def windows_mesa_renderer(executable, runtime):
    """Temporarily provide desktop GL for both Qt and native GL imports."""
    if runtime is None:
        yield {}
        return
    if os.name != "nt":
        raise ValueError("The Mesa desktop renderer option requires Windows")
    runtime = runtime.resolve(strict=True)
    names = ("opengl32.dll", "libgallium_wgl.dll")
    sources = [runtime / name for name in names]
    targets = [executable.parent / name for name in names]
    marker = executable.with_name(executable.name + ".local")
    if not all(source.is_file() for source in sources):
        raise RuntimeError("The verified Mesa runtime is missing a required DLL")
    if any(path.exists() or path.is_symlink() for path in [*targets, marker]):
        raise RuntimeError("The temporary Mesa renderer would replace a packaged file")
    created = []
    try:
        for source, target in zip(sources, targets):
            with target.open("xb") as destination:
                created.append(target)
                with source.open("rb") as original:
                    shutil.copyfileobj(original, destination)
        with marker.open("xb") as destination:
            created.append(marker)
            destination.write(b"native-ui-test-only\n")
        print("native-ui-integration: renderer: Mesa llvmpipe desktop OpenGL", flush=True)
        # The standard DLL name keeps Qt's desktop WGL/GDI dispatch and native
        # opengl32 imports on the same module. Do not load opengl32sw as well.
        yield {"GALLIUM_DRIVER": "llvmpipe", "QT_OPENGL": "desktop",
               "QT_OPENGL_DLL": "opengl32.dll"}
    finally:
        cleanup_error = None
        for path in reversed(created):
            try:
                path.unlink()
            except OSError as error:
                cleanup_error = error
        if cleanup_error is not None:
            raise RuntimeError("Temporary Mesa renderer cleanup failed") from cleanup_error


@contextmanager
def isolated_fixture(source, executable, mesa_runtime=None):
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
        with windows_mesa_renderer(executable, mesa_runtime) as renderer_environment:
            environment.update(renderer_environment)
            yield directory, environment


def decode_output(output):
    return output.decode("utf-8", errors="replace") if isinstance(output, bytes) else output or ""


def windows_crash_diagnostics(executable, source, native_status, symbol_directories,
                              mesa_runtime=None):
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
    with isolated_fixture(source, executable, mesa_runtime) as (directory, environment):
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


def linux_crash_diagnostics(executable, source):
    debugger = shutil.which("gdb")
    if not debugger:
        return "Linux debugger is unavailable; use the flushed native checkpoints.\n"
    command = [debugger, "--batch", "-ex", "set pagination off", "-ex", "run",
               "-ex", "thread apply all bt 20", "--args", str(executable), "--test-native-ui"]
    with isolated_fixture(source, executable) as (directory, environment):
        process = subprocess.Popen(command, cwd=directory, env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            output, _ = process.communicate(timeout=120)
            return (decode_output(output) +
                    "\nDiagnostic debugger completed. The original native failure remains authoritative.\n")
        except subprocess.TimeoutExpired:
            # Include the inferior; otherwise it can keep disposable files open.
            os.killpg(process.pid, signal.SIGKILL)
            output, _ = process.communicate(timeout=15)
            return decode_output(output) + "\nNative Linux crash debugger timed out.\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("stuff", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--artist-benchmark", action="store_true",
                        help="Measure real drawing, native rendering and resource-save workloads")
    parser.add_argument("--require-function-workflow", action="store_true",
                        help="Require the real application Function Editor fixture checkpoint")
    parser.add_argument("--windows-crash-debugger", action="store_true",
                        help="Capture a separate CDB stack after a Windows native exception")
    parser.add_argument("--linux-crash-debugger", action="store_true",
                        help="Capture a separate GDB stack after a Linux native signal")
    parser.add_argument("--debugger-symbols", type=Path, action="append", default=[],
                        help="Local PDB directory for optional Windows crash diagnostics")
    parser.add_argument("--windows-mesa", type=Path,
                        help="Verified Mesa x64 runtime for temporary Windows desktop GL")
    args = parser.parse_args()
    output, status = "", 1
    native_status = None
    try:
        executable = args.executable.resolve(strict=True)
        source = args.stuff.resolve(strict=True)
        with isolated_fixture(source, executable, args.windows_mesa) as (directory, environment):
            environment.pop("GROUNDZERO_ARTIST_BENCHMARK", None)
            if args.artist_benchmark:
                environment["GROUNDZERO_ARTIST_BENCHMARK"] = "1"
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
    if args.artist_benchmark and status == 0:
        try:
            records = [line.removeprefix("artist-workload-json: ")
                       for line in output.splitlines()
                       if line.startswith("artist-workload-json: ")]
            if len(records) != 1 or "artist-workload: PASS" not in output:
                raise ValueError("Native benchmark did not emit one successful workload report")
            report = json.loads(records[0])
            if report.get("schema") != 1 or report.get("fixture") != "native-artist-workload-v1":
                raise ValueError("Unsupported artist workload report")
            report["build_revision"] = os.environ.get("GITHUB_SHA", "unspecified")
            report["ci_run"] = os.environ.get("GITHUB_RUN_ID", "unspecified")
            report["ci_attempt"] = os.environ.get("GITHUB_RUN_ATTEMPT", "unspecified")
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n",
                                                       encoding="utf-8")
        except (OSError, ValueError) as error:
            output += f"\nArtist workload report validation failed: {error}\n"
            status = 1
    if args.require_function_workflow and not args.artist_benchmark and status == 0 and "function-workflow-native: PASS" not in output:
        output += "\nThe required native Function Editor workflow fixture did not run.\n"
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
    if (args.linux_crash_debugger and not args.artist_benchmark and sys.platform.startswith("linux")
            and native_status is not None and native_status < 0):
        try:
            diagnostics = linux_crash_diagnostics(executable, source)
        except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
            diagnostics = f"Native Linux crash diagnostics failed: {type(error).__name__}: {error}\n"
        report = args.output.with_name(args.output.stem + "-debugger.txt")
        try:
            report.write_text(diagnostics, encoding="utf-8")
        except (OSError, ValueError) as error:
            diagnostics += f"Could not save native debugger report: {error}\n"
        print(diagnostics, end="")
    if (args.windows_crash_debugger and not args.artist_benchmark and os.name == "nt" and native_status is not None
            and (native_status & 0xc0000000) == 0xc0000000):
        try:
            diagnostics = windows_crash_diagnostics(executable, source, native_status,
                                                    args.debugger_symbols, args.windows_mesa)
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
