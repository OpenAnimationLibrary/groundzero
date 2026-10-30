"""Publish failed CTest output as annotations accessible with the checks API."""
import pathlib
import sys


def escape(value):
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


# CI redirects stdout on Windows, where its default encoding can be cp1252.
# Preserve Unicode diagnostics in the UTF-8 annotation consumed by GitHub.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")

report = pathlib.Path(sys.argv[1])
title = sys.argv[2] if len(sys.argv) > 2 else "Native UI regression"
annotation = "::error title=" + escape(title).replace(",", "%2C") + "::"
try:
    output = report.read_text(encoding="utf-8", errors="replace")
    print(annotation
          + escape(output[-12000:] or "Tests produced an empty output report"))
except OSError as error:
    print(annotation + escape(f"Test output report is unavailable: {error}"))
