"""Original raw U16 impulse controls for sampler geometry and duplicate hits.

This records outputs, not inferred source counts or a fitted sampler formula.
"""
import base64
import hashlib
import json
import math
import struct
import sys
import zlib

PROFILE = "sRGB-elle-V2-srgbtrc.icc"
QUALIFICATION = "research-only raw U16 single-mark sampling; no source count claimed"
WIDTH, HEIGHT = 1024, 256
COMBINATIONS = ((56, 100), (64, 100), (32, 250), (64, 150),
                (72, 100), (88, 100), (120, 100))
LOW, HIGH = 112, 144
COUNT = 70


def plan(fixtures):
    if fixtures.get("version") != "5.3.4.1" or fixtures.get("group") != "wetkernelcount":
        raise ValueError("Single-mark diagnostics require the pinned kernel group")
    result = []
    for diameter, radius in COMBINATIONS:
        cases = [case for case in fixtures.get("cases", [])
                 if case.get("wetPattern") == "opaque-black-white-points"
                 and case.get("diameter") == diameter
                 and case.get("overrides", {}).get("SmudgeRadiusValue") == str(radius)
                 and case.get("wetKernelComplement") is False]
        if not cases:
            raise ValueError("Missing original single-mark geometry preset")
        minimum = min(case["wetKernelOffset"] for case in cases)
        cases = [case for case in cases if case["wetKernelOffset"] == minimum]
        if len(cases) != 1:
            raise ValueError("Ambiguous original single-mark geometry preset")
        case = cases[0]
        points = case.get("wetKernelPoints", [])
        if (not 4 <= len(points) <= 256 or case.get("spacing") != 4 or
                not all(isinstance(point, list) and len(point) == 2 and
                        all(type(v) is int and abs(v) <= 64 for v in point)
                        for point in points) or len({tuple(p) for p in points}) != len(points)):
            raise ValueError("Invalid original quarter-mask coordinates")
        selected = [points[index] for index in (0, len(points) // 3,
                                               2 * len(points) // 3, len(points) - 1)]
        for coordinate in selected:
            for complement in (False, True):
                result.append({"preset": case["preset"], "diameter": diameter,
                               "radiusPercent": radius, "spacing": 4,
                               "markedCoordinate": coordinate,
                               "backgroundGray": HIGH if complement else LOW,
                               "markedGray": LOW if complement else HIGH,
                               "uniform": False})
        for gray in (LOW, HIGH):
            result.append({"preset": case["preset"], "diameter": diameter,
                           "radiusPercent": radius, "spacing": 4,
                           "markedCoordinate": None, "backgroundGray": gray,
                           "markedGray": gray, "uniform": True})
    if len(result) != COUNT:
        raise ValueError("Unexpected single-mark diagnostic cardinality")
    return result


def initial(item):
    boundary = math.ceil(256 + item["diameter"] * item["spacing"] * .8)
    gray = item["backgroundGray"] * 257
    row = struct.pack("<4H", gray, gray, gray, 65535) * boundary
    row += bytes((WIDTH - boundary) * 8)
    data = bytearray(row * HEIGHT)
    if item["markedCoordinate"] is not None:
        dx, dy = item["markedCoordinate"]
        index = ((128 + dy) * WIDTH + 256 + dx) * 8
        gray = item["markedGray"] * 257
        data[index:index + 8] = struct.pack("<4H", gray, gray, gray, 65535)
    return bytes(data)


def sample(raw, x, y):
    b, g, r, a = struct.unpack_from("<4H", raw, (y * WIDTH + x) * 8)
    return [x, y, r, g, b, a]


def canonical(raw):
    result = bytearray(raw)
    for index in range(0, len(result), 8):
        if not any(result[index + 6:index + 8]):
            result[index:index + 6] = bytes(6)
    return bytes(result)


def crop_rect(item):
    x = 256 + item["diameter"] * item["spacing"]
    radius = math.ceil(item["diameter"] * max(1, item["radiusPercent"] / 100) / 2) + 2
    return [x - radius, 128 - radius, radius * 2 + 1, radius * 2 + 1]


def raw_crop(raw, rect):
    x, y, width, height = rect
    if not (0 <= x < x + width <= WIDTH and 0 <= y < y + height <= HEIGHT and
            width * height * 8 <= 128 * 128 * 8):
        raise ValueError("Single-mark raw crop exceeds its bounds")
    return b"".join(raw[(row * WIDTH + x) * 8:(row * WIDTH + x + width) * 8]
                    for row in range(y, y + height))


def validate(report, fixtures, originals):
    def sha(value):
        return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)
    if (report.get("schema"), report.get("status"), report.get("qualification")) != (1, "complete", QUALIFICATION):
        raise ValueError("Incomplete or misclassified single-mark evidence")
    if report.get("runtimeVersion", "").split()[:1] != ["5.3.4.1"]:
        raise ValueError("Unpinned single-mark runtime")
    records = report.get("records", [])
    planned = plan(fixtures)
    if len(records) != len(planned):
        raise ValueError("Incomplete single-mark matrix")
    observed = {case["preset"]: case for case in originals}
    for ordinal, (row, item) in enumerate(zip(records, planned)):
        if (row.get("id") != ordinal or any(row.get(key) != value for key, value in item.items()) or
                (row.get("colorModel"), row.get("depth"), row.get("profile"), row.get("canvas")) !=
                ("RGBA", "U16", PROFILE, [WIDTH, HEIGHT]) or
                row.get("rawInitialRoundTrip") is not True or row.get("repeatEquivalent") is not True):
            raise ValueError("Changed single-mark input or storage identity")
        authored = initial(item)
        if row.get("initialRawSha256") != hashlib.sha256(authored).hexdigest():
            raise ValueError("Single-mark authored buffer changed")
        if (not sha(row.get("effectivePresetSha256")) or item["preset"] not in observed or
                row["effectivePresetSha256"] != observed[item["preset"]]["effectivePresetSha256"]):
            raise ValueError("Single-mark source preset changed")
        if (not sha(row.get("canonicalSha256")) or len(row.get("rawSha256", [])) != 2 or
                not all(sha(value) for value in row["rawSha256"])):
            raise ValueError("Invalid single-mark output digest")
        samples = row.get("samplesRGBA16", [])
        second_x = 256 + item["diameter"] * item["spacing"]
        if (len(samples) != 2 or any(not isinstance(value, list) or len(value) != 6 or
                value[:2] != [256 if dab == 0 else second_x, 128] or
                not all(type(channel) is int and 0 <= channel <= 65535 for channel in value[2:])
                for dab, value in enumerate(samples))):
            raise ValueError("Invalid single-mark raw center samples")
        if samples[0] != sample(authored, 256, 128) or samples[1][5] != 65535:
            raise ValueError("Single-mark initial state or opaque pickup changed")
        if item["uniform"] and samples[1][2:] != [item["backgroundGray"] * 257] * 3 + [65535]:
            raise ValueError("Single-mark uniform control drifted")
        rect = crop_rect(item)
        if row.get("rect") != rect or row.get("encoding") != "zlib-base64-bgra16le-row-major":
            raise ValueError("Single-mark raw crop identity changed")
        expected = rect[2] * rect[3] * 8
        if not isinstance(row.get("data"), str) or len(row["data"]) > 200000:
            raise ValueError("Oversized single-mark compressed crop")
        compressed = base64.b64decode(row["data"], validate=True)
        decoder = zlib.decompressobj()
        raw = decoder.decompress(compressed, expected + 1)
        if len(raw) != expected or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
            raise ValueError("Invalid single-mark raw crop")
        local_x, local_y = second_x - rect[0], 128 - rect[1]
        b, g, r, a = struct.unpack_from("<4H", raw, (local_y * rect[2] + local_x) * 8)
        if [r, g, b, a] != samples[1][2:]:
            raise ValueError("Single-mark crop disagrees with center readback")


def render(app, output, fixtures, presets, original_records, checkpoint):
    from krita import Preset
    from PyQt5.QtCore import QByteArray, QPoint
    if sys.byteorder != "little" or "U16" not in app.colorDepths("RGBA") or PROFILE not in app.profiles("RGBA", "U16"):
        raise RuntimeError("Pinned little-endian U16 single-mark storage unavailable")
    planned = plan(fixtures)
    originals = {row["preset"]: row for row in original_records}
    document = app.createDocument(WIDTH, HEIGHT, "Original raw single-mark sampling", "RGBA", "U16", PROFILE, 72.0)
    view = app.activeWindow().addView(document)
    app.setActiveDocument(document)
    node = document.createNode("Original single-mark pixels", "paintlayer")
    document.rootNode().addChildNode(node, None)
    document.setActiveNode(node)
    yield
    records = []
    try:
        for ordinal, item in enumerate(planned):
            checkpoint("kernel U16 single-mark diagnostic " + str(ordinal))
            app.setActiveDocument(document)
            if ((document.colorModel(), document.colorDepth(), document.colorProfile()) != ("RGBA", "U16", PROFILE) or
                    (node.colorModel(), node.colorDepth(), node.colorProfile()) != ("RGBA", "U16", PROFILE)):
                raise RuntimeError("Single-mark document/node identity changed")
            authored = initial(item)
            outputs, settings = [], []
            for repetition in range(2):
                view.setCurrentBrushPreset(presets[item["preset"]])
                view.setBrushSize(item["diameter"])
                view.setPaintingOpacity(1)
                view.setPaintingFlow(1)
                view.setBrushRotation(0)
                view.setEraserMode(False)
                view.setGlobalAlphaLock(False)
                view.setDisablePressure(False)
                if view.currentBrushPreset().name() != item["preset"]:
                    raise RuntimeError("Single-mark preset selection changed")
                effective = Preset(view.currentBrushPreset()).toXML()
                settings.append(hashlib.sha256(effective.encode()).hexdigest())
                if settings[-1] != originals[item["preset"]]["effectivePresetSha256"]:
                    raise RuntimeError("Single-mark preset differs from original kernel controls")
                if not node.setPixelData(QByteArray(authored), 0, 0, WIDTH, HEIGHT):
                    raise RuntimeError("Single-mark raw initialization failed")
                document.refreshProjection()
                document.waitForDone()
                if bytes(node.pixelData(0, 0, WIDTH, HEIGHT)) != authored:
                    raise RuntimeError("Single-mark raw initial roundtrip differs")
                endpoint = 256 + item["diameter"] * item["spacing"] + 1
                node.paintLine(QPoint(256, 128), QPoint(endpoint, 128), 1, 1)
                document.waitForDone()
                document.refreshProjection()
                document.waitForDone()
                raw = bytes(node.pixelData(0, 0, WIDTH, HEIGHT))
                if len(raw) != WIDTH * HEIGHT * 8:
                    raise RuntimeError("Incomplete single-mark raw output")
                outputs.append(raw)
                yield
            canonical_outputs = [canonical(raw) for raw in outputs]
            if canonical_outputs[0] != canonical_outputs[1] or settings[0] != settings[1]:
                raise RuntimeError("Single-mark raw output or settings repeat changed")
            rect = crop_rect(item)
            raw = outputs[0]
            records.append({"id": ordinal, **item, "colorModel": "RGBA", "depth": "U16",
                            "profile": PROFILE, "canvas": [WIDTH, HEIGHT],
                            "rawInitialRoundTrip": True, "repeatEquivalent": True,
                            "initialRawSha256": hashlib.sha256(authored).hexdigest(),
                            "effectivePresetSha256": settings[0],
                            "canonicalSha256": hashlib.sha256(canonical_outputs[0]).hexdigest(),
                            "rawSha256": [hashlib.sha256(value).hexdigest() for value in outputs],
                            "samplesRGBA16": [sample(raw, 256, 128), sample(raw, endpoint - 1, 128)],
                            "rect": rect, "encoding": "zlib-base64-bgra16le-row-major",
                            "data": base64.b64encode(zlib.compress(raw_crop(raw, rect))).decode("ascii")})
            # Preserve completed raw observations if a later source operation fails.
            partial = {"schema": 1, "status": "incomplete", "qualification": QUALIFICATION,
                       "runtimeVersion": app.version(), "records": records}
            (output / "kernel-single-mark.json").write_text(json.dumps(partial, indent=2))
        report = {"schema": 1, "status": "complete", "qualification": QUALIFICATION,
                  "runtimeVersion": app.version(), "records": records}
        validate(report, fixtures, original_records)
        (output / "kernel-single-mark.json").write_text(json.dumps(report, indent=2))
        for row in records:
            print("::notice::" + json.dumps({"capture": "kernel-single-mark", **{
                key: value for key, value in row.items() if key not in ("data", "rawSha256")}}, separators=(",", ":")))
    finally:
        # Keep the bounded document alive for the normal native window shutdown,
        # matching the existing precision sidecar's verified lifetime policy.
        document.setModified(False)
