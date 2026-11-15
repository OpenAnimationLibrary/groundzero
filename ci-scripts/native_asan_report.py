"""Report AddressSanitizer logs of the native UI run as workflow annotations.

Memory errors (use after free, overflow, bad free) fail the run. Leaks are
summarized as a notice: a Qt application reports framework and singleton
leaks at exit, so they need a reviewed baseline before they can fail a run.
"""

import re
import sys
from pathlib import Path

MAX_ERRORS = 10
MAX_FRAMES = 16
MAX_LOG_BYTES = 8 * 1024 * 1024

ERROR = re.compile(r"^==\d+==ERROR: (AddressSanitizer|LeakSanitizer): (.*)$")
LEAK_TOTAL = re.compile(
    r"^SUMMARY: AddressSanitizer: (\d+) byte\(s\) leaked in (\d+) allocation\(s\)"
)
FRAME = re.compile(r"^\s+#\d+ ")


def escape(text):
    """Escape a workflow command message."""
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def parse(text):
    """Return (memory errors, leaked bytes, leaked allocations) of one log."""
    errors, leaked_bytes, leaked_allocations = [], 0, 0
    lines = text.splitlines()
    for index, line in enumerate(lines):
        total = LEAK_TOTAL.match(line)
        if total:
            leaked_bytes += int(total.group(1))
            leaked_allocations += int(total.group(2))
            continue
        found = ERROR.match(line)
        if not found or found.group(1) == "LeakSanitizer":
            continue
        frames = []
        for following in lines[index + 1 :]:
            if FRAME.match(following):
                frames.append(following.strip())
                if len(frames) == MAX_FRAMES:
                    break
            elif frames:
                break
        errors.append((found.group(2).strip(), frames))
    return errors, leaked_bytes, leaked_allocations


def report(directory):
    """Print annotations for every log in directory; return the exit status."""
    logs = sorted(path for path in Path(directory).glob("asan.*") if path.is_file())
    errors, leaked_bytes, leaked_allocations = [], 0, 0
    for log in logs:
        text = log.read_bytes()[:MAX_LOG_BYTES].decode("utf-8", errors="replace")
        found, log_bytes, log_allocations = parse(text)
        errors.extend(found)
        leaked_bytes += log_bytes
        leaked_allocations += log_allocations
    for description, frames in errors[:MAX_ERRORS]:
        message = description + "\n" + "\n".join(frames)
        print("::error title=AddressSanitizer::" + escape(message))
    if len(errors) > MAX_ERRORS:
        print(
            "::error title=AddressSanitizer::"
            f"{len(errors) - MAX_ERRORS} more memory errors are in the retained logs"
        )
    print(
        "::notice title=LeakSanitizer (not failing)::"
        f"{leaked_bytes} bytes in {leaked_allocations} allocations leaked across "
        f"{len(logs)} sanitizer logs; stacks are in the retained logs"
    )
    return 1 if errors else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: native_asan_report.py <sanitizer log directory>")
    sys.exit(report(sys.argv[1]))
