"""Read-only generated-mask model research against original source BGRA notices.

The equations were derived from original source controls and captured pixels.
This file does not render native production strokes, change fidelity gates, or
admit a source profile. Gaussian circle's 0.27 coefficient is an independently
estimated global parameter, not a claim about a foreign implementation constant.
"""
import argparse
import base64
import bisect
import hashlib
import json
import math
import struct
from pathlib import Path
import zlib

RECT = [976, 976, 96, 96]


def curve(text):
    points = [tuple(map(float, item.split(","))) for item in text.strip(";").split(";")]
    if (not 2 <= len(points) <= 1024 or any(len(p) != 2 for p in points) or
            any(not math.isfinite(v) or not 0 <= v <= 1 for p in points for v in p) or
            points[0][0] != 0 or points[-1][0] != 1 or
            any(b[0] - a[0] < 1e-9 for a, b in zip(points, points[1:]))):
        raise ValueError("Invalid original normalized softness curve")
    xs, ys = [p[0] for p in points], [p[1] for p in points]
    second = [0.0] * len(points)
    diagonal, rhs, upper = [], [], []
    for i in range(1, len(points) - 1):
        left, right = xs[i] - xs[i - 1], xs[i + 1] - xs[i]
        d = 2 * (left + right)
        v = 6 * ((ys[i + 1] - ys[i]) / right - (ys[i] - ys[i - 1]) / left)
        if diagonal:
            d -= left * upper[-1] / diagonal[-1]
            v -= left * rhs[-1] / diagonal[-1]
        diagonal.append(d)
        rhs.append(v)
        upper.append(right)
    for i in range(len(diagonal) - 1, -1, -1):
        second[i + 1] = (rhs[i] - upper[i] * second[i + 2]) / diagonal[i]

    def evaluate(value):
        value = min(1, max(0, value))
        i = max(0, min(len(xs) - 2, bisect.bisect_right(xs, value) - 1))
        span = xs[i + 1] - xs[i]
        a, b = (xs[i + 1] - value) / span, (value - xs[i]) / span
        result = a * ys[i] + b * ys[i + 1]
        result += ((a ** 3 - a) * second[i] + (b ** 3 - b) * second[i + 1]) * span * span / 6
        return max(0, min(1, result))
    return evaluate


def gaussian_box(distance, mean, sigma):
    if sigma == 0:
        return 1.0 if abs(distance) <= mean else 0.0
    scale = sigma * math.sqrt(2)
    return ((math.erf((mean + distance) / scale) + math.erf((mean - distance) / scale)) /
            (2 * math.erf(mean / scale)))


def rectangle_aa(base, x, y, radius, pixel_x, start_x):
    # Original fixed 8-column source dispatch, anchored to the rounded stamp.
    # A boundary lane enables axis AA throughout that block. This is measured
    # output behavior; no foreign renderer implementation was translated.
    left = math.floor(start_x + .5) - int(radius)
    first = left + ((pixel_x - left) // 8) * 8
    if any(abs(i - start_x) > radius - 1 for i in range(first, first + 8)):
        base = math.ceil(base - 1e-10) * min(1, max(0, radius - x)) * min(1, max(0, radius - y))
    return base


def predict(case):
    spec = case["generatedTipProbe"]
    generator, shape = spec["id"], spec["type"]
    diameter, ratio = float(spec["diameter"]), float(spec["ratio"])
    if (generator not in ("soft", "gauss") or shape not in ("circle", "rect") or
            diameter not in (16, 64) or ratio != 1 or str(spec.get("antialiasEdges")) != "1" or
            case.get("background") != "transparent" or case.get("opacity") != 1 or case.get("flow") != 1 or
            case.get("pressure") != [1, 1] or case.get("spacing") != 1):
        raise ValueError("Model report is restricted to the acquired static generated-mask domain")
    radius, sx, sy = diameter / 2, case["start"][0], case["start"][1]
    horizontal, vertical = float(spec["hfade"]), float(spec["vfade"])
    if not all(math.isfinite(v) and 0 <= v <= 1 for v in (horizontal, vertical)):
        raise ValueError("Invalid acquired fade control")
    C = curve(spec["softness_curve"]) if generator == "soft" else None
    result = bytearray()
    for row in range(96):
        for col in range(96):
            pixel_x, pixel_y = RECT[0] + col, RECT[1] + row
            x, y = abs(pixel_x - sx), abs(pixel_y + .5 - sy)
            value = 0
            if shape == "rect":
                if x <= radius and y <= radius:
                    if generator == "soft":
                        u, v = x / radius, y / radius
                        value = 255 * C(u) * (1 - C(1 - u)) * C(v) * (1 - C(1 - v))
                    else:
                        value = 255 * gaussian_box(x, radius * (.5 + .25 * horizontal), radius * (.2 - .1 * horizontal))
                        value *= gaussian_box(y, radius * (.5 + .25 * vertical), radius * (.2 - .1 * vertical))
                    value = rectangle_aa(value, x, y, radius, pixel_x, sx)
            elif generator == "soft":
                squared = (x * x + y * y) / (radius * radius)
                inner = (radius - 1) ** 2 / (radius * radius)
                resolution = 4 * diameter
                if squared < 1:
                    if squared <= inner:
                        value = 255 * C(squared * (resolution - 1) / resolution)
                    else:
                        boundary = math.ceil(255 * C(inner * (resolution - 1) / resolution) - 1e-10)
                        value = boundary * (1 - squared) / (1 - inner)
            else:
                distance = math.hypot(x, y)
                fade = (horizontal + vertical) / 2
                sigma = .27 * radius * (1 - fade)
                mean = radius - 2.5 * sigma
                if distance < radius:
                    if distance <= radius - 1:
                        value = 255 * gaussian_box(distance, mean, sigma)
                    else:
                        boundary = math.ceil(255 * gaussian_box(radius - 1, mean, sigma) - 1e-10)
                        value = boundary * (radius - distance)
            result.append(max(0, min(255, math.floor(value + .5))))
    return bytes(result)


def png_crop(path):
    """Decode an original RGBA8 PNG when final stdout notices were not flushed.

    This is a bounded PNG reader, not a foreign or native brush renderer. The
    complete original PNG stays intact; alpha is never registered or altered.
    """
    data = path.read_bytes()
    if len(data) > 64 * 1024 * 1024 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("Invalid bounded original PNG")
    position, header, compressed, ended = 8, None, [], False
    while position < len(data):
        if len(data) - position < 12:
            raise ValueError("Truncated original PNG chunk")
        size = struct.unpack_from(">I", data, position)[0]
        kind = data[position + 4:position + 8]
        end = position + 12 + size
        if end > len(data):
            raise ValueError("Truncated original PNG payload")
        payload = data[position + 8:position + 8 + size]
        checksum = struct.unpack_from(">I", data, position + 8 + size)[0]
        if zlib.crc32(kind + payload) & 0xffffffff != checksum:
            raise ValueError("Corrupt original PNG chunk")
        if kind == b"IHDR":
            if header is not None or size != 13:
                raise ValueError("Invalid original PNG header")
            header = struct.unpack(">IIBBBBB", payload)
        elif kind == b"IDAT":
            compressed.append(payload)
        elif kind == b"IEND":
            if size or end != len(data):
                raise ValueError("Invalid original PNG end")
            ended = True
        position = end
    if not ended or header != (2048, 2048, 8, 6, 0, 0, 0) or not compressed:
        raise ValueError("Expected complete original 2048-square RGBA8 PNG")
    width, height = 2048, 2048
    length = (width * 4 + 1) * height
    stream = zlib.decompressobj()
    raw = stream.decompress(b"".join(compressed), length + 1)
    if len(raw) != length or not stream.eof or stream.unused_data or stream.unconsumed_tail:
        raise ValueError("Invalid original PNG pixel stream")
    previous, crop = bytes(width * 4), bytearray()
    for y in range(RECT[1] + RECT[3]):
        offset = y * (width * 4 + 1)
        filtering = raw[offset]
        row = bytearray(raw[offset + 1:offset + 1 + width * 4])
        if filtering not in range(5):
            raise ValueError("Invalid original PNG scanline filter")
        if filtering:
            for i in range(len(row)):
                left, up, corner = row[i - 4] if i >= 4 else 0, previous[i], previous[i - 4] if i >= 4 else 0
                if filtering == 1:
                    prediction = left
                elif filtering == 2:
                    prediction = up
                elif filtering == 3:
                    prediction = (left + up) // 2
                else:
                    estimate = left + up - corner
                    dl, du, dc = abs(estimate - left), abs(estimate - up), abs(estimate - corner)
                    prediction = left if dl <= du and dl <= dc else up if du <= dc else corner
                row[i] = (row[i] + prediction) & 255
        if y >= RECT[1]:
            region = row[RECT[0] * 4:(RECT[0] + RECT[2]) * 4]
            for i in range(0, len(region), 4):
                crop.extend((region[i + 2], region[i + 1], region[i], region[i + 3]))
        previous = row
    return bytes(crop)


def captures(runtime, expected_names, allow_missing=False):
    chunks = {}
    for line in runtime.splitlines():
        position = line.find("::notice::")
        if position < 0:
            continue
        item = json.loads(line[position + len("::notice::"):])
        if not isinstance(item, dict) or item.get("capture") != "generated-tip-bgra":
            continue
        name, offset, total, data = item.get("preset"), item.get("offset"), item.get("total"), item.get("data")
        if (name not in expected_names or item.get("rect") != RECT or item.get("encoding") != "zlib-base64-bgra8" or
                type(offset) is not int or type(total) is not int or not 0 <= offset < total <= 100000 or
                not isinstance(data, str) or not 0 < len(data) <= 1800 or offset + len(data) > total):
            raise ValueError("Malformed bounded original generated-mask chunk")
        parts = chunks.setdefault(name, {})
        if offset in parts:
            raise ValueError("Duplicate original generated-mask chunk")
        parts[offset] = total, data
    if not allow_missing and set(chunks) != set(expected_names):
        raise ValueError("Missing original generated-mask captures")
    result = {}
    for name, parts in chunks.items():
        offset, encoded, declared = 0, [], set()
        for start, (total, data) in sorted(parts.items()):
            if start != offset:
                raise ValueError("Noncontiguous original generated-mask chunks")
            offset += len(data)
            encoded.append(data)
            declared.add(total)
        if declared != {offset}:
            if allow_missing and len(declared) == 1 and offset < next(iter(declared)):
                continue  # Unflushed final stdout suffix; retain explicit original-PNG provenance.
            raise ValueError("Original generated-mask total disagrees")
        stream = zlib.decompressobj()
        pixels = stream.decompress(base64.b64decode("".join(encoded), validate=True), 96 * 96 * 4 + 1)
        if len(pixels) != 96 * 96 * 4 or not stream.eof or stream.unused_data or stream.unconsumed_tail:
            raise ValueError("Invalid original generated-mask pixel stream")
        result[name] = pixels
    return result


def analyze(root):
    if (root / "brush-reference-run").is_dir():
        root = root / "brush-reference-run"
    fixture_path, oracle_path, runtime_path = root / "output/fixtures.json", root / "output/oracle-status.json", root / "runtime.log"
    fixture, oracle = json.loads(fixture_path.read_text()), json.loads(oracle_path.read_text())
    cases = fixture.get("cases", [])
    expected = 124 if fixture.get("group") == "generatedcurves" else 32 if fixture.get("group") == "generatedtips" else 0
    if (not expected or len(cases) != expected or oracle.get("status") != "complete" or
            oracle.get("version") != "5.3.4.1" or len(oracle.get("records", [])) != expected):
        raise ValueError("Complete pinned original generated-mask acquisition required")
    source = {r["preset"]: r for r in oracle["records"]}
    names = [c["preset"] for c in cases]
    if len(set(names)) != expected or set(names) != set(source):
        raise ValueError("Original source case identities differ")
    pixels = captures(runtime_path.read_text(errors="strict"), names, allow_missing=True)
    png_fallback = {}
    for name in set(names) - set(pixels):
        image = source[name].get("image")
        if not isinstance(image, str) or Path(image).name != image or not image.endswith(".png"):
            raise ValueError("Missing complete original source PNG")
        image_path = root / "output" / image
        pixels[name] = png_crop(image_path)
        png_fallback[name] = hashlib.sha256(image_path.read_bytes()).hexdigest()
    rows, skipped = [], []
    for case in cases:
        name = case["preset"]
        if case["generatedTipProbe"]["id"] == "default":
            skipped.append(name)
            continue
        effective = source[name].get("generatedTipEvidence", {}).get("effectiveMask", {})
        if not effective or any(str(effective.get(k)) != str(v) for k, v in case["generatedTipProbe"].items()):
            raise ValueError("Original effective generated-mask controls differ")
        raw = pixels[name]
        predicted = predict(case)
        alpha = raw[3::4]
        errors = [abs(a - b) for a, b in zip(alpha, predicted)]
        rows.append({"preset": name, "generator": case["generatedTipProbe"]["id"],
                     "shape": case["generatedTipProbe"]["type"], "maxAlphaErrorCodes": max(errors),
                     "capture": "original-PNG-crop" if name in png_fallback else "original-BGRA-notices",
                     "sourcePngSha256": png_fallback.get(name),
                     "alphaMAECodes": sum(errors) / len(errors), "pixelsOverOneCode": sum(e > 1 for e in errors),
                     "originalBGRASha256": hashlib.sha256(raw).hexdigest(),
                     "modelAlphaSha256": hashlib.sha256(predicted).hexdigest()})
    return {"schema": 1, "sourceProfileVerified": False, "pixelParityClaimed": False, "gate": False,
            "qualification": "Independent original-pixel model research; final full-image source qualification remains separate",
            "gaussianCircleCoefficient": .27, "coefficientIsEstimated": True,
            "inputSha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (fixture_path, oracle_path, runtime_path)},
            "modeledCases": len(rows), "skippedExistingDefaultCases": skipped, "records": rows}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.artifact), indent=2, allow_nan=False))
