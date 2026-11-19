"""Record the FX render matrix cells of this platform: the one command that writes them.

    mcp_fx_matrix_record.py --executable APP --media-tools DIR --reason "why" [--only TYPE ...]

Renders the admitted types through the worker of APP (see `mcp_fx_matrix_cases.py` for what a
cell measures), stores the results as a new run of this platform in `mcp_fx_matrix.json` and
regenerates `doc/mcp_fx_render_matrix.md`. It refuses in CI and without a reason of at least
20 characters. `--only` stores a subset: the platform is then incomplete until a full run.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
from pathlib import Path

from mcp_acceptance import NativeAcceptance
from mcp_fx_matrix import (
    DATA_PATH,
    DOCUMENT_PATH,
    admitted_types,
    document_of,
    dump_data,
    load_data,
)
from mcp_fx_matrix_cases import apply_run, measure

MINIMUM_REASON = 20
SCRATCH = Path("/private/tmp/gzq")  # short and canonical: the worker has a socket path limit


class Recorder(NativeAcceptance):
    """A worker and the helpers of the acceptance suite, outside a test run."""

    def runTest(self) -> None:  # noqa: N802 - the name unittest calls
        """Nothing runs; the recorder uses the helpers."""


def record(executable: Path, media: Path | None, reason: str, only: list[str]) -> None:
    """Measure the cells of this platform and replace them in the data, with the reason."""
    if os.environ.get("CI"):
        raise SystemExit("the matrix is never recorded in CI; run this by hand")
    if len(reason.strip()) < MINIMUM_REASON:
        raise SystemExit(f"give a --reason of at least {MINIMUM_REASON} characters")
    types = admitted_types()
    unknown = sorted(set(only) - set(types))
    if unknown:
        raise SystemExit(f"not admitted types: {unknown}")
    os.environ.setdefault("QT_QPA_PLATFORM", "cocoa" if sys.platform == "darwin" else "xcb")
    Recorder.executable = executable.resolve(strict=True)
    Recorder.output = Path(tempfile.mkdtemp(prefix="fxm-", dir=SCRATCH)).resolve()
    Recorder.media_tools = media
    recorder = Recorder()
    recorder.setUp()
    try:
        results, identity = measure(recorder, only or types, media)
    finally:
        recorder.doCleanups()
        shutil.rmtree(Recorder.output, ignore_errors=True)
    data = load_data()
    apply_run(data, results, identity, reason)
    DATA_PATH.write_text(dump_data(data), encoding="utf-8")
    DOCUMENT_PATH.write_text(document_of(data, types), encoding="utf-8")


def main() -> int:
    """Record the cells of this platform."""
    parser = argparse.ArgumentParser(description=__doc__)
    recording = parser
    recording.add_argument("--executable", type=Path, required=True)
    recording.add_argument("--media-tools", type=Path)
    recording.add_argument("--reason", default="")
    recording.add_argument("--only", nargs="*", default=[])
    arguments = parser.parse_args()
    record(arguments.executable, arguments.media_tools, arguments.reason, arguments.only)
    return 0


if __name__ == "__main__":
    sys.exit(main())
