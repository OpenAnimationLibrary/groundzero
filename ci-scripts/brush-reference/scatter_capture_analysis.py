"""Read-only original scatter process diagnostics; no renderer or admission gate.

Connected components are matched by the authored stroke order, using disabled
controls from the same application. No image is translated, registered, fitted,
or used as a candidate lookup. These descriptive measurements supplement, and
never replace, the unchanged RCP1 aggregate statistical gates.
"""
import argparse
import base64
import collections
import hashlib
import json
import math
from pathlib import Path
import statistics
import zlib


RECTS = {"east": [128, 896, 1280, 256], "north": [896, 128, 256, 1280]}


def decode(evidence, direction):
    if not isinstance(evidence, dict) or evidence.get("rect") != RECTS.get(direction):
        raise ValueError("Unexpected original scatter capture rectangle")
    channels = {"zlib-base64-bgra8-row-major": 4,
                "zlib-base64-alpha8-row-major": 1}.get(evidence.get("encoding"))
    if channels is None:
        raise ValueError("Unexpected captured pixel encoding")
    size = 1280 * 256 * channels
    encoded = evidence.get("data")
    if not isinstance(encoded, str) or len(encoded) > 2 * size + 1024:
        raise ValueError("Capture exceeds original bounded raster contract")
    stream = zlib.decompressobj()
    raw = stream.decompress(base64.b64decode(encoded, validate=True), size + 1)
    if len(raw) != size or not stream.eof or stream.unconsumed_tail or stream.unused_data:
        raise ValueError("Incomplete or concatenated original capture")
    return raw[3::4] if channels == 4 else raw


def components(alpha, width, height, direction):
    if (type(width) is not int or type(height) is not int or width <= 0 or height <= 0 or
            len(alpha) != width * height or direction not in RECTS):
        raise ValueError("Invalid component analysis plane")
    remaining = {i for i, a in enumerate(alpha) if a}
    result = []
    while remaining:
        queue, pixels = [remaining.pop()], []
        while queue:
            i = queue.pop()
            pixels.append(i)
            x, y = i % width, i // width
            for neighbor in (i - 1, i + 1, i - width, i + width):
                if (neighbor in remaining and
                        abs(neighbor % width - x) + abs(neighbor // width - y) == 1):
                    remaining.remove(neighbor)
                    queue.append(neighbor)
        mass = sum(alpha[i] for i in pixels)
        result.append({"centroid": [sum((i % width) * alpha[i] for i in pixels) / mass,
                                     sum((i // width) * alpha[i] for i in pixels) / mass],
                       "alphaMassCodes": mass, "nonzeroPixels": len(pixels),
                       "bounds": [min(i % width for i in pixels), min(i // width for i in pixels),
                                  max(i % width for i in pixels), max(i // width for i in pixels)]})
    return sorted(result, key=lambda c: c["centroid"][0] if direction == "east" else -c["centroid"][1])


def moment(values):
    if not values or not all(math.isfinite(v) for v in values):
        raise ValueError("Invalid displacement observations")
    return {"count": len(values), "min": min(values), "max": max(values),
            "mean": statistics.mean(values), "sampleSD": statistics.stdev(values) if len(values) > 1 else 0,
            "rms": math.sqrt(sum(v * v for v in values) / len(values))}


def vector_moments(points, unit):
    ux, uy = unit
    xs, ys = [p[0] for p in points], [p[1] for p in points]
    mx, my = statistics.mean(xs), statistics.mean(ys)
    vx = statistics.variance(xs) if len(xs) > 1 else 0
    vy = statistics.variance(ys) if len(ys) > 1 else 0
    covariance = sum((x - mx) * (y - my) for x, y in points) / max(1, len(points) - 1)
    angle = math.degrees(.5 * math.atan2(2 * covariance, vx - vy)) if vx + vy else None
    return {"x": moment(xs), "y": moment(ys), "xySampleCovariance": covariance,
            "principalAxisDegreesModulo180": angle,
            "parallelToAuthoredPath": moment([x * ux + y * uy for x, y in points]),
            "perpendicularToAuthoredPath": moment([-x * uy + y * ux for x, y in points])}


def analyze_series(planes, control, width, height, direction, start, end, slots=32):
    if len(planes) != 100:
        raise ValueError("Process diagnostics require exactly 100 independent strokes")
    base = components(control, width, height, direction)
    if len(base) != slots:
        raise ValueError("Disabled control does not contain the declared disconnected dabs")
    dx, dy = end[0] - start[0], end[1] - start[1]
    distance = math.hypot(dx, dy)
    if not distance:
        raise ValueError("Authored stroke has no direction")
    unit = dx / distance, dy / distance
    first, later, rows, counts, sequences = [], [], [], collections.Counter(), set()
    for plane in planes:
        cells = components(plane, width, height, direction)
        if len(cells) != slots:
            raise ValueError("Disconnected-dab diagnostic cannot resolve this captured stroke")
        offsets = [[c["centroid"][0] - b["centroid"][0], c["centroid"][1] - b["centroid"][1]]
                   for c, b in zip(cells, base)]
        first.append(offsets[0])
        later.extend(offsets[1:])
        counts.update(c["nonzeroPixels"] for c in cells)
        sequence = hashlib.sha256(json.dumps(offsets, separators=(",", ":"), allow_nan=False).encode()).hexdigest()
        sequences.add(sequence)
        rows.append({"firstOffsetPixels": offsets[0], "laterDabs": vector_moments(offsets[1:], unit),
                     "centroidSequenceSha256": sequence})
    start_length = math.hypot(*start)
    origin_angle = math.degrees(math.atan2(start[1], start[0])) if start_length else None
    return {"strokes": 100, "dabsPerStroke": slots,
            "uniqueRasterCount": len({hashlib.sha256(p).hexdigest() for p in planes}),
            "uniqueCentroidSequenceCount": len(sequences),
            "alphaCodeValues": sorted({a for p in planes for a in p}),
            "dabAreaPixelCounts": dict(sorted(counts.items())), "disabledControlDabs": base,
            "firstDab": vector_moments(first, unit), "laterDabs": vector_moments(later, unit),
            "globalOriginRadialDirectionHypothesisDegrees": origin_angle,
            "directionHypothesisQualified": False, "strokeMeasurements": rows}


def analyze(root):
    out = root / "output" if (root / "output").is_dir() else root
    oracle_path, candidate_path = out / "oracle-status.json", out / "comparison.json"
    oracle, candidate = json.loads(oracle_path.read_text()), json.loads(candidate_path.read_text())
    source_rows, candidate_rows = oracle.get("records", []), candidate.get("records", [])
    if oracle.get("status") != "complete" or len(source_rows) != 102 or len(candidate_rows) != 102:
        raise ValueError("A complete original 100-stroke plus two-control acquisition is required")
    if len({r["preset"] for r in source_rows}) != 102 or {r["preset"] for r in source_rows} != {r["preset"] for r in candidate_rows}:
        raise ValueError("Source/candidate capture identities disagree")
    source_rows = sorted(source_rows, key=lambda r: r["preset"])
    candidates = {r["preset"]: r for r in candidate_rows}
    first = source_rows[0]
    spec, direction = first["scatterProbe"], first["scatterProbe"]["direction"]
    if direction not in RECTS or spec.get("axes") not in ("x", "y", "xy") or spec.get("amount") != 1:
        raise ValueError("This diagnostic only describes the original bounded configuration")
    for row in source_rows:
        s = row["scatterProbe"]
        if (any(s.get(k) != spec.get(k) for k in ("axes", "direction", "amount")) or
                row["start"] != first["start"] or row["end"] != first["end"] or row["diameter"] != 8 or
                not row["scatterEvidence"].get("effectiveControls") or
                not row["scatterEvidence"].get("effectiveBrushDefinition")):
            raise ValueError("Missing original controls or mixed configuration")
    active = [r for r in source_rows if r["scatterProbe"]["enabled"] is True]
    controls = [r for r in source_rows if r["scatterProbe"]["enabled"] is False]
    if ({r["scatterProbe"]["replicate"] for r in active} != set(range(100)) or
            {r["scatterProbe"]["replicate"] for r in controls} != {0, 1}):
        raise ValueError("Missing original replicate identities")
    width, height = RECTS[direction][2:]
    result = {"schema": 1, "configuration": {"axes": spec["axes"], "direction": direction,
              "amount": 1, "diameter": 8, "spacing": 4, "start": first["start"], "end": first["end"]},
              "sourceProfileVerified": False, "sourceRngLawIdentified": False, "pixelParityClaimed": False,
              "gate": False, "qualification": "Read-only process research; unchanged RCP1 gates and source admission remain separate",
              "inputSha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (oracle_path, candidate_path)}}
    for label in ("source", "candidate"):
        def evidence(row):
            return row["scatterEvidence"] if label == "source" else candidates[row["preset"]]["stochasticAlpha"]
        control_planes = [decode(evidence(row), direction) for row in controls]
        if control_planes[0] != control_planes[1]:
            raise ValueError("Independent disabled controls disagree")
        result[label] = analyze_series([decode(evidence(row), direction) for row in active],
                                       control_planes[0], width, height, direction, first["start"], first["end"])
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.capture), indent=2, allow_nan=False))
