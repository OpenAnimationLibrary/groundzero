"""Original generated-mask transfer holdouts and remaining domain discriminators.

These manifests do not qualify a source profile. Pending geometry/sensor rows
remain obligations, rather than expected-rejection exceptions to the contract.
"""
import json
from xml.etree.ElementTree import Element, SubElement, tostring


GROUPS = ("generated-transfer", "generated-geometry", "generated-sensors")


def _control(target, sensor="pressure", attributes=None):
    curve = "0,0.25;1,1;" if sensor in ("time", "distance") else "0,0;1,1;"
    definition = Element("params", id=sensor, **(attributes or {}))
    SubElement(definition, "curve").text = curve
    return {"Pressure" + target: "true", target + "UseCurve": "true",
            target + "UseSameCurve": "true", target + "Value": "1",
            target + "curveMode": "0", target + "commonCurve": curve,
            target + "Sensor": tostring(definition, encoding="unicode")}


def plans(group):
    if group not in GROUPS:
        raise ValueError("Unknown generated controls acquisition group")
    rows = []
    for generator in ("soft", "gauss"):
        for shape in ("circle", "rect"):
            base = {"generator": generator, "shape": shape, "diameter": 64,
                    "ratio": 1, "angle": 0, "antialias": 1,
                    "spacing": 1, "opacity": 1, "flow": 1, "action": 1,
                    "blend": "normal", "overrides": {}, "pressure": [1, 1]}
            if group == "generated-transfer":
                for diameter in (16, 64):
                    fixed = {**base, "diameter": diameter, "candidateExpected": True}
                    rows.append({**fixed, "objective": "baseline"})
                    for value in (.25, .5, .75):
                        rows.append({**fixed, "objective": "static opacity " + str(value),
                                     "opacity": value})
                    for value in (0, .25, .5):
                        rows.append({**fixed, "objective": "static flow " + str(value),
                                     "flow": value})
                    for action in (0, 1):
                        for blend in ("normal", "multiply", "screen", "overlay"):
                            rows.append({**fixed, "objective": "shared blend " + blend + " action " + str(action),
                                         "action": action, "blend": blend, "opacity": .5, "flow": .25})
                    for spacing in (.25, 5):
                        rows.append({**fixed, "objective": "fixed spacing " + str(spacing),
                                     "spacing": spacing})
                    for target in ("Opacity", "Flow"):
                        for pressure in (.25, .75):
                            for enabled in (False, True):
                                options = _control(target)
                                options["Pressure" + target] = str(enabled).lower()
                                options[target + "Value"] = ".5"
                                rows.append({**fixed, "objective": target + " flag " + str(enabled) + " pressure " + str(pressure),
                                             "pressure": [pressure, pressure], "spacing": .25,
                                             "opacity": .5 if target == "Opacity" else 1,
                                             "flow": .5 if target == "Flow" else 1,
                                             "sensor": "pressure", "target": target,
                                             "enabled": enabled, "overrides": options})
                        options = _control(target)
                        options[target + "Value"] = ".5"
                        rows.append({**fixed, "objective": target + " pressure ramp", "spacing": .25,
                                     "pressure": [.25, .75], "target": target, "sensor": "pressure",
                                     "enabled": True, "overrides": options,
                                     "opacity": .5 if target == "Opacity" else 1,
                                     "flow": .5 if target == "Flow" else 1})
            elif group == "generated-geometry":
                pending = {**base, "candidateExpected": False}
                for diameter in (7, 15.5, 24, 32, 128):
                    rows.append({**pending, "objective": "unmeasured size " + str(diameter), "diameter": diameter})
                for ratio in (.25, .5, .75):
                    rows.append({**pending, "objective": "independent ratio " + str(ratio), "ratio": ratio})
                for angle in (30, 45, 90):
                    rows.append({**pending, "objective": "independent rotation " + str(angle), "angle": angle})
                for diameter in (16, 64):
                    rows.append({**pending, "objective": "AA disabled " + str(diameter),
                                 "diameter": diameter, "antialias": 0})
            else:
                pending = {**base, "candidateExpected": False, "spacing": .25, "enabled": True}
                for target, values in (("Size", (.25, .5, .75)), ("Ratio", (.25, .75))):
                    for value in values:
                        rows.append({**pending, "objective": target + " pressure " + str(value),
                                     "target": target, "sensor": "pressure", "pressure": [value, value],
                                     "overrides": _control(target)})
                    rows.append({**pending, "objective": target + " pressure ramp", "target": target,
                                 "sensor": "pressure", "pressure": [.25, .75], "overrides": _control(target)})
                for rotation in (0, 45, 90, "ramp"):
                    rows.append({**pending, "objective": "barrel " + str(rotation), "target": "Rotation",
                                 "sensor": "rotation", "rotation": rotation, "overrides": _control("Rotation", "rotation")})
                for periodic in (0, 1):
                    rows.append({**pending, "objective": "elapsed Time periodic " + str(periodic),
                                 "target": "Size", "sensor": "time", "durationUs": 1250000,
                                 "overrides": _control("Size", "time", {"duration": "1000", "periodic": str(periodic)})})
                    rows.append({**pending, "objective": "Distance periodic " + str(periodic),
                                 "target": "Size", "sensor": "distance", "distancePixels": 256,
                                 "overrides": _control("Size", "distance", {"length": "64", "periodic": str(periodic)})})
                for horizontal, vertical in ((True, False), (False, True), (True, True)):
                    for pressure in (.49, .51):
                        rows.append({**pending, "objective": "Mirror " + str(horizontal) + " " + str(vertical) + " " + str(pressure),
                                     "target": "Mirror", "sensor": "pressure", "pressure": [pressure, pressure],
                                     "mirror": [horizontal, vertical],
                                     "overrides": {**_control("Mirror"), "HorizontalMirrorEnabled": str(horizontal).lower(),
                                                   "VerticalMirrorEnabled": str(vertical).lower()}})
    return rows


def prepare(root, resources, writer, group):
    cases = []
    for index, row in enumerate(plans(group)):
        name = "GZ Original " + group + " " + str(index).zfill(3)
        brush = Element("Brush", type="auto_brush", BrushVersion="2", spacing=str(row["spacing"]),
                        angle=str(-row["angle"] * 3.141592653589793 / 180), randomness="0", density="1", useAutoSpacing="0")
        mask = {"id": row["generator"], "type": row["shape"], "diameter": str(row["diameter"]),
                "ratio": str(row["ratio"]), "hfade": ".25", "vfade": ".75",
                "antialiasEdges": str(row["antialias"]), "spikes": "2"}
        if row["generator"] == "soft":
            mask["softness_curve"] = "0,0;0.25,0.75;1,1;"
        SubElement(brush, "MaskGenerator", mask)
        overrides = {"brush_definition": tostring(brush, encoding="unicode"), "Spacing/Isotropic": "false",
                     "PaintOpAction": str(row["action"]), "CompositeOp": row["blend"],
                     "OpacityValue": str(row["opacity"]), "FlowValue": str(row["flow"]), **row["overrides"]}
        (resources / "paintoppresets" / (name + ".kpp")).write_bytes(
            writer(name, "dry", False, overrides, row["diameter"], row["spacing"], row["antialias"]))
        length = row.get("distancePixels", row["diameter"] * row["spacing"] * 2 + 1) if group == "generated-transfer" or "target" in row else 1
        start, end = [1024, 1024], [1024 + length, 1024]
        case = {"preset": name, "sourceEngine": "paintbrush", "variant": "baseline",
                "diameter": row["diameter"], "spacing": row["spacing"], "pressure": row["pressure"],
                "opacity": row["opacity"], "flow": row["flow"], "foregroundRGBA8": [173, 91, 47, 255],
                "start": start, "end": end, "background": "transparent", "overrides": overrides,
                "delivery": "Node.paintLine", "canvasCenter": [1024, 1024],
                # Measured K5341 build-up ignores FlowValue, including zero.
                # Only an explicitly authored static Wash zero-flow row is a no-op.
                "expectUnchanged": group == "generated-transfer" and row["action"] == 0 and row["flow"] == 0,
                "generatedTipProbe": mask,
                "generatedControlProbe": {"objective": row["objective"], "candidateExpected": row["candidateExpected"],
                                           "sourceParityQualified": False,
                                           "scope": "shared Pixel transfer holdout" if row["candidateExpected"] else "open generated domain obligation"}}
        if "target" in row:
            duration = row.get("durationUs", 133333)
            events = []
            for ordinal in range(33):
                fraction = ordinal / 32
                rotation = row.get("rotation", 0)
                events.append({"x": start[0] + length * fraction, "y": start[1],
                               "pressure": row["pressure"][0] + fraction * (row["pressure"][1] - row["pressure"][0]),
                               "tiltX": 0, "tiltY": 0, "rotation": 180 * fraction if rotation == "ramp" else rotation,
                               "timeUs": round(duration * fraction)})
            case.update(delivery="Qt tablet events", events=events,
                        sensorProbe={"target": row["target"], "sensor": row["sensor"], "enabled": row["enabled"],
                                     "rect": [976, 976, 384, 96], "isotropic": False, "verifyCurve": True,
                                     "verifyForeground": True, "qualification": "authored/delivered controls only; source parity unqualified"})
            if row["sensor"] == "time":
                # Paced events carry measured device/monotonic times. The
                # synthetic sensor clock guard is intentionally a different
                # contract; timed_probe records the actual elapsed evidence.
                case.pop("sensorProbe")
                case["timedProbe"] = {"durationUs": duration, "moving": True, "target": "time",
                                      "rect": [976, 976, 384, 96], "maxLatenessUs": 20000,
                                      "qualification": "paced elapsed-Time acquisition; no physical tablet claim"}
            if "mirror" in row:
                case["mirrorProbe"] = {"horizontal": row["mirror"][0], "vertical": row["mirror"][1]}
        cases.append(case)
    (root / "output/fixtures.json").write_text(json.dumps({
        "version": "5.3.4.1", "group": group, "presets": [case["preset"] for case in cases],
        "cases": cases, "license": "CC0-1.0", "width": 2048, "height": 2048,
        "input": "Original heldout Node lines and exact Qt tablet traces; pending rows do not qualify or excuse missing behavior"}, indent=2))
