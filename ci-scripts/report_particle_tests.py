"""Expose particle integration failures through the GitHub checks API."""

import pathlib
import sys


def escape(text):
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


report = pathlib.Path(sys.argv[1])
if report.is_file():
    output = report.read_text(encoding="utf-8", errors="replace")
    print("::error title=Particle cache regression::" + escape(
        output[-16000:] or "CTest produced an empty report"))
else:
    print("::error title=Particle cache regression::CTest report is missing")
