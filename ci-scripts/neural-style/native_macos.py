"""Qualify the universal macOS C API against the Linux model-reference fixture."""
import json
from pathlib import Path
import platform
import sys

import numpy as np
from smoke import native_check


def main():
    report, native, fixture = map(Path, sys.argv[1:])
    with np.load(fixture, allow_pickle=False) as data:
        cases = [(1, 1, data["style"], data["embedding"]),
                 (2, 2, data["content"], data["output"]),
                 (2, 3, data["content"], data["output"]),
                 (1, 4, data["other_style"], data["other_embedding"]),
                 (2, 5, data["content"], data["other"]),
                 (1, 6, data["style"], data["embedding"]),
                 (2, 7, data["other_content"], data["changed_content"])]
        result = native_check(native, cases)
        host = report / "host-fixtures"
        host.mkdir()
        for name in ("reference", "alternate", "content", "expected", "alternate-expected"):
            array = data["host_" + name]
            side = 256 if name in ("reference", "alternate") else 384
            if array.dtype != np.uint8 or array.shape != (1, side, side, 3):
                raise RuntimeError("Invalid same-revision host fixture")
            (host / (name + ".rgb")).write_bytes(array.tobytes())
    result.update({"platform": platform.system(), "machine": platform.machine(),
                   "reference": "Same-revision Linux Python/LiteRT qualification fixture"})
    (report / "inference.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print("::notice title=Intel macOS native qualification::" + json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
