"""RCP1 scatter metrics for 100 independent original strokes; stdlib only.

This is an analysis gate, not a source RNG implementation or a profile verifier.
"""
import base64
import json
import math
from pathlib import Path
import sys
import zlib
from stochastic_gate import compare, read_report, SEED

RECTS = {"east": [128, 896, 1280, 256], "north": [896, 128, 256, 1280]}


def decode(item, direction):
    if direction not in RECTS or item.get("rect") != RECTS[direction]:
        raise ValueError("Invalid scatter capture rectangle")
    encoding = item.get("encoding")
    channels = 4 if encoding == "zlib-base64-bgra8-row-major" else 1 if encoding == "zlib-base64-alpha8-row-major" else 0
    if not channels:
        raise ValueError("Invalid scatter capture encoding")
    size = 1280 * 256 * channels
    data = item.get("data")
    if type(data) is not str or len(data) > 2 * size + 1024:
        raise ValueError("Scatter encoding exceeds bounded region")
    stream = zlib.decompressobj()
    pixels = stream.decompress(base64.b64decode(data, validate=True), size + 1)
    if len(pixels) != size or not stream.eof or stream.unconsumed_tail or stream.unused_data:
        raise ValueError("Invalid bounded scatter raster stream")
    alpha = pixels[3::4] if channels == 4 else pixels
    if direction == "north":
        # Align the public path direction for spatial bands. This is a fixed
        # coordinate transform of both rasters, never image registration.
        alpha = bytes(alpha[(1279 - x) * 256 + y] for y in range(256) for x in range(1280))
    return alpha


def measure(alpha, direction):
    if direction not in RECTS or not isinstance(alpha, (bytes, bytearray)) or len(alpha) != 1280 * 256:
        raise ValueError("Invalid normalized scatter alpha plane")
    width, height, diameter = 1280, 256, 8
    first = 128 if direction == "east" else 159
    result = {}
    for band in range(3):
        lo, hi = width * band // 3, width * (band + 1) // 3
        w = hi - lo
        data = [alpha[y * width + x] for y in range(height) for x in range(lo, hi)]
        coverage = sum(a != 0 for a in data)
        columns = sum(any(data[y * w + x] for y in range(height)) for x in range(w))
        occupied = total = 0
        for slot in range(32):
            center = first + slot * 32
            if not lo <= center < hi:
                continue
            total += 1
            occupied += any(alpha[y * width + x] for y in range(height)
                            for x in range(max(0, center - 16), min(width, center + 16)))
        if not total:
            raise ValueError("Scatter statistic band contains no authored tip cells")
        values = {"coverage": coverage / len(data),
                  "integratedAlpha": sum(data) / (255 * len(data)),
                  "meanWidth": coverage / max(1, columns) / diameter,
                  "occupiedTips": occupied / total}
        for scale in (1, 2, 4, 8):
            horizontal = sum((data[y * w + x] - data[y * w + x + scale]) ** 2
                             for y in range(height) for x in range(w - scale))
            vertical = sum((data[y * w + x] - data[(y + scale) * w + x]) ** 2
                           for y in range(height - scale) for x in range(w))
            count = height * (w - scale) + (height - scale) * w
            values["energy" + str(scale)] = (horizontal + vertical) / (255 * 255 * count)
        for name, value in values.items():
            result[str(band) + "/" + name] = value
    return result


def analyze(source_rows, candidate_rows):
    if len(source_rows) != 100 or len(candidate_rows) != 100:
        raise ValueError("Expected 100 independent enabled scatter strokes per application")
    expected = set(range(100))
    if {r.get("scatterProbe", {}).get("replicate") for r in source_rows} != expected:
        raise ValueError("Missing/duplicate source scatter replicate identities")
    ordinals = [r.get("executionOrdinal") for r in source_rows]
    if len(set(ordinals)) != 100 or any(type(v) is not int or v < 0 or v >= 102 for v in ordinals) or ordinals == sorted(ordinals):
        raise ValueError("Missing actual randomized source execution evidence")
    candidates = {r.get("preset"): r for r in candidate_rows}
    if len(candidates) != 100:
        raise ValueError("Duplicate candidate scatter identities")
    seeds = [r.get("candidateSeed") for r in candidate_rows]
    if any(type(s) is not str or not s.isascii() or not s.isdecimal() or len(s) > 20 or
           not 0 < int(s) < 2**64 or str(int(s)) != s for s in seeds) or len(set(seeds)) != 100:
        raise ValueError("Candidate scatter streams are not distinct canonical seeds")
    measurements = [[], []]
    configuration = None
    for row in source_rows:
        spec = row["scatterProbe"]
        settings = (spec.get("axes"), spec.get("direction"), spec.get("amount"))
        if (settings[0] not in ("x", "y", "xy") or settings[1] not in RECTS or settings[2] != 1 or
                spec.get("enabled") is not True or spec.get("replicates") != 100):
            raise ValueError("Unexpected scatter configuration")
        configuration = settings if configuration is None else configuration
        if settings != configuration:
            raise ValueError("Different scatter configurations were pooled")
        other = candidates.pop(row["preset"], None)
        if other is None:
            raise ValueError("Missing matching candidate scatter stroke")
        for index, item in enumerate((row["scatterEvidence"], other["stochasticAlpha"])):
            measurements[index].append(measure(decode(item, settings[1]), settings[1]))
    gates = {key: compare([m[key] for m in measurements[0]], [m[key] for m in measurements[1]])
             for key in measurements[0][0]}
    return {"status": "original-fixture-statistics-passed" if all(g["passed"] for g in gates.values()) else "statistical-gates-failed",
            "configuration": configuration, "strokesPerApplication": 100,
            "bootstrapResamples": 10000, "analysisSeed": SEED, "bands": 3,
            "pixelParityClaimed": False, "sourceProfileVerified": False,
            "qualification": "Original scatter configuration only; source semantic admission, wider combinations and artist review remain separate",
            "gates": gates, "strokeMeasurements": measurements}


def run(root):
    out = root / "output"
    oracle, comparison = read_report(out / "oracle-status.json"), read_report(out / "comparison.json")
    if oracle.get("status") != "complete" or oracle.get("version") != "5.3.4.1":
        raise ValueError("Source scatter acquisition is incomplete or has another baseline")
    source = [r for r in oracle["records"] if r.get("scatterProbe", {}).get("enabled") is True]
    candidate = [r for r in comparison["records"] if r.get("inputCase", {}).get("scatterProbe", {}).get("enabled") is True]
    report = analyze(source, candidate)
    (out / "scatter-statistics.json").write_text(json.dumps(report, indent=2, allow_nan=False))
    for name, gate in report["gates"].items():
        print(("::notice::" if gate["passed"] else "::error::") + name + " " + json.dumps(gate))
    return report["status"] == "original-fixture-statistics-passed"


if __name__ == "__main__":
    sys.exit(0 if run(Path(sys.argv[1])) else 1)
