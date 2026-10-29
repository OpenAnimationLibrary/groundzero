"""Launch the native application against a disposable copied data directory."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


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
        with tempfile.TemporaryDirectory(prefix="groundzero-native-ui-") as root:
            directory = Path(root)
            shutil.copytree(source, directory / "portablestuff")
            (directory / ".groundzero-native-ui-test").write_bytes(b"groundzero-native-ui-test\n")
            environment = os.environ.copy()
            for key, leaf in (("XDG_CONFIG_HOME", "config"), ("XDG_CACHE_HOME", "cache"),
                              ("XDG_DATA_HOME", "data"), ("APPDATA", "appdata"),
                              ("LOCALAPPDATA", "localappdata")):
                path = directory / leaf
                path.mkdir()
                environment[key] = str(path)
            environment["LIBGL_ALWAYS_SOFTWARE"] = "1"
            try:
                result = subprocess.run([str(executable), "--test-native-ui"],
                                        cwd=directory, env=environment,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        timeout=180, encoding="utf-8", errors="replace")
                output, status = result.stdout, result.returncode
                if status:
                    output += (f"\nNative application exited with status {status} "
                               f"(0x{status & 0xffffffff:08X}).\n")
            except subprocess.TimeoutExpired as error:
                output = error.stdout or b""
                if isinstance(output, bytes):
                    output = output.decode("utf-8", errors="replace")
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
    print(output, end="")
    return 0 if status == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
