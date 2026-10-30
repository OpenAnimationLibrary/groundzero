"""Publish failed CTest output as annotations accessible with the checks API."""
import argparse
import pathlib
import sys


def escape(value):
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("report", type=pathlib.Path)
parser.add_argument("--outcome", required=True,
                    choices=("success", "failure", "cancelled", "skipped"))
args = parser.parse_args()

# A later native failure does not turn a successful Cel run into a regression.
# Conversely, a failed Cel command can have a successful CTest summary followed
# by a launch/runner error: its recorded step outcome is authoritative.
if args.outcome == "failure":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    try:
        output = args.report.read_text(encoding="utf-8", errors="replace")
        detail = output or "CTest produced an empty output report"
    except OSError as error:
        detail = f"CTest output report is unavailable: {error}"
    detail = detail.encode("utf-8", errors="backslashreplace")[-12000:].decode(
        "utf-8", errors="ignore")
    print("::error title=Cel effect regression::"
          + escape("Cel test command failed.\n" + detail))
