"""Original public-API diagnostics; deliberately separate from source parity."""
import base64
import hashlib
import json
import struct
import sys
import zlib

PROFILE = "sRGB-elle-V2-srgbtrc.icc"
COUNT = 58
WIDTH, HEIGHT = 1024, 256


def plan(fixtures):
    selected = []
    for diameter, radius in ((56, 100), (64, 100), (32, 250), (64, 150)):
        cases = [c for c in fixtures["cases"] if c.get("wetPattern") == "opaque-black-white-points"
                 and c["diameter"] == diameter and
                 c["overrides"]["SmudgeRadiusValue"] == str(radius)]
        ranks = sorted({c["wetKernelOffset"] for c in cases})
        if len(ranks) != 3 or len(cases) != 6:
            raise RuntimeError("Missing explicit kernel precision masks")
        selected.extend(c for c in cases if c["wetKernelOffset"] in (ranks[0], ranks[-1]))
    result = []
    for depth in ("U8", "U16"):
        for case in selected:
            result.append({"case": case, "depth": depth, "low": 0, "high": 255, "dabs": 2})
        base = next(c for c in selected if c["diameter"] == 64 and
                    c["overrides"]["SmudgeRadiusValue"] == "100")
        for gray in (0, 64, 128, 192, 255):
            result.append({"case": base, "depth": depth, "low": gray, "high": gray, "dabs": 2})
        for diameter, radius in ((56, 100), (64, 100), (32, 250), (64, 150)):
            case = next(c for c in selected if c["diameter"] == diameter and
                        c["overrides"]["SmudgeRadiusValue"] == str(radius) and
                        not c["wetKernelComplement"])
            result.append({"case": case, "depth": depth, "low": 0, "high": 255, "dabs": 3})
    # Same-U8 non-extreme complements distinguish clipping/fast paths from a
    # different-depth engine path. They are not inferred from U16 behavior.
    for diameter, radius in ((56, 100), (64, 100), (32, 250), (64, 150)):
        subset = [c for c in selected if c["diameter"] == diameter and
                  c["overrides"]["SmudgeRadiusValue"] == str(radius)]
        first = min(c["wetKernelOffset"] for c in subset)
        result.extend({"case": c, "depth": "U8", "low": 64, "high": 192, "dabs": 2}
                      for c in subset if c["wetKernelOffset"] == first)
    if len(result) != COUNT:
        raise RuntimeError("Unexpected kernel precision diagnostic count")
    return result


def original_second_sample(record, case):
    # Oracle status contains the complete scanline, whereas notice summaries
    # retain only probePositions. The last full scan entry is not the last dab.
    target = 256 + int(case["diameter"] * case["spacing"])
    matches = [sample[1:] for sample in record["centerlineRGBA8"]
               if len(sample) == 5 and sample[0] == target]
    if len(matches) != 1:
        raise ValueError("Missing unique original second-dab center sample")
    return matches[0]


def canonical(raw, depth):
    stride = 4 if depth == "U8" else 8
    alpha = 3 if depth == "U8" else 6
    result = bytearray(raw)
    for offset in range(0, len(result), stride):
        if not any(result[offset + alpha:offset + stride]):
            result[offset:offset + alpha] = bytes(alpha)
    return bytes(result)


def render(app, output, fixtures, presets, original_records, checkpoint):
    from krita import Preset
    from PyQt5.QtCore import QByteArray, QPoint
    if sys.byteorder != "little":
        raise RuntimeError("Kernel U16 diagnostic requires explicitly little-endian raw storage")
    if not all(depth in app.colorDepths("RGBA") and PROFILE in app.profiles("RGBA", depth)
               for depth in ("U8", "U16")):
        raise RuntimeError("Exact shared U8/U16 diagnostic profile unavailable")
    originals = {r["preset"]: r for r in original_records}
    documents = {}
    records = []
    try:
        for ordinal, item in enumerate(plan(fixtures)):
            case, depth = item["case"], item["depth"]
            checkpoint("kernel precision diagnostic " + str(ordinal))
            if depth not in documents:
                document = app.createDocument(WIDTH, HEIGHT, "Original kernel precision " + depth,
                                              "RGBA", depth, PROFILE, 72.0)
                view = app.activeWindow().addView(document)
                app.setActiveDocument(document)
                node = document.createNode("Original raw diagnostic pixels", "paintlayer")
                document.rootNode().addChildNode(node, None)
                document.setActiveNode(node)
                documents[depth] = (document, view, node)
                yield
            document, view, node = documents[depth]
            app.setActiveDocument(document)
            if (document.colorModel(), document.colorDepth(), document.colorProfile()) != ("RGBA", depth, PROFILE):
                raise RuntimeError("Kernel diagnostic document identity changed")
            if (node.colorModel(), node.colorDepth(), node.colorProfile()) != ("RGBA", depth, PROFILE):
                raise RuntimeError("Kernel diagnostic node identity changed")
            scale, stride = (1, 4) if depth == "U8" else (257, 8)
            def pixel(gray):
                v, a = gray * scale, 255 * scale
                return bytes((v, v, v, a)) if depth == "U8" else struct.pack("<4H", v, v, v, a)
            low, high = item["low"], item["high"]
            background, marked = (high, low) if case["wetKernelComplement"] else (low, high)
            boundary = int(case["wetBoundary"] + .999999)
            row = pixel(background) * boundary + bytes((WIDTH - boundary) * stride)
            initial = bytearray(row * HEIGHT)
            for dx, dy in case["wetKernelPoints"]:
                offset = ((128 + dy) * WIDTH + 256 + dx) * stride
                initial[offset:offset + stride] = pixel(marked)
            initial = bytes(initial)
            raw_outputs = []
            settings_hashes = []
            for repetition in range(2):
                view.setCurrentBrushPreset(presets[case["preset"]])
                view.setBrushSize(case["diameter"])
                view.setPaintingOpacity(1)
                view.setPaintingFlow(1)
                view.setBrushRotation(0)
                view.setEraserMode(False)
                view.setGlobalAlphaLock(False)
                view.setDisablePressure(False)
                if view.currentBrushPreset().name() != case["preset"]:
                    raise RuntimeError("Kernel diagnostic preset selection changed")
                effective = Preset(view.currentBrushPreset()).toXML()
                settings_hashes.append(hashlib.sha256(effective.encode()).hexdigest())
                if settings_hashes[-1] != originals[case["preset"]]["effectivePresetSha256"]:
                    raise RuntimeError("Kernel diagnostic preset differs from original U8 controls")
                if not node.setPixelData(QByteArray(initial), 0, 0, WIDTH, HEIGHT):
                    raise RuntimeError("Kernel diagnostic raw initialization failed")
                document.refreshProjection()
                document.waitForDone()
                if bytes(node.pixelData(0, 0, WIDTH, HEIGHT)) != initial:
                    raise RuntimeError("Kernel diagnostic raw initial round-trip differs")
                endpoint = 256 + int(case["diameter"] * case["spacing"]) * (item["dabs"] - 1) + 1
                node.paintLine(QPoint(256, 128), QPoint(endpoint, 128), 1., 1., "ForegroundColor")
                document.waitForDone()
                raw = bytes(node.pixelData(0, 0, WIDTH, HEIGHT))
                if len(raw) != WIDTH * HEIGHT * stride:
                    raise RuntimeError("Kernel diagnostic raw output incomplete")
                raw_outputs.append(raw)
                document.setModified(False)
                yield
            if canonical(raw_outputs[0], depth) != canonical(raw_outputs[1], depth):
                raise RuntimeError("Kernel diagnostic visible or alpha repeat mismatch")
            raw = raw_outputs[0]
            samples = []
            for dab in range(item["dabs"]):
                x = 256 + int(case["diameter"] * case["spacing"]) * dab
                offset = (128 * WIDTH + x) * stride
                bgra = list(raw[offset:offset + stride]) if depth == "U8" else list(struct.unpack("<4H", raw[offset:offset + stride]))
                samples.append([x, 128, bgra[2], bgra[1], bgra[0], bgra[3]])
            equivalent = depth == "U8" and low == 0 and high == 255 and item["dabs"] == 2
            if equivalent:
                expected = original_second_sample(originals[case["preset"]], case)
                if samples[-1][2:] != expected:
                    raise RuntimeError("Translated U8 kernel diagnostic differs from original coordinates: " +
                                       json.dumps({"id": ordinal, "preset": case["preset"],
                                                   "sourceX": samples[-1][0], "actualRGBA": samples[-1][2:],
                                                   "expectedRGBA": expected}, separators=(",", ":")))
            radius = int(case["diameter"] * max(1, float(case["overrides"]["SmudgeRadiusValue"]) / 100) / 2) + 2
            cx = samples[-1][0]
            rect = [cx - radius, 128 - radius, 2 * radius + 1, 2 * radius + 1]
            crop = b"".join(raw[(y * WIDTH + rect[0]) * stride:(y * WIDTH + rect[0] + rect[2]) * stride]
                            for y in range(rect[1], rect[1] + rect[3]))
            records.append({"id": ordinal, "preset": case["preset"], "depth": depth, "profile": PROFILE,
                            "low": low, "high": high, "dabs": item["dabs"],
                            "pointCount": len(case["wetKernelPoints"]), "colorModel": "RGBA",
                            "runtimeVersion": app.version(), "canvas": [WIDTH, HEIGHT],
                            "maximumComponent": 255 * scale, "samplesRGBA": samples,
                            "translationControl": equivalent, "initialRawSha256": hashlib.sha256(initial).hexdigest(),
                            "rawSha256": [hashlib.sha256(b).hexdigest() for b in raw_outputs],
                            "canonicalSha256": hashlib.sha256(canonical(raw, depth)).hexdigest(),
                            "effectivePresetSha256": settings_hashes[0], "rawInitialRoundTrip": True,
                            "repeatEquivalent": True, "rect": rect,
                            "encoding": "zlib-base64-bgra" + ("8" if depth == "U8" else "16le") + "-row-major",
                            "data": base64.b64encode(zlib.compress(crop)).decode("ascii")})
            (output / "kernel-precision.json").write_text(json.dumps({"schema": 1, "status": "running",
                "qualification": "research-only; original U8 parity gates remain separate", "records": records}, indent=2))
        (output / "kernel-precision.json").write_text(json.dumps({"schema": 1, "status": "complete",
            "qualification": "research-only; original U8 parity gates remain separate", "records": records}, indent=2))
    finally:
        # Two bounded 1024x256 documents stay alive for ordinary window shutdown,
        # matching the main harness's safe native view lifetime policy.
        for document, view, node in documents.values():
            document.setModified(False)


def validate(report, fixtures, originals):
    """Validate research completeness independently of the renderer's U8 gates."""
    def require(condition, message):
        if not condition:
            raise ValueError("Kernel precision: " + message)
    def sha(value):
        return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)
    require(report.get("schema") == 1 and report.get("status") == "complete" and
            report.get("qualification") == "research-only; original U8 parity gates remain separate",
            "incomplete or misclassified diagnostics")
    records = report.get("records", [])
    require(len(records) == COUNT, "missing diagnostic cases")
    original = {r["preset"]: r for r in originals}
    for ordinal, (item, row) in enumerate(zip(plan(fixtures), records)):
        case, depth = item["case"], item["depth"]
        require(row.get("id") == ordinal and row.get("preset") == case["preset"] and
                row.get("depth") == depth and row.get("profile") == PROFILE and
                row.get("pointCount") == len(case["wetKernelPoints"]) and
                row.get("colorModel") == "RGBA" and row.get("runtimeVersion") == fixtures["version"] and
                row.get("canvas") == [WIDTH, HEIGHT] and
                all(row.get(key) == item[key] for key in ("low", "high", "dabs")),
                "stale diagnostic inputs")
        maximum, stride = (255, 4) if depth == "U8" else (65535, 8)
        require(row.get("maximumComponent") == maximum and row.get("rawInitialRoundTrip") is True and
                row.get("repeatEquivalent") is True, "unverified raw identity/repeat")
        require(all(sha(row.get(key)) for key in ("initialRawSha256", "canonicalSha256", "effectivePresetSha256")) and
                len(row.get("rawSha256", [])) == 2 and all(sha(v) for v in row["rawSha256"]),
                "invalid evidence digest")
        require(row["effectivePresetSha256"] == original[case["preset"]]["effectivePresetSha256"],
                "preset identity changed")
        samples = row.get("samplesRGBA", [])
        require(len(samples) == item["dabs"], "missing dab samples")
        for dab, sample in enumerate(samples):
            require(isinstance(sample, list) and len(sample) == 6 and
                    sample[:2] == [256 + int(case["diameter"] * case["spacing"]) * dab, 128] and
                    all(type(c) is int and 0 <= c <= maximum for c in sample[2:]), "invalid raw sample")
        equivalent = depth == "U8" and item["low"] == 0 and item["high"] == 255 and item["dabs"] == 2
        require(row.get("translationControl") is equivalent, "missing translation classification")
        if equivalent:
            require(samples[-1][2:] == original_second_sample(original[case["preset"]], case),
                    "translated U8 control changed")
        radius = int(case["diameter"] * max(1, float(case["overrides"]["SmudgeRadiusValue"]) / 100) / 2) + 2
        rect = [samples[-1][0] - radius, 128 - radius, 2 * radius + 1, 2 * radius + 1]
        require(row.get("rect") == rect and row.get("encoding") ==
                "zlib-base64-bgra" + ("8" if depth == "U8" else "16le") + "-row-major",
                "invalid raw crop format")
        expected = rect[2] * rect[3] * stride
        require(expected <= 128 * 128 * 8 and isinstance(row.get("data"), str) and
                len(row["data"]) <= 200000, "raw crop budget")
        compressed = base64.b64decode(row["data"], validate=True)
        decoder = zlib.decompressobj()
        raw = decoder.decompress(compressed, expected + 1)
        require(len(raw) == expected and decoder.eof and not decoder.unused_data and not decoder.unconsumed_tail,
                "invalid or oversized compressed raw crop")
