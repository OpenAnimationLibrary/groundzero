"""Publish failed CTest output as annotations accessible with the checks API."""
import pathlib
import sys


def escape(value):
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


report = pathlib.Path(sys.argv[1])
try:
    output = report.read_text(encoding="utf-8", errors="replace")
    print("::error title=Native UI regression::"
          + escape(output[-12000:] or "Native UI tests produced an empty output report"))
except OSError as error:
    print("::error title=Native UI regression::"
          + escape(f"Native UI test output report is unavailable: {error}"))
