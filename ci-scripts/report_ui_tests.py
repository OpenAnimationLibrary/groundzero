"""Publish failed CTest output as annotations accessible with the checks API."""
import pathlib
import re
import sys


def escape(value):
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def failure_reports(output):
    """Separate the mandatory process from the runner's isolated diagnostics.

    The summary is written by run_native_ui_test.py after all diagnostic
    launches. Use those final statuses, not incidental FAIL text in a passing
    diagnostic or its debugger output, to select secondary annotations.
    """
    primary, boundary, diagnostics = output.partition(
        "\nIndependent diagnostics; original all-workflows result unchanged:\n")
    yield "", primary
    if not boundary:
        return
    sections = re.split(
        r"(?m)^--- Independent native diagnostic: ([a-z0-9-]+) ---\r?$",
        diagnostics)
    details = dict(zip(sections[1::2], sections[2::2]))
    for result in re.finditer(
            r"(?m)^([a-z0-9-]+)=(PASS|FAIL) \(status (-?\d+)\)\r?$", sections[0]):
        suite, outcome, status = result.groups()
        if outcome == "FAIL" or int(status) != 0:
            yield " - secondary diagnostic: " + suite, details.get(suite, result.group())


def failure_excerpt(output):
    """Keep the cause before GitHub's annotation truncation boundary.

    The complete output is retained as a workflow artifact. A long tail alone
    can start with routine Qt messages and lose the actual final assertion.
    """
    lines = output.splitlines()
    failure = re.compile(
        r"\bFAIL(?:ED)?\b|Traceback|Assertion.*failed|assertion.*failed|"
        r"\*\*\*Exception|received signal|SIGSEGV|SIGABRT|\b\w*Error:", re.I)
    candidates = [i for i, line in enumerate(lines) if failure.search(line)]
    native = [i for i in candidates if "native-ui-integration: FAIL:" in lines[i]]
    timeouts = [i for i, line in enumerate(lines) if re.fullmatch(
        r"(?:Native application integration test|Isolated diagnostic) "
        r"timed out after \d+(?:\.\d+)? seconds\.", line)]
    index = (timeouts or native or candidates or [max(0, len(lines) - 1)])[-1]
    phases = [line for line in lines[:index]
              if "checkpoint:" in line or "workflow:" in line or "-native:" in line]
    sections = []
    if lines:
        sections.append("Failure: " + lines[index])
        if phases:
            sections.append("Recent phases:\n" + "\n".join(phases[-6:]))
        sections.append("Failure context:\n" + "\n".join(lines[max(0, index - 3):index + 10]))
        sections.append("Output tail:\n" + output[-1800:])
    else:
        sections.append("Tests produced an empty output report")
    # Bound bytes as well as characters; non-ASCII captions must not push the
    # failure past the checks API's annotation limit.
    return "\n\n".join(sections).encode("utf-8")[:3500].decode("utf-8", errors="ignore")

# CI redirects stdout on Windows, where its default encoding can be cp1252.
# Preserve Unicode diagnostics in the UTF-8 annotation consumed by GitHub.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")

report = pathlib.Path(sys.argv[1])
title = sys.argv[2] if len(sys.argv) > 2 else "Native UI regression"
annotation = "::error title=" + escape(title).replace(",", "%2C") + "::"
try:
    output = report.read_text(encoding="utf-8", errors="replace")
    for suffix, section in failure_reports(output):
        section_annotation = "::error title=" + escape(title + suffix).replace(",", "%2C") + "::"
        print(section_annotation + escape(failure_excerpt(section)))
except OSError as error:
    print(annotation + escape(f"Test output report is unavailable: {error}"))
