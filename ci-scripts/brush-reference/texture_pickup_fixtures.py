"""Original discriminators for coverage-aware Color Smudge texture pickup."""
import base64
import hashlib
import json
import struct
import zlib


def png(values):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data)))
    pixels = b"".join(b"\0" + b"".join(bytes([values[y * 16 + x]] * 3 + [255])
                                        for x in range(16)) for y in range(16))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))


def prepare(root, resources, preset_writer):
    patterns = {}
    # Equal asymmetric neighborhoods with only the central byte crossing the
    # proposed threshold. Diagonal/color-background cases distinguish a true
    # 2D neighborhood from a horizontal-band coincidence.
    for center in (127, 128, 129):
        values = [32] * 256
        for x, y, value in ((7, 8, 255), (8, 7, 64), (9, 8, 192), (8, 9, 96),
                            (7, 7, 224), (9, 9, 48), (8, 8, center)):
            values[y * 16 + x] = value
        patterns[f"threshold-{center}"] = values
    values = [0] * 256
    values[9 * 16 + 5] = 255
    patterns["isolated-off-axis"] = values
    for radius in (2, 4):
        patterns[f"empty-hole-{radius}"] = [
            0 if max(abs(x - 8), abs(y - 8)) <= radius else
            32 + ((37 * x + 53 * y) % 224)
            for y in range(16) for x in range(16)]
    patterns["asymmetric-ramp"] = [(17 * x + 29 * y) % 256
                                     for y in range(16) for x in range(16)]
    patterns["sparse-diagonal"] = [255 if x == y else 0
                                      for y in range(16) for x in range(16)]
    (resources / "patterns").mkdir(parents=True, exist_ok=True)
    names, cases = [], []
    for pattern_name, values in patterns.items():
        data = png(values)
        filename = "gz-original-pickup-" + pattern_name + ".png"
        (resources / "patterns" / filename).write_bytes(data)
        for background in ("wet-halves", "blend-stripes"):
            for color in (0, .25):
                for dabs in (2, 3):
                    name = f"GZ Original new Dulling pickup {pattern_name} background {background} color {round(color * 100):03d} dabs {dabs}"
                    overrides = {
                        "Texture/Pattern/Enabled": "true", "Texture/Pattern/Scale": "1",
                        "Texture/Pattern/Brightness": "0", "Texture/Pattern/Contrast": "1",
                        "Texture/Pattern/NeutralPoint": ".5", "Texture/Pattern/OffsetX": "0",
                        "Texture/Pattern/OffsetY": "0", "Texture/Pattern/isRandomOffsetX": "false",
                        "Texture/Pattern/isRandomOffsetY": "false", "Texture/Pattern/CutoffPolicy": "0",
                        "Texture/Pattern/CutoffLeft": "0", "Texture/Pattern/CutoffRight": "255",
                        "Texture/Pattern/Invert": "false", "Texture/Pattern/AutoInvertOnErase": "false",
                        "Texture/Pattern/TexturingMode": "0", "Texture/Pattern/UseSoftTexturing": "false",
                        "Texture/Pattern/PatternFileName": filename,
                        "Texture/Pattern/Name": "gz-original-pickup-" + pattern_name,
                        "Texture/Pattern/PatternMD5Sum": hashlib.md5(data).hexdigest(),
                        "Texture/Pattern/Pattern": base64.b64encode(data).decode("ascii"),
                        "PressureTexture/Strength/": "true", "Texture/Strength/Value": "1",
                        "Texture/Strength/UseCurve": "false", "SmudgeRateValue": ".5",
                        "ColorRateValue": str(color), "SmudgeRadiusValue": "0"}
                    (resources / "paintoppresets" / (name + ".kpp")).write_bytes(
                        preset_writer(name, "dulling", True, overrides, 32, 5, 0))
                    center = 264 + 160 * (dabs - 1)
                    names.append(name)
                    cases.append({"preset": name, "variant": "baseline", "diameter": 32, "spacing": 5,
                        "pressure": [1, 1], "opacity": 1, "flow": 1, "start": [264, 1032],
                        "end": [center + 1, 1032], "background": background, "wetBoundary": 344,
                        "foregroundRGBA8": [0, 255, 0, 255], "overrides": overrides,
                        "expectUnchanged": False, "probePositions": [center + x for x in (-6, -4, -2, 0, 2, 4, 6)],
                        "probeY": 1032, "texturePatternFilename": filename, "texturePatternExpectedGray8": values,
                        "texturePatternSha256": hashlib.sha256(data).hexdigest(),
                        "textureProbeRect": [center - 16, 1016, 32, 32],
                        "sourceCandidateGate": False,
                        "changeExpectation": "distinguish threshold, spatial filtering and empty-hole fallback; no source admission"})
    if len(cases) != 64:
        raise RuntimeError("Unexpected texture pickup discriminator count")
    (root / "output" / "fixtures.json").write_text(json.dumps({
        "version": "5.3.4.1", "group": "texturepickup", "presets": names, "cases": cases,
        "license": "CC0-1.0", "width": 2048, "height": 2048,
        "input": "Node.paintLine, independent original two/three-dab traces"}, indent=2))
