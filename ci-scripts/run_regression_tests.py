#!/usr/bin/env python3
"""Run a required CTest subset and annotate failures in CI.

Unlike a regex alone, the named manifest fails if even one expected test is
missing or skipped. Run from the CMake build directory.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET


def error(message):
    escaped = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    print("::error::" + escaped, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config")
    parser.add_argument("--report", default="reliability-tests.xml")
    parser.add_argument("--include-prefix", action="append", default=[],
                        help="Also retain every registered test with this prefix")
    parser.add_argument("tests", nargs="+")
    args = parser.parse_args()
    command = ["ctest"] + (["-C", args.config] if args.config else [])
    listed = subprocess.run(command + ["--show-only=json-v1"],
                            text=True, capture_output=True, check=True)
    available = {test["name"] for test in json.loads(listed.stdout)["tests"]}
    expected = set(args.tests)
    missing = expected - available
    if missing:
        error("Required tests were not registered: " + ", ".join(sorted(missing)))
        return 1

    expected.update(name for name in available
                    if any(name.startswith(prefix) for prefix in args.include_prefix))
    report = Path(args.report)
    report.unlink(missing_ok=True)
    pattern = "^(" + "|".join(re.escape(name) for name in sorted(expected)) + ")$"
    result = subprocess.run(command + ["--output-on-failure", "--no-tests=error",
                                      "--output-junit", str(report), "-R", pattern])
    if not report.exists():
        error("CTest did not produce its required regression report")
        return 1

    cases = list(ET.parse(report).iter("testcase"))
    observed = {case.attrib.get("name") for case in cases}
    bad = bool(result.returncode)
    if expected - observed:
        error("Required tests were not run: " + ", ".join(sorted(expected - observed)))
        bad = True
    for case in cases:
        if any(case.find(tag) is not None for tag in ("failure", "error", "skipped")):
            output = case.findtext("system-out", "")
            error(case.attrib.get("name", "unknown test") + " failed or skipped\n" + output[-5000:])
            bad = True
    return int(bad)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, ET.ParseError, subprocess.SubprocessError) as exc:
        error("Regression runner failed: " + str(exc))
        sys.exit(1)
