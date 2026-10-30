"""Run a bounded Windows test command with the same desktop GL as native UI CI.

The verified Mesa files are removed after the command, or retained with an error
when descendants may still be using them. They are never shipped. The rendering
executable can be a CTest child instead of the command itself.
"""
import argparse
import os
from pathlib import Path
import subprocess

from run_native_ui_test import windows_mesa_renderer


def run_command(command, environment, timeout, runtime_state):
    process = subprocess.Popen(command, env=environment)
    runtime_state["safe_to_release"] = False
    try:
        result = process.wait(timeout=timeout)
        runtime_state["safe_to_release"] = True
        return result
    except subprocess.TimeoutExpired:
        # CTest and painting tests can own renderer/model worker processes.
        # Terminate the entire tree before releasing DLLs used by descendants.
        tree_stopped = False
        try:
            cleanup = subprocess.run(
                ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                check=False, timeout=20)
            if cleanup.returncode:
                print(f"Windows renderer process-tree cleanup exited with {cleanup.returncode}",
                      flush=True)
            else:
                tree_stopped = True
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f"Windows renderer process-tree cleanup failed: {type(error).__name__}",
                  flush=True)
        if process.poll() is None:
            process.kill()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            raise RuntimeError("Timed-out Windows OpenGL process did not terminate")
        runtime_state["safe_to_release"] = tree_stopped
        raise RuntimeError("Windows OpenGL test command exceeded its deadline")


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
    with windows_mesa_renderer(
            executable, args.runtime,
            cleanup_allowed=lambda: runtime_state["safe_to_release"]) as renderer:
        environment = os.environ.copy()
        environment.update(renderer)
        environment["GROUNDZERO_REQUIRE_VECTOR_GL"] = "1"
        environment["GROUNDZERO_OFFSCREEN_GL_DIAGNOSTICS"] = "1"
        return run_command(command, environment, args.timeout, runtime_state)


if __name__ == "__main__":
    raise SystemExit(main())
