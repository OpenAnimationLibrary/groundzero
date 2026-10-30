"""Launch the native application against a disposable copied data directory."""
import argparse
from contextlib import contextmanager, nullcontext
import json
import math
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from windows_process import WindowsJobProcess


def original_macos_identity_witness(identity, native_status):
    """Publish only bounded numeric identity, never an executable/local path."""
    def scalar(value):
        if type(value) not in (int, float):
            return "unavailable"
        try:
            return format(value, ".17g") if math.isfinite(value) else "unavailable"
        except OverflowError:
            return "unavailable"
    _, pid, started_at, finished_at = identity
    return ("Original macOS identity: "
            f"pid={scalar(pid)} status={scalar(native_status)} "
            f"started_at={scalar(started_at)} finished_at={scalar(finished_at)}\n")


def write_macos_crash_text(path, text):
    """Replace a complete report atomically, retaining old evidence on failure."""
    temporary = None
    diagnostic = ""
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="", dir=path.parent,
                                         prefix=f".{path.name}.", suffix=".tmp",
                                         delete=False) as destination:
            temporary = Path(destination.name)
            destination.write(text)
        # The sibling file is closed successfully before replacing the old
        # report, so a partial write or failed replace cannot truncate it.
        os.replace(temporary, path)
    except (OSError, ValueError) as error:
        diagnostic = (f"Could not save original macOS crash report: {type(error).__name__} "
                      f"(errno={getattr(error, 'errno', None)}).\n")
    finally:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError as error:
                diagnostic += (f"Could not remove temporary macOS crash report: {type(error).__name__} "
                               f"(errno={getattr(error, 'errno', None)}).\n")
    return diagnostic


@contextmanager
def windows_mesa_renderer(executable, runtime, cleanup_allowed=None):
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
    markers = [executable.with_name(executable.name + ".local")]
    converter = executable.with_name("tconverter.exe")
    if converter.is_file():
        markers.append(converter.with_name(converter.name + ".local"))
    if not all(source.is_file() for source in sources):
        raise RuntimeError("The verified Mesa runtime is missing a required DLL")
    if any(path.exists() or path.is_symlink() for path in [*targets, *markers]):
        raise RuntimeError("The temporary Mesa renderer would replace a packaged file")
    created = []
    try:
        for source, target in zip(sources, targets):
            with target.open("xb") as destination:
                created.append(target)
                with source.open("rb") as original:
                    shutil.copyfileobj(original, destination)
        for marker in markers:
            with marker.open("xb") as destination:
                created.append(marker)
                destination.write(b"native-ui-test-only\n")
        print("native-ui-integration: renderer: Mesa llvmpipe desktop OpenGL", flush=True)
        # The standard DLL name keeps Qt's desktop WGL/GDI dispatch and native
        # opengl32 imports on the same module. Do not load opengl32sw as well.
        yield {"GALLIUM_DRIVER": "llvmpipe", "QT_OPENGL": "desktop",
               "QT_OPENGL_DLL": "opengl32.dll"}
    finally:
        if cleanup_allowed is not None and not cleanup_allowed():
            raise RuntimeError(
                "Temporary Mesa renderer retained: renderer descendants may still be running")
        cleanup_error = None
        cleanup_details = []
        for path in reversed(created):
            try:
                path.unlink()
            except OSError as error:
                cleanup_error = error
                # The runner reports str(error), not its chained traceback.
                # Keep bounded OS evidence without exposing the full path.
                reason = str(error.strerror or type(error).__name__)
                reason = reason.replace("\r", " ").replace("\n", " ")[:240]
                cleanup_details.append(
                    f"{path.name[:128]}: {type(error).__name__} "
                    f"(errno={error.errno}, winerror={getattr(error, 'winerror', None)}): {reason}")
        if cleanup_error is not None:
            raise RuntimeError("Temporary Mesa renderer cleanup failed: " +
                               "; ".join(cleanup_details)) from cleanup_error


def stop_process_tree(process):
    """Bound the complete tree cleanup to 15 seconds; uncertainty is not success."""
    if isinstance(process, WindowsJobProcess):
        return process.retire(timeout=15)
    deadline = time.monotonic() + 15
    stopped = False
    notes = b""
    try:
        if os.name == "nt":
            result = subprocess.run(
                ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=12, check=False)
            notes += result.stdout or b""
            stopped = result.returncode == 0
            if not stopped:
                notes += f"\nProcess-tree cleanup exited with {result.returncode}.\n".encode()
        else:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass  # This entire process group has already exited.
            stopped = True
    except (OSError, subprocess.TimeoutExpired) as error:
        notes += f"\nProcess-tree cleanup failed: {type(error).__name__}.\n".encode()
    try:
        if process.poll() is None:
            process.kill()
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            if process.poll() is None:
                raise subprocess.TimeoutExpired("process cleanup", 15)
        else:
            process.wait(timeout=remaining)
    except (OSError, subprocess.TimeoutExpired) as error:
        stopped = False
        notes += f"\nParent cleanup failed: {type(error).__name__}.\n".encode()
    if stopped and os.name != "nt":
        # Signal delivery is not process exit. Do not release shared files
        # until the entire group, including converter descendants, is gone.
        stopped = False
        while time.monotonic() < deadline:
            try:
                os.killpg(process.pid, 0)
            except ProcessLookupError:
                stopped = True
                break
            except OSError as error:
                notes += f"\nProcess-group verification failed: {type(error).__name__}.\n".encode()
                break
            time.sleep(min(0.05, max(0, deadline - time.monotonic())))
        if not stopped:
            notes += b"\nProcess-group exit was not confirmed within the cleanup deadline.\n"
    return stopped, notes


def launch_captured_process(command, **options):
    if os.name == "nt":
        return WindowsJobProcess(command, cwd=options["cwd"], env=options["env"],
                                 stdout=options["stdout"])
    return subprocess.Popen(command, **options)


def run_captured_process(command, *, cwd, environment, timeout, runtime_state):
    """Capture flushed bytes even if a Windows descendant prevents pipe EOF.

    The capture lives in the disposable fixture. An independent bounded read
    avoids changing the inherited writer's file position. No pipe-reader thread
    or unbounded close/wait is involved in the timeout path.
    """
    descriptor, name = tempfile.mkstemp(prefix="native-process-", suffix=".log", dir=cwd)
    capture = Path(name)
    runtime_state["capture_path"] = capture
    notes = b""
    process = None

    def captured():
        with capture.open("rb") as source:
            output = source.read(16 * 1024 * 1024 + 1)
        if len(output) > 16 * 1024 * 1024:
            output = output[:16 * 1024 * 1024] + b"\nCaptured output truncated at 16 MiB.\n"
        runtime_state["output"] = notes + output
        return runtime_state["output"]

    try:
        with os.fdopen(descriptor, "wb", buffering=0) as destination:
            runtime_state["started_at"] = time.time()
            process = launch_captured_process(command, cwd=cwd, env=environment,
                                       stdout=destination, stderr=subprocess.STDOUT,
                                       start_new_session=os.name != "nt")
            # This is the directly launched OpenToonz PID. xvfb-run wraps this
            # Python runner outside it; no shell or GL wrapper owns this PID.
            runtime_state["pid"] = process.pid
            runtime_state["safe_to_release"] = False
            try:
                status = process.wait(timeout=timeout)
                runtime_state["finished_at"] = time.time()
            except subprocess.TimeoutExpired:
                safe, notes = stop_process_tree(process)
                runtime_state["safe_to_release"] = safe
                runtime_state["finished_at"] = time.time()
                raise subprocess.TimeoutExpired(command, timeout, output=captured())
            except BaseException:
                safe, notes = stop_process_tree(process)
                runtime_state["safe_to_release"] = safe
                runtime_state["finished_at"] = time.time()
                captured()
                raise
            if status != 0 or isinstance(process, WindowsJobProcess):
                # A crashed parent can leave converter descendants behind even
                # though wait() returned. Keep its exact status/output, and do
                # not release shared files unless tree exit is confirmed.
                safe, notes = stop_process_tree(process)
                runtime_state["safe_to_release"] = safe
                if not safe:
                    notes += (f"\nOriginal process exited with status {status} "
                              f"(0x{status & 0xffffffff:08X}); descendant exit remains uncertain.\n").encode()
            else:
                runtime_state["safe_to_release"] = True
            return subprocess.CompletedProcess(command, status, stdout=captured())
    finally:
        if isinstance(process, WindowsJobProcess):
            process.close()
        if runtime_state["safe_to_release"]:
            capture.unlink()


@contextmanager
def isolated_fixture(source, executable, mesa_runtime=None, runtime_state=None):
    """Give every launch its own untouched data and identical isolation rules."""
    state = runtime_state if runtime_state is not None else {"safe_to_release": True}
    root = tempfile.mkdtemp(prefix="groundzero-native-ui-")
    try:
        directory = Path(root).resolve(strict=True)
        shutil.copytree(source, directory / "portablestuff")
        (directory / ".groundzero-native-ui-test").write_bytes(b"groundzero-native-ui-test\n")
        environment = os.environ.copy()
        # Every ordinary, benchmark and debugger launch starts unselected.
        # Only an explicit bounded diagnostic/lifecycle launch selects a suite.
        environment.pop("GROUNDZERO_NATIVE_UI_SUITE", None)
        environment.pop("GROUNDZERO_ARTIST_BENCHMARK", None)
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
        with windows_mesa_renderer(executable, mesa_runtime,
                                   cleanup_allowed=lambda: state["safe_to_release"]) as renderer_environment:
            environment.update(renderer_environment)
            yield directory, environment
    finally:
        if not state["safe_to_release"]:
            raise RuntimeError(
                "Disposable fixture data and any temporary renderer retained: "
                "process-tree cleanup could not confirm all descendants stopped")
        shutil.rmtree(root)


def decode_output(output):
    return output.decode("utf-8", errors="replace") if isinstance(output, bytes) else output or ""


def select_diagnostic_suite(environment, suite):
    if suite is None:
        return
    if suite not in ("function-workflow", "cast-workflow", "combined-timing",
                     "combined-cast-vector", "combined-fidelity", "vector-renderer"):
        raise ValueError("Unknown native UI diagnostic suite")
    environment["GROUNDZERO_NATIVE_UI_SUITE"] = suite


def missing_icon_lifecycle_checkpoints(output, terminal="replacement"):
    # Match application output lines, never an echoed debugger command or the
    # earlier fixture's PASS. Process status is checked independently by callers.
    lines = output.splitlines()
    marker = "icon-lifecycle-native: PASS"
    required = {
        f"icon-lifecycle-native: terminal {terminal} PASS":
            f"icon-lifecycle-native: terminal {terminal} PASS" in lines,
        marker: any(line == marker or line.startswith(marker + " (") for line in lines),
        "native-ui-integration: checkpoint: native icon workers drained":
            "native-ui-integration: checkpoint: native icon workers drained" in lines,
        "native-ui-integration: checkpoint: native application cleanup completed":
            "native-ui-integration: checkpoint: native application cleanup completed" in lines,
    }
    return [marker for marker, observed in required.items() if not observed]

def required_icon_clear(executable, source, output_path, mesa_runtime=None,
                        runtime_state=None):
    """Require the other terminal Quit case in its own disposable process."""
    if runtime_state is not None and not runtime_state["safe_to_release"]:
        return ("\nRequired reviewed-clear lifecycle was not launched: "
                "process-tree exit was not confirmed.\n", 1)
    state = {"safe_to_release": True}
    output, status = "", 1
    try:
        with isolated_fixture(source, executable, mesa_runtime,
                              runtime_state=state) as (directory, environment):
            # This mode runs only the lifecycle fixture. Never inherit a
            # feature-suite selector or the independent benchmark bypass.
            environment.pop("GROUNDZERO_NATIVE_UI_SUITE", None)
            environment.pop("GROUNDZERO_ARTIST_BENCHMARK", None)
            try:
                result = run_captured_process([str(executable), "--test-native-icon-clear"],
                                              cwd=directory, environment=environment,
                                              timeout=120, runtime_state=state)
                output, status = decode_output(result.stdout), result.returncode
                if status:
                    output += (f"\nReviewed-clear native process exited with status {status} "
                               f"(0x{status & 0xffffffff:08X}).\n")
                if status == 0:
                    missing = missing_icon_lifecycle_checkpoints(output, "reviewed-clear")
                    if missing:
                        output += ("\nMissing required reviewed-clear lifecycle checkpoints: " +
                                   ", ".join(missing) + "\n")
                        status = 1
            except subprocess.TimeoutExpired as error:
                output = decode_output(error.stdout)
                output += "\nRequired reviewed-clear lifecycle timed out after 120 seconds.\n"
                status = 1
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        output = output or decode_output(state.get("output", b""))
        output += ("\nRequired reviewed-clear lifecycle setup, launch or cleanup failed: "
                   f"{type(error).__name__}: {error}\n")
        status = 1
    if not state["safe_to_release"] and runtime_state is not None:
        runtime_state["safe_to_release"] = False
    output = ("\n--- Required reviewed-clear lifecycle (fresh process) ---\n" + output +
              f"\nRequired reviewed-clear lifecycle: {'PASS' if status == 0 else 'FAIL'} "
              f"(status {status}).\n")
    report = output_path.with_name(output_path.stem + "-icon-clear.txt")
    try:
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(output, encoding="utf-8")
    except (OSError, ValueError) as error:
        output += f"Could not save required reviewed-clear lifecycle report: {error}\n"
        status = 1
    return output, 0 if status == 0 else 1


def combined_failure_diagnostics(executable, source, output_path, mesa_runtime=None,
                                 linux_debugger=False, windows_debugger=False,
                                 symbol_directories=(), runtime_state=None, macos_debugger=False):
    """Collect isolated evidence without changing the original failed result."""
    suites = (("function-workflow", "function-workflow-native: PASS"),
              ("cast-workflow", "cast-native-workflow: PASS"),
              ("combined-timing", "function-timing-native: PASS"),
              ("combined-cast-vector", "cast-vector-native: PASS"),
              ("combined-fidelity", "combined-fidelity-native: PASS"),
              ("vector-renderer", "vector-renderer-native: PASS"))
    timeout_seconds = 120
    reports, results = [], []
    for suite, marker in suites:
        label = f"Independent native diagnostic: {suite}"
        print(f"\n{label} (not an all-workflows qualification).", flush=True)
        output, status, native_status = "", 1, None
        state = {"safe_to_release": True}
        try:
            # These launches must remain sequential: the Windows renderer
            # installs and removes temporary DLLs beside the shared executable.
            with isolated_fixture(source, executable, mesa_runtime,
                                  runtime_state=state) as (directory, environment):
                select_diagnostic_suite(environment, suite)
                try:
                    result = run_captured_process([str(executable), "--test-native-ui"],
                                                  cwd=directory, environment=environment,
                                                  timeout=timeout_seconds, runtime_state=state)
                    output, status = decode_output(result.stdout), result.returncode
                    native_status = status
                    required = (marker, f"native-ui-diagnostic: PASS {suite}")
                    missing = ([value for value in required if value not in output] +
                               missing_icon_lifecycle_checkpoints(output))
                    if status == 0 and missing:
                        output += "\nMissing isolated diagnostic checkpoints: " + ", ".join(missing) + "\n"
                        status = 1
                except subprocess.TimeoutExpired as error:
                    output = decode_output(error.stdout)
                    output += f"\nIsolated diagnostic timed out after {timeout_seconds} seconds.\n"
                    status = 1
        except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
            output = output or decode_output(state.get("output", b""))
            output += ("\nIsolated diagnostic setup, launch or cleanup failed: "
                       f"{type(error).__name__}: {error}\n")
            status = 1
        # A crash in an isolated fixture must be diagnosed in that same
        # fixture, rather than silently rerunning the earlier all-suite path.
        # Each helper creates another fresh profile and preserves the failure.
        debugger_output = None
        try:
            if (state["safe_to_release"] and linux_debugger and sys.platform.startswith("linux") and
                    native_status is not None and native_status < 0):
                debugger_output = linux_crash_diagnostics(executable, source, suite, runtime_state=state)
            elif (state["safe_to_release"] and macos_debugger and sys.platform == "darwin" and
                  native_status is not None and native_status < 0):
                debugger_output = macos_crash_diagnostics(executable, source, suite, runtime_state=state)
            elif (state["safe_to_release"] and windows_debugger and os.name == "nt" and native_status is not None and
                  (native_status & 0xc0000000) == 0xc0000000):
                debugger_output = windows_crash_diagnostics(
                    executable, source, native_status, symbol_directories, mesa_runtime, suite,
                    runtime_state=state)
        except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
            debugger_output = f"Isolated crash diagnostics failed: {type(error).__name__}: {error}\n"
        if debugger_output is not None:
            debugger_report = output_path.with_name(output_path.stem + f"-{suite}-debugger.txt")
            try:
                debugger_report.parent.mkdir(parents=True, exist_ok=True)
                debugger_report.write_text(debugger_output, encoding="utf-8")
            except (OSError, ValueError) as error:
                debugger_output += f"Could not save isolated debugger report: {error}\n"
            output += f"\nCrash diagnostics for {suite}:\n" + debugger_output
        output = (f"\n--- {label} ---\n" + output +
                  f"\n{label} exited with status {status} (0x{status & 0xffffffff:08X}).\n"
                  "The original all-workflows failure remains authoritative.\n")
        report = output_path.with_name(output_path.stem + f"-{suite}.txt")
        try:
            report.parent.mkdir(parents=True, exist_ok=True)
            report.write_text(output, encoding="utf-8")
        except (OSError, ValueError) as error:
            output += f"Could not save isolated diagnostic report: {error}\n"
            status = 1
        results.append(f"{suite}={'PASS' if status == 0 else 'FAIL'} (status {status})")
        reports.append(output)
        print(f"{label}: {'PASS' if status == 0 else 'FAIL'} (status {status}).", flush=True)
        if not state["safe_to_release"]:
            if runtime_state is not None:
                runtime_state["safe_to_release"] = False
            reports.append("\nRemaining diagnostics were not launched because process-tree exit was not confirmed.\n")
            break
    # Keep every result in the original report too, so existing artifact and
    # failure-report collection retains diagnostics without workflow changes.
    return ("\nIndependent diagnostics; original all-workflows result unchanged:\n" +
            "\n".join(results) + "\n" + "".join(reports))


def capture_debugger(command, executable, source, mesa_runtime=None, native_suite=None,
                     runtime_state=None, cleanup_checkpoint=None, output_limit_bytes=None):
    state = runtime_state if runtime_state is not None else {"safe_to_release": True}
    output = b""
    notes = ""
    try:
        with isolated_fixture(source, executable, mesa_runtime, runtime_state=state) as (directory, environment):
            select_diagnostic_suite(environment, native_suite)
            try:
                result = run_captured_process(command, cwd=directory, environment=environment,
                                              timeout=120, runtime_state=state)
                output = result.stdout
                notes += f"\nDiagnostic debugger exited with status {result.returncode}.\n"
            except subprocess.TimeoutExpired as error:
                output = error.output or b""
                notes += "\nNative crash debugger timed out after 120 seconds.\n"
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
                output = output or state.get("output", b"")
                notes += f"\nDiagnostic debugger launch or wait failed: {type(error).__name__}: {error}\n"
            finally:
                if cleanup_checkpoint is not None:
                    observed = decode_output(output or state.get("output", b""))
                    if cleanup_checkpoint not in observed.splitlines():
                        # A debugger can own an inferior in another process
                        # group. Group cleanup alone cannot prove its exit.
                        state["safe_to_release"] = False
                        notes += "\nDebugger target exit was not confirmed; disposable resources retained.\n"
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        output = output or state.get("output", b"")
        notes += f"\nDiagnostic launch or cleanup failed: {type(error).__name__}: {error}\n"
    if output_limit_bytes is not None:
        raw = output if isinstance(output, bytes) else output.encode("utf-8")
        if len(raw) > output_limit_bytes:
            half = output_limit_bytes // 2
            output = (raw[:half] + b"\nDebugger output truncated; beginning and ending retained.\n" + raw[-half:])
    return decode_output(output) + notes + "The original native test failure remains authoritative.\n"


def windows_crash_diagnostics(executable, source, native_status, symbol_directories,
                              mesa_runtime=None, native_suite=None, runtime_state=None):
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
    return capture_debugger(command, executable, source, mesa_runtime, native_suite,
                            runtime_state=runtime_state)


def linux_crash_diagnostics(executable, source, native_suite=None, runtime_state=None):
    debugger = shutil.which("gdb")
    if not debugger:
        return "Linux debugger is unavailable; use the flushed native checkpoints.\n"
    command = [debugger, "--batch", "-ex", "set pagination off", "-ex", "run",
               "-ex", "thread apply all bt 20", "--args", str(executable), "--test-native-ui"]
    return capture_debugger(command, executable, source, native_suite=native_suite,
                            runtime_state=runtime_state)


def macos_crash_diagnostics(executable, source, native_suite=None, runtime_state=None):
    debugger = shutil.which("lldb")
    if not debugger:
        return ("macOS LLDB is unavailable; no debugger process was launched.\n"
                "The original native test failure remains authoritative.\n")
    cleanup_checkpoint = "native-lldb-target-cleanup: CONFIRMED"
    # LLDB's crash command list is separate from its ordinary -o commands.
    # Confirm inferior termination on both paths; do not infer it merely from
    # the debugger exiting. Match the emitted line, never the echoed command.
    cleanup = (
        "script p=lldb.debugger.GetSelectedTarget().GetProcess(); "
        "e=p.Kill() if p.IsValid() and p.GetState()!=lldb.eStateExited else None; "
        "print('" + cleanup_checkpoint + "' if not p.IsValid() or "
        "p.GetState()==lldb.eStateExited else 'native-lldb-target-cleanup: UNCONFIRMED')")
    command = [debugger, "--batch", "--no-lldbinit",
               "-o", "settings set auto-confirm true",
               "-o", "run",
               "-o", cleanup,
               "--one-line-on-crash", "thread backtrace all -c 40",
               "--one-line-on-crash", cleanup,
               "--", str(executable), "--test-native-ui"]
    return capture_debugger(command, executable, source, native_suite=native_suite,
                            runtime_state=runtime_state, cleanup_checkpoint=cleanup_checkpoint,
                            output_limit_bytes=256 * 1024)


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("stuff", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--artist-benchmark", action="store_true",
                        help="Measure real drawing, native rendering and resource-save workloads")
    parser.add_argument("--isolated-timing-lifecycle", action="store_true",
                        help="Require the isolated combined-timing fixture and clean process exit; does not replace the full suite")
    parser.add_argument("--require-function-workflow", action="store_true",
                        help="Require the real application Function Editor fixture checkpoint")
    parser.add_argument("--windows-crash-debugger", action="store_true",
                        help="Capture a separate CDB stack after a Windows native exception")
    parser.add_argument("--windows-original-dump-directory", type=Path,
                        help="Capture and decode a bounded WER dump of the original Windows process; no replay")
    parser.add_argument("--linux-crash-debugger", action="store_true",
                        help="Capture a separate GDB stack after a Linux native signal")
    parser.add_argument("--linux-original-core-directory", type=Path,
                        help="Collect a bounded postmortem of the original Linux process; no replay")
    parser.add_argument("--macos-crash-debugger", action="store_true",
                        help="Capture a separate LLDB stack after a macOS native signal")
    parser.add_argument("--debugger-symbols", type=Path, action="append", default=[],
                        help="Local PDB directory for optional Windows crash diagnostics")
    parser.add_argument("--windows-mesa", type=Path,
                        help="Verified Mesa x64 runtime for temporary Windows desktop GL")
    parser.add_argument("--require-offline-gl-workflow", action="store_true",
                        help="Require native context and actual converter acceptance checkpoints")
    parser.add_argument("--require-vector-workflows", action="store_true",
                        help="Require vector/native production renderer and CLI acceptance checkpoints")
    parser.add_argument("--require-combined-workflows", action="store_true",
                        help="Require cross-feature timing, vector metadata and export acceptance")
    args = parser.parse_args()
    if args.linux_original_core_directory and (not sys.platform.startswith("linux") or args.artist_benchmark):
        parser.error("--linux-original-core-directory requires the Linux native qualification run")
    if args.windows_original_dump_directory and (os.name != "nt" or args.artist_benchmark):
        parser.error("--windows-original-dump-directory requires the Windows native qualification run")
    if args.isolated_timing_lifecycle and any((args.artist_benchmark,
            args.require_function_workflow, args.require_offline_gl_workflow,
            args.require_vector_workflows, args.require_combined_workflows)):
        parser.error("--isolated-timing-lifecycle must run separately from the full suite and benchmark")
    selected_suite = "combined-timing" if args.isolated_timing_lifecycle else None
    timeout_seconds = (120 if selected_suite else 600 if args.require_vector_workflows
                       else 240 if args.require_offline_gl_workflow else 180)
    output, status = "", 1
    native_status = None
    executable, source = None, None
    original_windows = None
    original_macos_identity = None
    original_macos_text = ""
    original_macos_matched = False
    runtime_state = {"safe_to_release": True}
    try:
        executable = args.executable.resolve(strict=True)
        source = args.stuff.resolve(strict=True)
        with isolated_fixture(source, executable, args.windows_mesa,
                              runtime_state=runtime_state) as (directory, environment):
            select_diagnostic_suite(environment, selected_suite)
            if args.artist_benchmark:
                environment["GROUNDZERO_ARTIST_BENCHMARK"] = "1"
            try:
                core_limit = nullcontext()
                dump_capture = nullcontext(None)
                if args.linux_original_core_directory:
                    from native_ui_core import original_core_limit
                    core_limit = original_core_limit(args.linux_original_core_directory)
                if args.windows_original_dump_directory:
                    from native_ui_windows_dump import OriginalWindowsDump
                    dump_capture = OriginalWindowsDump(executable, args.windows_original_dump_directory,
                                                       runtime_state=runtime_state)
                with core_limit, dump_capture as original_windows:
                    result = run_captured_process([str(executable), "--test-native-ui"],
                                                  cwd=directory, environment=environment,
                                                  timeout=timeout_seconds, runtime_state=runtime_state)
                    output, status = decode_output(result.stdout), result.returncode
                    native_status = status
                    if status:
                        output += (f"\nNative application exited with status {status} "
                                   f"(0x{status & 0xffffffff:08X}).\n")
                    if original_windows is not None and (status & 0xc0000000) == 0xc0000000:
                        if runtime_state["safe_to_release"]:
                            dump_output = original_windows.collect(runtime_state.get("pid"),
                                runtime_state.get("started_at"), runtime_state.get("finished_at"),
                                args.debugger_symbols, run_captured_process)
                        else:
                            original_windows.safe_to_release = False
                            dump_output = ("Original Windows dump analysis was not launched: "
                                "original process-tree exit is uncertain. Raw dump resources retained "
                                "outside report artifacts; original status remains authoritative.\n")
                        output += "\n--- Original Windows process dump (no replay) ---\n" + dump_output
                        if not original_windows.safe_to_release:
                            runtime_state["safe_to_release"] = False
                        report = args.output.with_name(args.output.stem + "-original-windows-dump.txt")
                        report.parent.mkdir(parents=True, exist_ok=True)
                        report.write_text(dump_output, encoding="utf-8")
                if original_windows is not None:
                    if original_windows.restore_failed and status == 0:
                        status = 1
                if args.linux_original_core_directory and native_status < 0:
                    from native_ui_core import collect_original_core
                    core_output = collect_original_core(executable,
                        args.linux_original_core_directory, runtime_state.get("pid"))
                    output += "\n--- Original native process core (no replay) ---\n" + core_output
                    try:
                        report = args.output.with_name(args.output.stem + "-original-core.txt")
                        report.parent.mkdir(parents=True, exist_ok=True)
                        report.write_text(core_output, encoding="utf-8")
                    except (OSError, ValueError) as error:
                        output += f"Could not save original core diagnostic report: {error}\n"
                if sys.platform == "darwin" and native_status < 0 and runtime_state["safe_to_release"]:
                    from native_macos_crash import collect_original_macos_crash
                    # Later clear/debugger helpers may overwrite runtime_state.
                    # Keep the original process identity independent of them.
                    original_macos_identity = (executable, runtime_state.get("pid"),
                        runtime_state.get("started_at"), runtime_state.get("finished_at"))
                    crash_output = collect_original_macos_crash(*original_macos_identity)
                    original_macos_matched = crash_output.startswith(
                        f"original-macos-crash: MATCHED original PID {original_macos_identity[1]}; no replay.\n")
                    original_macos_text = original_macos_identity_witness(original_macos_identity, native_status) + crash_output
                    output += "\n--- Original macOS process crash report (no replay) ---\n" + original_macos_text
                    report = args.output.with_name(args.output.stem + "-original-macos-crash.txt")
                    output += write_macos_crash_text(report, original_macos_text)
                if status == 0 and selected_suite:
                    lines = output.splitlines()
                    timing_pass = "function-timing-native: PASS"
                    required = {
                        timing_pass: any(line == timing_pass or line.startswith(timing_pass + " (")
                                         for line in lines),
                        "native-ui-diagnostic: PASS combined-timing":
                            "native-ui-diagnostic: PASS combined-timing" in lines,
                        "native-ui-integration: checkpoint: native application cleanup completed":
                            "native-ui-integration: checkpoint: native application cleanup completed" in lines,
                    }
                    missing = ([marker for marker, observed in required.items() if not observed] +
                               missing_icon_lifecycle_checkpoints(output))
                    if missing:
                        output += "\nMissing required isolated timing lifecycle checkpoints: " + ", ".join(missing) + "\n"
                        status = 1
                if status == 0 and not selected_suite and not args.artist_benchmark:
                    missing = missing_icon_lifecycle_checkpoints(output)
                    if missing:
                        output += "\nMissing native icon lifecycle checkpoints: " + ", ".join(missing) + "\n"
                        status = 1
                if status == 0 and args.require_vector_workflows and not args.artist_benchmark:
                    missing = [marker for marker in ("native-ui-integration: PASS", "vector-workflows: PASS", "offline-gl-workflow: PASS")
                               if marker not in output]
                    if missing:
                        output += "\nMissing required native acceptance checkpoints: " + ", ".join(missing) + "\n"
                        status = 1
                if status and native_status == 0:
                    output += (f"\nNative application exited with status {status} "
                               f"(0x{status & 0xffffffff:08X}).\n")
            except subprocess.TimeoutExpired as error:
                output = decode_output(error.stdout)
                output += f"\nNative application integration test timed out after {timeout_seconds} seconds.\n"
                status = 1
    except (OSError, RuntimeError, ValueError) as error:
        output = output or decode_output(runtime_state.get("output", b""))
        output += ("\nNative application integration setup, launch or cleanup failed: "
                   f"{type(error).__name__}: {error}\n")
        status = 1
    if original_windows is not None:
        output += "\n".join(original_windows.notes) + "\n"
    if selected_suite and status == 0:
        output += "\nRequired isolated combined-timing lifecycle: PASS (process exited with status 0).\n"
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
    if args.require_offline_gl_workflow and not args.artist_benchmark and status == 0 and "offline-gl-workflow: PASS" not in output:
        output += "\nThe required native offscreen context/converter fixture did not run.\n"
        status = 1
    if args.require_combined_workflows and not args.artist_benchmark and status == 0:
        missing = [marker for marker in ("function-workflow-native: PASS",
                                         "cast-native-workflow: PASS",
                                         "function-timing-native: PASS",
                                         "cast-vector-native: PASS",
                                         "combined-fidelity-native: PASS")
                   if marker not in output]
        if missing:
            output += "\nMissing required combined acceptance checkpoints: " + ", ".join(missing) + "\n"
            status = 1
    # Complete every original-process gate before adding a separate process's
    # markers. Both terminal cases are mandatory; diagnostics remain separate.
    original_status = status
    if not args.artist_benchmark:
        if executable is not None and source is not None:
            clear_output, clear_status = required_icon_clear(
                executable, source, args.output, args.windows_mesa, runtime_state)
            output += clear_output
            if clear_status:
                status = 1
        else:
            output += "\nRequired reviewed-clear lifecycle unavailable: executable or fixture source could not be resolved.\n"
            status = 1
    if (runtime_state["safe_to_release"] and original_status != 0 and
            args.require_combined_workflows and not args.artist_benchmark):
        # Validation above uses only the original process output. Separate
        # diagnostic markers can never satisfy its qualification requirements,
        # and neither their success nor failure changes its nonzero status.
        if executable is not None and source is not None:
            output += combined_failure_diagnostics(executable, source, args.output,
                                                  args.windows_mesa,
                                                  args.linux_crash_debugger,
                                                  args.windows_crash_debugger,
                                                  args.debugger_symbols,
                                                  runtime_state=runtime_state,
                                                  macos_debugger=args.macos_crash_debugger)
        else:
            output += "\nIndependent diagnostics unavailable: executable or fixture source could not be resolved.\n"
    if not runtime_state["safe_to_release"]:
        output += "\nFurther diagnostics were not launched because process-tree exit was not confirmed.\n"
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="utf-8")
    except (OSError, ValueError) as error:
        message = f"Could not write native UI integration report: {error}"
        message = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        print(f"::error title=Native UI report::{message}")
        status = 1
    print(output, end="", flush=True)
    if (runtime_state["safe_to_release"] and args.linux_crash_debugger and not args.linux_original_core_directory
            and not args.artist_benchmark and sys.platform.startswith("linux")
            and native_status is not None and native_status < 0):
        try:
            diagnostics = linux_crash_diagnostics(executable, source,
                **({"native_suite": selected_suite} if selected_suite else {}))
        except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
            diagnostics = f"Native Linux crash diagnostics failed: {type(error).__name__}: {error}\n"
        report = args.output.with_name(args.output.stem + "-debugger.txt")
        try:
            report.write_text(diagnostics, encoding="utf-8")
        except (OSError, ValueError) as error:
            diagnostics += f"Could not save native debugger report: {error}\n"
        print(diagnostics, end="")
    if (runtime_state["safe_to_release"] and args.macos_crash_debugger and not args.artist_benchmark
            and sys.platform == "darwin" and native_status is not None and native_status < 0):
        try:
            diagnostics = macos_crash_diagnostics(executable, source, runtime_state=runtime_state,
                **({"native_suite": selected_suite} if selected_suite else {}))
        except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
            diagnostics = f"Native macOS crash diagnostics failed: {type(error).__name__}: {error}\n"
        report = args.output.with_name(args.output.stem + "-debugger.txt")
        try:
            report.write_text(diagnostics, encoding="utf-8")
        except (OSError, ValueError) as error:
            diagnostics += f"Could not save native debugger report: {error}\n"
        print(diagnostics, end="")
    if (runtime_state["safe_to_release"] and args.windows_crash_debugger and not args.windows_original_dump_directory
            and not args.artist_benchmark and os.name == "nt" and native_status is not None
            and (native_status & 0xc0000000) == 0xc0000000):
        try:
            diagnostics = windows_crash_diagnostics(executable, source, native_status,
                                                    args.debugger_symbols, args.windows_mesa,
                                                    **({"native_suite": selected_suite} if selected_suite else {}))
        except (OSError, RuntimeError, ValueError) as error:
            diagnostics = ("Native crash diagnostics could not complete: "
                           f"{type(error).__name__}: {error}\n")
        report = args.output.with_name(args.output.stem + "-debugger.txt")
        try:
            report.write_text(diagnostics, encoding="utf-8")
        except (OSError, ValueError) as error:
            diagnostics += f"Could not save native debugger report: {error}\n"
        print(diagnostics, end="")
    if original_macos_identity is not None and not original_macos_matched:
        # The OS may finish its report during existing diagnostic work. This
        # is one zero-wait read for the SAME retired original process, even if
        # a diagnostic changed runtime_state; no later PID/status can qualify it.
        from native_macos_crash import collect_original_macos_crash
        try:
            late_report = collect_original_macos_crash(*original_macos_identity, timeout=0)
        except (OSError, RuntimeError, TypeError, ValueError) as error:
            late_report = ("original-macos-crash: UNAVAILABLE (final original-process "
                           f"scan failed: {type(error).__name__}).\n")
        late_output = "\n--- Final zero-wait scan for the same original macOS process ---\n" + late_report
        original_macos_text += late_output
        report = args.output.with_name(args.output.stem + "-original-macos-crash.txt")
        late_output += write_macos_crash_text(report, original_macos_text)
        output += late_output
        # The initial report was already printed/saved before debugger work.
        # Preserve it verbatim and retain the late evidence in both destinations.
        late_output += write_macos_crash_text(args.output, output)
        print(late_output, end="", flush=True)
    return 0 if status == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
