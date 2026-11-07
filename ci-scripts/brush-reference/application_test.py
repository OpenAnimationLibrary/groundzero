"""Exercise the real brush tool in a disposable native application profile."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_native_ui_test import isolated_fixture, decode_output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("stuff", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output, status = "", 1
    try:
        executable = args.executable.resolve(strict=True)
        source = args.stuff.resolve(strict=True)
        mesa = os.environ.get("NATIVE_UI_MESA_RUNTIME") or os.environ.get("SEEXPR_TEST_MESA")
        # The expanded Cocoa UI suite includes collection, conversion and editor
        # dialogs as well as painting. A complete macOS run already took 162s;
        # the original 180s watchdog cut off a later run still making progress.
        # This is an aggregate liveness limit, not an engine performance budget.
        timeout_seconds = 300 if sys.platform == "darwin" else 180
        with isolated_fixture(source, executable, Path(mesa) if mesa and os.name == "nt" else None) as (directory, environment):
            result = subprocess.run([str(executable), "--test-raster-brush"], cwd=directory,
                                    env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=timeout_seconds, encoding="utf-8", errors="replace")
            output, status = result.stdout, result.returncode
            if status == 0 and "raster-brush-integration: PASS" not in output:
                output += "\nApplication exited without completing the brush workload.\n"
                status = 1
    except subprocess.TimeoutExpired as error:
        output = decode_output(error.stdout) + "\nRaster brush application test timed out.\n"
    except (OSError, RuntimeError, ValueError) as error:
        output += "\nBrush application fixture failed: " + str(error) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output, encoding="utf-8")
    print(output, end="", flush=True)
    if status:
        excerpt = output[-6000:].replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        print("::error title=Raster brush application test::" + excerpt)
    return 0 if status == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
