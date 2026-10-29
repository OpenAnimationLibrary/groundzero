"""Expose particle integration failures through the GitHub checks API."""

import pathlib
import sys


def escape(text):
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


report = pathlib.Path(sys.argv[1])
if report.is_file():
    output = report.read_text(encoding="utf-8", errors="replace")
    # GitHub truncates an individual annotation at roughly 4 KiB. Split the
    # bounded report so API-only review retains every diagnostic and test name.
    output = output[-64000:] or "CTest produced an empty report"
    chunks = []
    while output:
        end = min(len(output), 3000)
        if end < len(output):
            line_end = output.rfind("\n", 0, end)
            if line_end > 0:
                end = line_end + 1
        chunks.append(output[:end])
        output = output[end:]
    for index, chunk in enumerate(chunks, 1):
        title = "Particle cache regression"
        if len(chunks) > 1:
            title += f" ({index}/{len(chunks)})"
        print(f"::error title={title}::" + escape(chunk))
else:
    print("::error title=Particle cache regression::CTest report is missing")
