"""Expose bounded SeExpr build and test failures through check annotations."""
import argparse
from pathlib import Path
import re


def escape(value):
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    args = parser.parse_args()
    found = False
    ansi = re.compile(r"\x1b\[[0-9;]*m")
    for report in args.logs:
        if not report.is_file():
            continue
        found = True
        data = ansi.sub("", report.read_text(encoding="utf-8", errors="replace"))
        # Include distinct compiler diagnostics even if they precede the last
        # output chunk; keep CTest assertion output and timeout details too.
        errors = list(dict.fromkeys(line for line in data.splitlines()
                      if re.search(r"(?:fatal )?error[: ]|CMake Error|undefined reference|"
                                   r"undefined symbol|FAILED|FAIL:|Exception|SegFault", line)))
        details = "\n".join(errors[:35]) + "\n" + data[-10000:]
        print("::error title=SeExpr qualification::" + escape(details))
    if not found:
        print("::error title=SeExpr qualification::The failed step produced no readable build or test report.")


if __name__ == "__main__":
    main()
