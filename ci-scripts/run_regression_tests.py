#!/usr/bin/env python3
"""Run the registered regression tests with CTest and annotate failures in CI.

The tests come from the build: every registered test with a selected CTest
label, plus every registered test whose name starts with a selected prefix.
A test is added by registering it with LABELS regression in CMake; nothing
here or in the workflows names it. The run fails if nothing is selected, if a
selected test's executable is missing, or if CTest does not run, or skips, a
selected test. Run from the CMake build directory.
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


def test_labels(test):
    labels = set()
    for prop in test.get("properties", []):
        if prop.get("name") == "LABELS":
            value = prop.get("value", [])
            labels.update([value] if isinstance(value, str) else value)
    return labels


def select_tests(registered, labels, prefixes):
    """Return the registered tests with one of the labels or name prefixes."""
    return [test for test in registered
            if test_labels(test) & set(labels)
            or any(test["name"].startswith(prefix) for prefix in prefixes)]


def missing_executables(tests):
    """Return the names of tests whose executable CTest could not find.

    CTest leaves "command" out of its listing when it cannot resolve the
    executable, for example for a target that did not build.
    """
    return [test["name"] for test in tests if not test.get("command")]


def check_results(report, expected, child_status):
    """Return True if the JUnit report shows a failure or a missing test."""
    cases = list(ET.parse(report).iter("testcase"))
    observed = {case.attrib.get("name") for case in cases}
    bad = bool(child_status)
    if expected - observed:
        error("Required tests were not run: " + ", ".join(sorted(expected - observed)))
        bad = True
    for case in cases:
        if any(case.find(tag) is not None for tag in ("failure", "error", "skipped")):
            output = case.findtext("system-out", "")
            error(case.attrib.get("name", "unknown test") + " failed or skipped\n" + output[-5000:])
            bad = True
    return bad


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config")
    parser.add_argument("--report", default="reliability-tests.xml")
    parser.add_argument("--label", action="append", default=[],
                        help="Run every registered test with this CTest label")
    parser.add_argument("--include-prefix", action="append", default=[],
                        help="Run every registered test with this name prefix")
    args = parser.parse_args()
    if not args.label and not args.include_prefix:
        parser.error("select tests with --label or --include-prefix")
    command = ["ctest"] + (["-C", args.config] if args.config else [])
    listed = subprocess.run(command + ["--show-only=json-v1"],
                            text=True, capture_output=True, check=True)
    selected = select_tests(json.loads(listed.stdout)["tests"],
                            args.label, args.include_prefix)
    if not selected:
        error("No registered test has label " + repr(args.label) +
              " or name prefix " + repr(args.include_prefix))
        return 1
    missing = missing_executables(selected)
    if missing:
        error("CTest cannot find the executable of: " + ", ".join(sorted(missing)) +
              " (the target did not build or is not in this configuration)")
        return 1

    expected = {test["name"] for test in selected}
    print(f"Running {len(expected)} regression tests: {' '.join(sorted(expected))}", flush=True)
    report = Path(args.report)
    report.unlink(missing_ok=True)
    pattern = "^(" + "|".join(re.escape(name) for name in sorted(expected)) + ")$"
    result = subprocess.run(command + ["--output-on-failure", "--no-tests=error",
                                      "--output-junit", str(report), "-R", pattern])
    if not report.exists():
        error("CTest did not produce its required regression report")
        return 1
    return int(check_results(report, expected, result.returncode))


if __name__ == "__main__":
    # Redirected Windows stdout otherwise uses the ANSI code page. A Unicode
    # test path must not replace the actual failure with an encoding exception.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, ET.ParseError, subprocess.SubprocessError) as exc:
        error("Regression runner failed: " + str(exc))
        sys.exit(1)
