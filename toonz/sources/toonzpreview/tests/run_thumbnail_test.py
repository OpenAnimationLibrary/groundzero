"""Run the actual Windows Shell provider and renderer in disposable settings."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "ci-scripts"))
from run_native_ui_test import isolated_fixture

parser = argparse.ArgumentParser()
parser.add_argument("fixture", type=Path)
parser.add_argument("converter", type=Path)
parser.add_argument("provider", type=Path)
parser.add_argument("stuff", type=Path)
args = parser.parse_args()
mesa = os.environ.get("NATIVE_UI_MESA_RUNTIME")
with isolated_fixture(args.stuff.resolve(), args.fixture.resolve(),
                      Path(mesa) if mesa else None) as (directory, environment):
    process = subprocess.Popen([str(args.fixture.resolve()), str(args.converter.resolve()),
                                str(args.provider.resolve())], cwd=directory, env=environment,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        output, _ = process.communicate(timeout=180)
    except subprocess.TimeoutExpired:
        process.kill()
        output, _ = process.communicate(timeout=10)
        print(output.decode("utf-8", errors="replace"), flush=True)
        raise RuntimeError("Shell thumbnail fixture exceeded its deadline")
    print(output.decode("utf-8", errors="replace"), flush=True)
    if process.returncode:
        raise SystemExit(process.returncode)
