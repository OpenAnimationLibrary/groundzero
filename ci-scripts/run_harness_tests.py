"""Run a Python CI contract suite with retained output and actionable annotations."""
import argparse
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
import sys
import unittest


def annotation(title, message):
    # Keep the actual assertion at the end of a traceback, within the checks
    # API's annotation bound. The UTF-8 report retains the complete traceback.
    raw = str(message).encode("utf-8", errors="backslashreplace")
    message = raw[-3500:].decode("utf-8", errors="ignore")
    def escape(value):
        return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    print("::error title=" + escape(title).replace(",", "%2C").replace(":", "%3A") + "::" + escape(message),
          flush=True)


class Tee:
    def __init__(self, *streams):
        self.streams = streams

    def write(self, value):
        for stream in self.streams:
            stream.write(value)
        return len(value)

    def flush(self):
        for stream in self.streams:
            stream.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pattern", required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--directory", type=Path,
                        default=Path(__file__).resolve().parent / "tests")
    args = parser.parse_args()
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    try:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        with args.report.open("w", encoding="utf-8", errors="backslashreplace") as report:
            transcript = Tee(sys.stdout, report)
            with redirect_stdout(transcript), redirect_stderr(transcript):
                suite = unittest.TestLoader().discover(str(args.directory), pattern=args.pattern)
                result = unittest.TextTestRunner(stream=transcript, verbosity=2).run(suite)
            for test, traceback in result.failures + result.errors:
                annotation(test.id(), traceback)
            for test in result.unexpectedSuccesses:
                annotation(test.id(), "Unexpected success in an expected-failure contract.")
            if not result.testsRun:
                annotation("Python harness discovery", "No tests matched " + args.pattern)
                return 1
            return 0 if result.wasSuccessful() else 1
    except Exception as error:
        annotation("Python harness runner", f"{type(error).__name__}: {error}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
