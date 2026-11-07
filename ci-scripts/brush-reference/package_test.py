"""Run the brush workload from the actual release package on a fresh runner."""
import argparse
import os
from pathlib import Path
import sys
import subprocess
import tempfile

import application_test

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "seexpr"))
from package_smoke import (linux_package, macos_package, windows_package,
                           packaged_environment, verify_test_artifacts_absent)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("linux", "macos", "windows"), required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        actual = "windows" if os.name == "nt" else "macos" if sys.platform == "darwin" else "linux"
        if args.platform != actual:
            raise RuntimeError("Package and fresh runner platform differ")
        artifact = args.artifact.resolve(strict=True)
        if actual == "windows" and not os.environ.get("SEEXPR_TEST_MESA"):
            raise RuntimeError("Packaged brush test requires the verified desktop renderer")
        with tempfile.TemporaryDirectory(prefix="groundzero-brush-installed-") as temporary:
            installed = Path(temporary)
            if actual == "linux":
                executable, stuff = linux_package(artifact, installed)
            elif actual == "macos":
                executable, stuff = macos_package(artifact, installed)
            else:
                executable, stuff = windows_package(artifact)
            package = executable.parent / "usr" if actual == "linux" else executable.parent
            verify_test_artifacts_absent(package)
            arguments = sys.argv
            try:
                with packaged_environment(actual, executable):
                    os.environ["QT_QPA_PLATFORM"] = {"linux": "xcb", "macos": "cocoa", "windows": "windows"}[actual]
                    sys.argv = [str(Path(application_test.__file__)), str(executable), str(stuff),
                                "--output", str(output)]
                    status = application_test.main()
            finally:
                sys.argv = arguments
                verify_test_artifacts_absent(package)
            if status or not output.is_file() or "raster-brush-integration: PASS" not in output.read_text(
                    encoding="utf-8", errors="replace").splitlines():
                raise RuntimeError("Packaged brush workload did not complete")
        print("raster-brush-installed-package: PASS", flush=True)
        return 0
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        message = "Packaged raster brush test failed: " + str(error)
        with output.open("a", encoding="utf-8") as log:
            log.write("\n" + message + "\n")
        print("::error::" + message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A"), flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
