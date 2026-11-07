"""Bounded source readback for original spacing/scatter acquisitions.

No source equation, RNG law or native-profile admission is inferred here.
"""
import base64
import hashlib
import json
import math
import zlib
from xml.etree.ElementTree import ParseError, fromstring

WIDTH = HEIGHT = 2048



def validate_manifest(fixtures):
    group = fixtures.get("group", "")
    groups = {"scatter-" + axis + "-" + direction
              for axis in ("x", "y", "xy") for direction in ("east", "north")}
    if group != "autospacing" and group not in groups:
        raise RuntimeError("Unexpected spacing/scatter acquisition group")
    cases, names = fixtures.get("cases"), fixtures.get("presets")
    expected = 192 if group == "autospacing" else 102
    if (fixtures.get("version") != "5.3.4.1" or fixtures.get("width") != WIDTH or
            fixtures.get("height") != HEIGHT or type(cases) is not list or
            type(names) is not list or len(cases) != expected or len(names) != expected or
            any(type(name) is not str or not name for name in names) or
            len(set(names)) != expected or [c.get("preset") for c in cases] != names or
            fixtures.get("randomizeExecution") is not (group in groups)):
        raise RuntimeError("Incomplete spacing/scatter acquisition manifest")
    if group in groups:
        enabled = [c for c in cases if c.get("scatterProbe", {}).get("enabled") is True]
        disabled = [c for c in cases if c.get("scatterProbe", {}).get("enabled") is False]
        if (len(enabled) != 100 or len(disabled) != 2 or
                {c["scatterProbe"].get("replicate") for c in enabled} != set(range(100)) or
                {c["scatterProbe"].get("replicate") for c in disabled} != {0, 1} or
                any(c["scatterProbe"].get("axes") != group.split("-")[1] or
                    c["scatterProbe"].get("direction") != group.split("-")[2] for c in cases)):
            raise RuntimeError("Incomplete independent scatter sample/control matrix")
    return True


def number(value):
    result = float(value)
    if not math.isfinite(result):
        raise RuntimeError("Nonfinite effective spacing/scatter scalar")
    return result


def validate_controls(case, fields, delivery):
    auto, scatter = case.get("autoSpacingProbe"), case.get("scatterProbe")
    if bool(auto) == bool(scatter):
        raise RuntimeError("Expected one spacing/scatter acquisition contract")
    expected_events = [
        {"type": "line-start", "position": case["start"], "pressure": 1},
        {"type": "line-end", "position": case["end"], "pressure": 1},
    ]
    if (case.get("sourceEngine") != "paintbrush" or case.get("background") != "transparent" or
            case.get("delivery") != "Node.paintLine" or case.get("pressure") != [1, 1] or
            delivery.get("mode") != "Node.paintLine" or delivery.get("events") != expected_events):
        raise RuntimeError("Spacing/scatter delivered source contract differs")
    required = {key: value for key, value in case["overrides"].items() if key != "brush_definition"}
    required.update(paintop="paintbrush", CompositeOp="normal")
    actual = {}
    for key, value in required.items():
        if fields.get(key) != value:
            raise RuntimeError("Effective spacing/scatter option differs: " + key)
        actual[key] = fields[key]
    try:
        requested = fromstring(case["overrides"]["brush_definition"])
        effective = fromstring(fields.get("brush_definition", ""))
    except (ParseError, TypeError) as error:
        raise RuntimeError("Malformed spacing/scatter brush definition") from error
    mask, requested_mask = effective.find("MaskGenerator"), requested.find("MaskGenerator")
    if (effective.tag != "Brush" or mask is None or requested_mask is None or
            len(effective) != 1 or len(mask) or mask.tag != "MaskGenerator"):
        raise RuntimeError("Effective spacing/scatter tip structure differs")
    for key in ("type", "BrushVersion", "useAutoSpacing"):
        if effective.get(key) != requested.get(key):
            raise RuntimeError("Effective spacing/scatter tip option differs: " + key)
    for key in ("spacing", "angle", "randomness", "density", "autoSpacingCoeff"):
        if number(effective.get(key, "nan")) != number(requested.get(key, "nan")):
            raise RuntimeError("Effective spacing/scatter tip scalar differs: " + key)
    for key in ("id", "type", "antialiasEdges", "spikes"):
        if mask.get(key) != requested_mask.get(key):
            raise RuntimeError("Effective spacing/scatter mask option differs: " + key)
    for key in ("diameter", "ratio", "hfade", "vfade"):
        if number(mask.get(key, "nan")) != number(requested_mask.get(key, "nan")):
            raise RuntimeError("Effective spacing/scatter mask scalar differs: " + key)
    spec = auto or scatter
    rect = spec.get("rect")
    expected_rect = ([704, 704, 640, 640] if auto else
                     [128, 896, 1280, 256] if scatter.get("direction") == "east" else
                     [896, 128, 256, 1280])
    if type(rect) is not list or rect != expected_rect or any(type(v) is not int for v in rect):
        raise RuntimeError("Spacing/scatter capture geometry changed")
    if auto:
        if (type(auto.get("enabled")) is not bool or
                effective.get("useAutoSpacing") != str(int(auto["enabled"])) or
                number(effective.get("autoSpacingCoeff")) != auto.get("coefficient") or
                number(mask.get("ratio")) != auto.get("ratio")):
            raise RuntimeError("Automatic spacing control readback differs")
    else:
        axes, direction = scatter.get("axes"), scatter.get("direction")
        if (axes not in ("x", "y", "xy") or direction not in ("east", "north") or
                scatter.get("amount") != 1 or type(scatter.get("enabled")) is not bool or
                scatter.get("replicates") != (100 if scatter["enabled"] else 2) or
                type(scatter.get("replicate")) is not int or
                not 0 <= scatter["replicate"] < scatter["replicates"] or
                fields["PressureScatter"] != str(scatter["enabled"]).lower() or
                fields["Scattering/AxisX"] != str("x" in axes).lower() or
                fields["Scattering/AxisY"] != str("y" in axes).lower() or
                number(fields["ScatterValue"]) != 1 or
                scatter.get("nominalSlots") != 32 or scatter.get("bands") != 3):
            raise RuntimeError("Scatter control/replicate contract differs")
        actual["ScatterSensor"] = fields["ScatterSensor"]
    return {"effectiveControls": actual, "effectiveBrushDefinition": fields["brush_definition"],
            "deliveredContactEvents": delivery["events"], "deliveryInterface": delivery["mode"],
            "sourceProfileVerified": False, "sourceRngLawIdentified": False,
            "pixelParityClaimed": False, "rect": rect}


def capture(case, fields, delivery, raw, width):
    evidence = validate_controls(case, fields, delivery)
    if type(width) is not int or width != WIDTH or not isinstance(raw, (bytes, bytearray)) or len(raw) != WIDTH * HEIGHT * 4:
        raise RuntimeError("Invalid bounded spacing/scatter source raster")
    left, top, width, height = evidence["rect"]
    # Verify no changed coverage escapes the retained region. A future measured
    # distribution outside it fails acquisition instead of being silently clipped.
    for y in range(HEIGHT):
        row = raw[y * WIDTH * 4:(y + 1) * WIDTH * 4]
        if y < top or y >= top + height:
            outside = row[3::4]
        else:
            outside = row[3:left * 4:4] + row[(left + width) * 4 + 3::4]
        if any(outside):
            raise RuntimeError("Source coverage escaped spacing/scatter capture region")
    pixels = b"".join(raw[(y * WIDTH + left) * 4:(y * WIDTH + left + width) * 4]
                      for y in range(top, top + height))
    alpha = pixels[3::4]
    if not any(alpha):
        raise RuntimeError("Spacing/scatter source unexpectedly painted no coverage")
    evidence.update(encoding="zlib-base64-bgra8-row-major",
                    data=base64.b64encode(zlib.compress(pixels)).decode("ascii"),
                    sha256Pixels=hashlib.sha256(pixels).hexdigest(),
                    nonzeroAlphaPixels=sum(a != 0 for a in alpha),
                    qualification="original acquisition only; source behavior and candidate fidelity remain unverified")
    return evidence


def emit_notices(name, evidence):
    def emit(kind, payload):
        text = json.dumps({"preset": name, "capture": kind, **payload}, separators=(",", ":"))
        if len(text) > 3900:
            raise RuntimeError("Spacing/scatter diagnostic notice exceeded transport budget")
        print("::notice title=Spacing/scatter source capture::" +
              text.replace("%", "%25").replace("\n", "%0A").replace("\r", "%0D"), flush=True)
    emit("controls", {key: evidence[key] for key in (
        "effectiveControls", "effectiveBrushDefinition", "deliveredContactEvents",
        "sourceRngLawIdentified", "sourceProfileVerified", "sha256Pixels", "nonzeroAlphaPixels")})
    for offset in range(0, len(evidence["data"]), 3000):
        emit("pixels", {"rect": evidence["rect"], "encoding": evidence["encoding"],
                        "offset": offset, "total": len(evidence["data"]),
                        "data": evidence["data"][offset:offset + 3000]})
