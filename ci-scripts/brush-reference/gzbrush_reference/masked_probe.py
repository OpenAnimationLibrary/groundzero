"""Validate embedded masked-brush metadata and retain bounded source evidence."""
import base64
import hashlib
import json
import zlib
from xml.etree.ElementTree import fromstring


def capture(case, fields, delivery, raw, width, initial):
    controls = {}
    for key, wanted in case["overrides"].items():
        actual = fields.get(key)
        if key.endswith("brush_definition"):
            if actual is None:
                raise RuntimeError("Missing effective masking brush definition")
            requested, effective = fromstring(wanted), fromstring(actual)
            for tag in ("Brush", "MaskGenerator"):
                a = effective if tag == "Brush" else effective.find(tag)
                b = requested if tag == "Brush" else requested.find(tag)
                if a is None or b is None:
                    raise RuntimeError("Missing procedural masking tip geometry")
                for attribute, value in b.attrib.items():
                    observed = a.get(attribute)
                    if observed != value:
                        try:
                            same = float(observed) == float(value)
                        except (TypeError, ValueError):
                            same = False
                        if not same:
                            raise RuntimeError("Effective masking geometry differs: " + attribute)
        elif actual != wanted:
            raise RuntimeError("Effective masking option differs: " + key)
        controls[key] = actual
    primary = fromstring(fields["brush_definition"])
    mask = primary.find("MaskGenerator")
    if (primary.get("type") != "auto_brush" or mask is None or mask.get("type") != "circle" or
            float(mask.get("diameter", "nan")) != 24 or
            float(primary.get("spacing", "nan")) != case["spacing"] or mask.get("antialiasEdges") != "0"):
        raise RuntimeError("Effective primary mask control geometry differs")
    events = delivery.get("events", [])
    if (delivery.get("mode") != "Node.paintLine" or len(events) != 2 or
            events[0].get("position") != case["start"] or events[1].get("position") != case["end"] or
            any(event.get("pressure") != 1 for event in events)):
        raise RuntimeError("Masked source line delivery differs")
    left, top, w, h = case["maskProbe"]["rect"]
    pixels = b"".join(raw[(y * width + left) * 4:(y * width + left + w) * 4] for y in range(top, top + h))
    count = sum(alpha != 0 for alpha in pixels[3::4])
    if len(initial) != len(raw):
        raise RuntimeError("Masked source initial raster is incomplete")
    changed = 0
    for index in range(len(raw) // 4):
        offset = index * 4
        alpha = raw[offset + 3]
        if alpha == initial[offset + 3] and (alpha == 0 or raw[offset:offset + 3] == initial[offset:offset + 3]):
            continue
        y, x = divmod(index, width)
        if not (left <= x < left + w and top <= y < top + h):
            raise RuntimeError("Masked source changes extend outside captured region")
        changed += 1
    if case["maskProbe"]["mode"] == "off" and changed == 0:
        raise RuntimeError("Unmasked paired control did not paint")
    return {"effectiveControls": controls, "effectivePrimaryBrush": fields["brush_definition"],
            "nestedMetadataVerified": True, "externalResourceDependencies": [],
            "deliveryVerified": True, "deliveredLine": delivery, "nonzeroAlphaPixels": count,
            "changedPixels": changed, "outsideRegionUnchanged": True,
            "rect": case["maskProbe"]["rect"], "encoding": "zlib-base64-bgra8-row-major",
            "data": base64.b64encode(zlib.compress(pixels)).decode("ascii"),
            "sha256Pixels": hashlib.sha256(pixels).hexdigest(), "sourceParityQualified": False}


def emit_notices(name, evidence):
    def emit(kind, payload):
        encoded = json.dumps({"preset": name, "capture": kind, **payload}, separators=(",", ":"))
        if len(encoded) > 3900:
            raise RuntimeError("Mask diagnostic notice exceeds bound")
        print("::notice title=Masked source capture::" + encoded.replace("%", "%25").replace("\n", "%0A").replace("\r", "%0D"), flush=True)
    # Partition arbitrary-size control metadata rather than assuming one notice fits.
    controls = json.dumps(evidence["effectiveControls"], separators=(",", ":"))
    for offset in range(0, len(controls), 2400):
        emit("controls", {"offset": offset, "total": len(controls), "data": controls[offset:offset + 2400]})
    emit("geometry", {"primary": evidence["effectivePrimaryBrush"], "line": evidence["deliveredLine"],
                      "nonzeroAlphaPixels": evidence["nonzeroAlphaPixels"], "sha256Pixels": evidence["sha256Pixels"]})
    data = evidence["data"]
    for offset in range(0, len(data), 3000):
        emit("color-chunk", {"rect": evidence["rect"], "encoding": evidence["encoding"],
                             "offset": offset, "total": len(data), "data": data[offset:offset + 3000]})
