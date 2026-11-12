"""Public-API resource identity and actual painted-tip readback, not engine code."""
import base64
import hashlib
import math
import zlib
from pathlib import Path
from xml.etree.ElementTree import fromstring


def resolve(app, root, case, effective_fields):
    expected = case["resourceProbe"]
    filename = expected["filename"]
    if Path(filename).name != filename or not filename.startswith("gz-original-"):
        raise RuntimeError("Unexpected original resource identity")
    data = (root / "data" / "krita" / "brushes" / filename).read_bytes()
    if len(data) != expected["bytes"] or hashlib.sha256(data).hexdigest() != expected["sha256"]:
        raise RuntimeError("Original brush resource bytes changed")
    brush = fromstring(effective_fields["brush_definition"])
    if (brush.tag != "Brush" or Path(brush.get("filename", "")).name != filename or
            brush.get("type") != expected["type"]):
        raise RuntimeError("Source selected a different brush resource/type")
    checksum = brush.get("md5sum", "")
    if checksum and checksum.lower() != expected["md5"]:
        raise RuntimeError("Effective source brush checksum disagrees with original resource")
    if (abs(float(brush.get("spacing", "nan")) - case["spacing"]) > 1e-9 or
            not math.isfinite(float(brush.get("spacing", "nan")))):
        raise RuntimeError("Effective source resource spacing differs from authored fixture")
    matches = [r for r in app.resources("brush").values() if Path(r.filename()).name == filename]
    if len(matches) != 1:
        raise RuntimeError("Source resource registry has missing/ambiguous original brush")
    resource = matches[0]
    preview = resource.image()
    # Resource.image is documented as a representation, potentially an icon.
    # Do not claim it proves all GIH frames; actual stamp rasters are captured.
    preview_bytes = bytes(channel for y in range(min(preview.height(), 64))
                          for x in range(min(preview.width(), 64))
                          for channel in preview.pixelColor(x, y).getRgb())
    return {"filename": filename, "registryName": resource.name(), "registryType": resource.type(),
            "sha256OriginalFile": hashlib.sha256(data).hexdigest(), "effectiveBrushAttributes": dict(brush.attrib),
            "previewDimensions": [preview.width(), preview.height()],
            "previewFirst64x64RGBA8Sha256": hashlib.sha256(preview_bytes).hexdigest(),
            "previewIsNotFullTipProof": True}


def readback(raw, width, case):
    if "labelProbe" in case:
        from . import label_probe
        return label_probe.capture(raw, width, case)
    if "cadenceProbe" in case:
        from . import cadence_probe
        cadence_probe.validate(case)
    spec = case["resourceProbe"]
    left, top, w, h = spec["rect"]
    pixels = b"".join(raw[(y * width + left) * 4:(y * width + left + w) * 4]
                      for y in range(top, top + h))
    if spec["kind"] == "random" and sum(v != 0 for v in pixels[3::4]) != sum(v != 0 for v in raw[3::4]):
        raise RuntimeError("Source random stroke escaped fixed statistical region")
    result = {"rect": spec["rect"], "encoding": "zlib-base64-bgra8-row-major",
              "data": base64.b64encode(zlib.compress(pixels)).decode("ascii"),
              "actualPaintedTipReadback": True}
    if spec["kind"] in ("incremental", "random"):
        selections = []
        radius = math.ceil(case["diameter"] / 2) + 2
        for slot in range(spec["expectedSlots"]):
            cx = 256 + slot * case["diameter"] * case["spacing"]
            cy = 1024
            if "cadenceProbe" in case:
                cx, cy = case["cadenceCenters"][slot]
            sums = [0, 0, 0]
            for y in range(cy - radius, cy + radius + 1):
                for x in range(cx - radius, cx + radius + 1):
                    offset = (y * width + x) * 4
                    for c in range(3):
                        sums[c] += raw[offset + 2 - c] * raw[offset + 3]
            winner = max(range(3), key=lambda c: sums[c])
            if not any(sums):
                selections.append(None)  # Preserve endpoint/spacing observations.
            elif any(sums[c] for c in range(3) if c != winner):
                raise RuntimeError("Source GIH frame color is not an original primary-color stamp")
            else:
                selections.append(winner)
        if not any(value is not None for value in selections):
            raise RuntimeError("No original GIH frame identified in actual painted output")
        result["frameSelections"] = selections
    return result


def summarize(records):
    random = [r for r in records if "cadenceProbe" not in r and r.get("resourceProbe", {}).get("kind") == "random"]
    if not random:
        return None
    if len(random) != 100 or {r["resourceProbe"]["replicate"] for r in random} != set(range(100)):
        raise RuntimeError("Random GIH qualification requires all 100 authored strokes")
    counts = [0, 0, 0]
    transitions = [[0] * 3 for _ in range(3)]
    missing = 0
    sequences = []
    for record in random:
        seq = record["resourceReadback"]["frameSelections"]
        sequences.append(seq)
        for value in seq:
            if value is None:
                missing += 1
            else:
                counts[value] += 1
        for a, b in zip(seq, seq[1:]):
            if a is not None and b is not None:
                transitions[a][b] += 1
    n = sum(counts)
    z = 1.959963984540054
    intervals = []
    for count in counts:
        p = count / n
        center = (p + z * z / (2 * n)) / (1 + z * z / n)
        half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / (1 + z * z / n)
        intervals.append([center - half, center + half])
    return {"strokes": 100, "observedDabs": n, "missingAuthoredSlots": missing,
            "frameCounts": counts, "transitionCounts": transitions,
            "marginalWilson95": intervals,
            "intervalCaveat": "Marginal descriptive intervals assume independent dabs; transitions and per-stroke sequences retained to inspect dependence",
            "sourceSequences": sequences, "qualification": "source distribution acquired; candidate distribution comparison not yet qualified",
            "seedParityClaimed": False, "uniformityAssumed": False}
