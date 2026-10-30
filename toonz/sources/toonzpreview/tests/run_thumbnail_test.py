"""Run the actual Windows Shell provider and renderer in disposable settings."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "ci-scripts"))
from run_native_ui_test import isolated_fixture


def emit_output(output):
    # CTest captures bytes. Decoding and printing through the Windows console
    # encoding can raise before the child's status is reported (for example,
    # a Unicode fixture path on cp1252). Preserve its complete raw diagnostics.
    sys.stdout.flush()
    sys.stdout.buffer.write(output)
    if not output.endswith(b"\n"):
        sys.stdout.buffer.write(b"\n")
    sys.stdout.buffer.flush()


parser = argparse.ArgumentParser()
parser.add_argument("fixture", type=Path)
parser.add_argument("converter", type=Path)
parser.add_argument("provider", type=Path)
parser.add_argument("stuff", type=Path)
args = parser.parse_args()
mesa = os.environ.get("NATIVE_UI_MESA_RUNTIME")
with isolated_fixture(args.stuff.resolve(), args.fixture.resolve(),
                      Path(mesa) if mesa else None) as (directory, environment):
    environment["GROUNDZERO_OFFSCREEN_GL_DIAGNOSTICS"] = "1"
    process = subprocess.Popen([str(args.fixture.resolve()), str(args.converter.resolve()),
                                str(args.provider.resolve())], cwd=directory, env=environment,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        output, _ = process.communicate(timeout=180)
    except subprocess.TimeoutExpired:
        # The real CLI comparisons also spawn renderers directly, outside the
        # provider's production Job. Stop that complete tree before releasing
        # the temporary Mesa runtime or disposable user settings.
        try:
            cleanup = subprocess.run(
                ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=15, check=False)
            emit_output(cleanup.stdout)
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f"Shell fixture process-tree cleanup failed: {type(error).__name__}", flush=True)
        if process.poll() is None:
            process.kill()
        try:
            output, _ = process.communicate(timeout=10)
        except subprocess.TimeoutExpired as error:
            output = error.output or b""
            process.stdout.close()
        emit_output(output)
        raise RuntimeError("Shell thumbnail fixture exceeded its deadline")
    emit_output(output)
    if process.returncode:
        raise SystemExit(process.returncode)
