"""Publish failed CTest output as annotations accessible with the checks API."""
import pathlib
import re
import sys


def escape(value):
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


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
    index = (native or candidates or [max(0, len(lines) - 1)])[-1]
    phases = [line for line in lines[:index]
              if "checkpoint:" in line or "workflow:" in line or "workflow-native:" in line]
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
    print(annotation + escape(failure_excerpt(output)))
except OSError as error:
    print(annotation + escape(f"Test output report is unavailable: {error}"))
