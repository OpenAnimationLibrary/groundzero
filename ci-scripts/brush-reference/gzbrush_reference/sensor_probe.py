"""Validate actual source options/events and capture sensor raster evidence."""
import base64
import hashlib
import math
import zlib
from xml.etree.ElementTree import fromstring


def capture(case, effective_fields, delivery, raw, width):
    spec = case["sensorProbe"]
    target = spec["target"]
    targets = spec.get("targets", [target])
    if (not isinstance(targets, list) or not targets or len(set(targets)) != len(targets)
            or target not in targets or (len(targets) > 1 and targets != ["h", "s", "v"])):
        raise RuntimeError("Invalid source sensor target set")
    actual = {}
    for current in targets:
        keys = ("Pressure" + current, current + "UseCurve", current + "UseSameCurve",
                current + "commonCurve", current + "curveMode", current + "Value", "Spacing/Isotropic")
        for key in keys:
            actual[key] = effective_fields.get(key)
            if actual[key] != case["overrides"][key]:
                raise RuntimeError("Effective sensor option differs: " + key)
        serialized = effective_fields.get(current + "Sensor", "")
        sensor = fromstring(serialized)
        requested = fromstring(case["overrides"][current + "Sensor"])
        if sensor.tag != "params" or sensor.get("id") != spec["sensor"]:
            raise RuntimeError("Source did not retain requested sensor identity")
        if sensor.attrib != requested.attrib:
            raise RuntimeError("Effective sensor attributes differ")
        if spec.get("verifyCurve") and sensor.findtext("curve") != requested.findtext("curve"):
            raise RuntimeError("Effective sensor curve differs")
        actual[current + "Sensor"] = serialized
    if spec.get("captureColorNotices"):
        for channel in ("h", "s", "v"):
            key = "Pressure" + channel
            if effective_fields.get(key) != case["overrides"][key]:
                raise RuntimeError("Unexpected active HSV channel: " + key)
            actual[key] = effective_fields[key]
    node_delivery = case.get("delivery") == "Node.paintLine"
    if node_delivery:
        expected_events = [
            {"type": "line-start", "position": case["start"], "pressure": case["pressure"][0]},
            {"type": "line-end", "position": case["end"], "pressure": case["pressure"][1]},
        ]
        if delivery.get("mode") != "Node.paintLine" or delivery.get("events") != expected_events:
            raise RuntimeError("Source Node endpoint/pressure call differs")
        delivered = delivery["events"]
    else:
        from PyQt5.QtCore import QEvent
        delivered = [event for event in delivery.get("events", [])
                     if event["type"] in (int(QEvent.TabletPress), int(QEvent.TabletMove)) and event["pressure"] > 0]
        if len(delivered) != len(case["events"]):
            raise RuntimeError("Source sensor contact-event count differs")
        for authored, event in zip(case["events"], delivered):
            observed = (*event["image"], event["pressure"], *event["tilt"], event["rotation"])
            expected = (authored["x"], authored["y"], authored["pressure"], authored["tiltX"], authored["tiltY"], authored["rotation"])
            if any(not math.isfinite(a) or abs(a - b) > 1e-7 for a, b in zip(observed, expected)):
                raise RuntimeError("Qt tablet event changed an authored sensor value")
            delta = event["timeMs"] - delivered[0]["timeMs"]
            if delta != round(authored["timeUs"] / 1000) - round(case["events"][0]["timeUs"] / 1000):
                raise RuntimeError("Delivered sensor timestamps differ from authored relative clock")
    known_noop = (target == "Ratio" and spec["sensor"] == "declination" and spec["enabled"] and
                  case["diameter"] == 24 and case["spacing"] == 1 and
                  all(e["tilt"] == [60, 0] and e["pressure"] == 1 for e in delivered))
    if bool(case.get("expectUnchanged")) != known_noop:
        raise RuntimeError("Sensor no-op declaration does not match captured endpoint controls")
    if known_noop:
        brush = fromstring(effective_fields["brush_definition"])
        mask = brush.find("MaskGenerator")
        if (mask is None or mask.get("type") != "rect" or
                float(mask.get("diameter", "nan")) != 24 or float(mask.get("ratio", "nan")) != .3 or
                mask.get("antialiasEdges") != "0" or mask.get("hfade") != "1" or mask.get("vfade") != "1"):
            raise RuntimeError("Known sensor no-op geometry changed")
    left, top, w, h = spec["rect"]
    pixels = b"".join(raw[(y * width + left) * 4:(y * width + left + w) * 4]
                      for y in range(top, top + h))
    return {"effectiveControls": actual, "knownZeroRatioEndpoint": known_noop,
            "captureColorNotices": spec.get("captureColorNotices", False),
            "effectiveBrushDefinition": effective_fields["brush_definition"], "effectiveSensorAttributes": dict(sensor.attrib),
            "authoredContactEvents": case.get("events", []),
            "deliveryInterface": delivery["mode"], "deliveredContactEvents": delivered,
            "deliveryValuesMatchAuthored": True, "rect": spec["rect"],
            "encoding": "zlib-base64-bgra8-row-major",
            "data": base64.b64encode(zlib.compress(pixels)).decode("ascii"),
            "sha256Pixels": hashlib.sha256(pixels).hexdigest(),
            "nonzeroAlphaPixels": sum(alpha != 0 for alpha in pixels[3::4]),
            "interpretation": "Source acquisition; no assumed normalization, angular sign, initial direction, or event-to-dab association"}


def emit_notices(name, evidence):
    """Bounded diagnostics survive failed cases and unavailable artifact access."""
    import json
    def emit(kind, payload):
        message = json.dumps({"preset": name, "capture": kind, **payload}, separators=(",", ":"))
        if len(message) > 3900:
            raise RuntimeError("Sensor diagnostic notice exceeds bounded transport")
        print("::notice title=Sensor source capture::" + message.replace("%", "%25").replace("\n", "%0A").replace("\r", "%0D"), flush=True)
    emit("controls", {key: evidence[key] for key in (
        "effectiveControls", "effectiveSensorAttributes", "effectiveBrushDefinition",
        "deliveryValuesMatchAuthored", "nonzeroAlphaPixels", "sha256Pixels", "knownZeroRatioEndpoint")})
    events = evidence["deliveredContactEvents"]
    for offset in range(0, len(events), 4):
        emit("delivered-events", {"offset": offset, "total": len(events), "events": events[offset:offset + 4]})
    # Retain exact diagnostic alpha in bounded chunks, even if output is empty.
    pixels = zlib.decompress(base64.b64decode(evidence["data"]))
    alpha = base64.b64encode(zlib.compress(pixels[3::4])).decode("ascii")
    for offset in range(0, len(alpha), 3000):
        emit("alpha-chunk", {"rect": evidence["rect"], "encoding": "zlib-base64-alpha8-row-major",
                             "offset": offset, "total": len(alpha), "data": alpha[offset:offset + 3000]})

    if evidence.get("captureColorNotices"):
        encoded = evidence["data"]
        for offset in range(0, len(encoded), 3000):
            emit("color-chunk", {"rect": evidence["rect"], "encoding": evidence["encoding"],
                                 "offset": offset, "total": len(encoded), "data": encoded[offset:offset + 3000]})
