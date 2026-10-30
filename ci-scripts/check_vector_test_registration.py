"""Fail CI if a required vector CTest is absent from the configured build."""
import argparse
import json
import subprocess
import sys

REQUIRED = {
    "ui_vector_opacity", "ui_vector_profilemanager", "ui_vector_gradientanimation",
    "ui_vector_svgopacity", "ui_vector_pliprofile", "ui_vector_morphworkflow",
    "ui_vector_contour_outline",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config")
    args = parser.parse_args()
    command = ["ctest", "--show-only=json-v1"]
    if args.config:
        command += ["-C", args.config]
    try:
        result = subprocess.run(command, check=True, capture_output=True, text=True)
        observed = {test["name"] for test in json.loads(result.stdout)["tests"]}
        missing = REQUIRED - observed
        if missing:
            raise ValueError("Required vector tests were not registered: " + ", ".join(sorted(missing)))
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"::error title=Vector test registration::{error}")
        return 1
    print("Required vector tests registered: " + ", ".join(sorted(REQUIRED)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
