"""Validate actual source options/events and capture sensor raster evidence."""
import base64
import hashlib
import math
import zlib
from xml.etree.ElementTree import fromstring
from PyQt5.QtCore import QEvent


def capture(case, effective_fields, delivery, raw, width):
    spec = case["sensorProbe"]
    target = spec["target"]
    keys = ("Pressure" + target, target + "UseCurve", target + "UseSameCurve",
            target + "commonCurve", target + "curveMode", target + "Value", "Spacing/Isotropic")
    actual = {key: effective_fields.get(key) for key in keys}
    for key in keys:
        if actual[key] != case["overrides"][key]:
            raise RuntimeError("Effective sensor option differs: " + key)
    serialized = effective_fields.get(target + "Sensor", "")
    sensor = fromstring(serialized)
    requested = fromstring(case["overrides"][target + "Sensor"])
    if sensor.tag != "params" or sensor.get("id") != spec["sensor"]:
        raise RuntimeError("Source did not retain requested sensor identity")
    for key, value in requested.attrib.items():
        if sensor.get(key) != value:
            raise RuntimeError("Effective sensor attribute differs: " + key)
    actual[target + "Sensor"] = serialized
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
    left, top, w, h = spec["rect"]
    pixels = b"".join(raw[(y * width + left) * 4:(y * width + left + w) * 4]
                      for y in range(top, top + h))
    return {"effectiveControls": actual, "effectiveSensorAttributes": dict(sensor.attrib),
            "authoredContactEvents": case["events"], "deliveredContactEvents": delivered,
            "deliveryValuesMatchAuthored": True, "rect": spec["rect"],
            "encoding": "zlib-base64-bgra8-row-major",
            "data": base64.b64encode(zlib.compress(pixels)).decode("ascii"),
            "sha256Pixels": hashlib.sha256(pixels).hexdigest(),
            "nonzeroAlphaPixels": sum(alpha != 0 for alpha in pixels[3::4]),
            "interpretation": "Source acquisition; no assumed normalization, angular sign, initial direction, or event-to-dab association"}
