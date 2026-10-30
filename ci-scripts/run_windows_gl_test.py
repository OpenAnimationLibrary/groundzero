"""Run a bounded Windows test command with the same desktop GL as native UI CI.

The verified Mesa files are removed after the command, or retained with an error
when descendants may still be using them. They are never shipped. The rendering
executable can be a CTest child instead of the command itself.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

from run_native_ui_test import stop_process_tree, windows_mesa_renderer


def emit_output(output):
    """Preserve cleanup bytes even when Windows redirects a cp1252 stream."""
    if not output:
        return
    sys.stdout.flush()
    if hasattr(sys.stdout, "buffer"):
        sys.stdout.buffer.write(output)
        sys.stdout.buffer.flush()
    else:
        sys.stdout.write(output.decode("utf-8", errors="backslashreplace"))
        sys.stdout.flush()


def run_command(command, environment, timeout, runtime_state):
    process = subprocess.Popen(command, env=environment)
    runtime_state["safe_to_release"] = False
    try:
        result = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        safe, output = stop_process_tree(process)
        runtime_state["safe_to_release"] = safe
        emit_output(output)
        raise RuntimeError("Windows OpenGL test command exceeded its deadline")
    except BaseException:
        safe, output = stop_process_tree(process)
        runtime_state["safe_to_release"] = safe
        emit_output(output)
        raise
    runtime_state["returncode"] = result
    if result != 0:
        # A returned failure can leave descendants alive after the parent is
        # reaped. Apply the same bounded confirmation as the captured runner.
        emit_output((f"Windows OpenGL test command exited with status {result} "
                     f"(0x{result & 0xffffffff:08X}).\n").encode("utf-8"))
        safe, output = stop_process_tree(process)
        runtime_state["safe_to_release"] = safe
        emit_output(output)
    else:
        runtime_state["safe_to_release"] = True
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--renderer-executable", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command or args.timeout <= 0:
        parser.error("A command and a positive timeout are required")
    executable = args.renderer_executable.resolve(strict=True)
    if not executable.is_file():
        parser.error("The renderer executable must be an existing file")
    runtime_state = {"safe_to_release": True}
    try:
        with windows_mesa_renderer(
                executable, args.runtime,
                cleanup_allowed=lambda: runtime_state["safe_to_release"]) as renderer:
            environment = os.environ.copy()
            environment.update(renderer)
            environment["GROUNDZERO_REQUIRE_VECTOR_GL"] = "1"
            environment["GROUNDZERO_OFFSCREEN_GL_DIAGNOSTICS"] = "1"
            return run_command(command, environment, args.timeout, runtime_state)
    except RuntimeError as error:
        # Retention/cleanup diagnostics must not replace a real returned native
        # failure with a generic exit code. A successful command with failed
        # cleanup still raises, and therefore can never qualify as a pass.
        if runtime_state.get("returncode", 0) != 0:
            emit_output((f"Windows renderer cleanup: {error}\n").encode("utf-8", errors="backslashreplace"))
            return runtime_state["returncode"]
        raise


if __name__ == "__main__":
    raise SystemExit(main())
