"""Run the actual Windows Shell provider and renderer in disposable settings."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "ci-scripts"))
from run_native_ui_test import isolated_fixture, run_captured_process


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
runtime_state = {"safe_to_release": True}
with isolated_fixture(args.stuff.resolve(), args.fixture.resolve(),
                      Path(mesa) if mesa else None,
                      runtime_state=runtime_state) as (directory, environment):
    environment["GROUNDZERO_OFFSCREEN_GL_DIAGNOSTICS"] = "1"
    try:
        result = run_captured_process(
            [str(args.fixture.resolve()), str(args.converter.resolve()), str(args.provider.resolve())],
            cwd=directory, environment=environment, timeout=180, runtime_state=runtime_state)
    except subprocess.TimeoutExpired as error:
        emit_output(error.output or b"")
        raise RuntimeError("Shell thumbnail fixture exceeded its deadline")
    except BaseException:
        if runtime_state.get("output"):
            emit_output(runtime_state["output"])
        raise
    emit_output(result.stdout)
    if result.returncode:
        raise SystemExit(result.returncode)
