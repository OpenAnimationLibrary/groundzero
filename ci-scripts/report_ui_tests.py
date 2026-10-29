"""Publish failed CTest output as annotations accessible with the checks API."""
import pathlib
import sys


def escape(value):
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


report = pathlib.Path(sys.argv[1])
if report.exists():
    output = report.read_text(encoding="utf-8", errors="replace")
    print("::error title=Native UI regression::"
          + escape(output[-12000:] or "CTest produced an empty output report"))
else:
    print("::notice::No native UI test output report was produced")
