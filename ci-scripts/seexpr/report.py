"""Expose bounded SeExpr build and test failures through check annotations."""
import argparse
from pathlib import Path
import re


def escape(value):
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def diagnostic_details(data):
    lines = data.splitlines()
    diagnostic = re.compile(
        r"\b(?:fatal )?error(?:[: ]|$)|CMake Error|undefined reference|"
        r"undefined symbols?|symbol not found|\bld:|\bFAILED:|\bFAIL:|"
        r"\*\*\*Failed\b|\bBuild FAILED\b|\bException(?:\s|:)|"
        r"\bSegFault\b|Traceback \(most recent call last\):|"
        r"AddressSanitizer|LeakSanitizer|runtime error|SUMMARY:|"
        r"(?:Direct|Indirect) leak of", re.IGNORECASE)
    selected = set()
    for index, line in enumerate(lines):
        if diagnostic.search(line):
            # Linker symbol names precede the generic final clang error. Leak
            # allocation stacks follow their heading. Retain both contexts.
            selected.update(range(max(0, index - 4), min(len(lines), index + 25)))
    if not selected:
        return None
    result = []
    previous = -1
    for index in sorted(selected):
        if previous != -1 and index > previous + 1:
            result.append("... omitted non-diagnostic output ...")
        result.append(lines[index])
        previous = index
    result.append("\nEnd of retained log:\n" + data[-3000:])
    return "\n".join(result)


def byte_chunks(data, size=3500):
    raw = data.encode("utf-8", errors="replace")
    while raw:
        end = min(len(raw), size)
        while end < len(raw) and raw[end] & 0xC0 == 0x80:
            end -= 1
        yield raw[:end].decode("utf-8")
        raw = raw[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    args = parser.parse_args()
    reports = []
    ansi = re.compile(r"\x1b\[[0-9;]*m")
    for report in args.logs:
        if not report.is_file():
            continue
        data = ansi.sub("", report.read_text(encoding="utf-8", errors="replace"))
        reports.append((report.name, data, diagnostic_details(data)))
    if not reports:
        print("::error title=SeExpr qualification::The failed step produced no readable build or test report.")
        return
    failures = [(name, details) for name, data, details in reports if details]
    if not failures:
        name, data, _ = reports[-1]
        failures = [(name, data[-10000:])]
    # GitHub truncates a single annotation and limits error annotations per
    # step. Split useful contexts into bounded UTF-8 chunks; successful
    # configure/build logs must not consume the test diagnostic budget.
    name = ", ".join(item[0] for item in failures)
    details = "\n\n".join(label + "\n" + data for label, data in failures)
    raw = details.encode("utf-8")
    if len(raw) > 34000:
        details = (raw[:16500].decode("utf-8", errors="ignore") +
                   "\n... full diagnostics retained in the log artifact ...\n" +
                   raw[-16500:].decode("utf-8", errors="ignore"))
    chunks = list(byte_chunks(details))
    for index, chunk in enumerate(chunks, 1):
        message = f"{name} ({index}/{len(chunks)})\n{chunk}"
        print("::error title=SeExpr qualification::" + escape(message))


if __name__ == "__main__":
    main()
