"""Original tablet sensor acquisition; no source normalization equations assumed."""
import json
from xml.etree.ElementTree import Element, SubElement, tostring


def trace(kind, tilt=(0, 0), rotation=0, pressure=1):
    points = []
    for i in range(33):
        t = i / 32
        if kind == "north":
            x, y = 1024, 1152 - i * 8
        elif kind == "west":
            x, y = 1152 - i * 8, 1024
        elif kind == "south":
            x, y = 1024, 896 + i * 8
        elif kind == "corner":
            x, y = (896 + i * 8, 1024) if i <= 16 else (1024, 1024 - (i - 16) * 8)
        elif kind == "reverse":
            x, y = (896 + i * 8, 1024) if i <= 16 else (1024 - (i - 16) * 8, 1024)
        else:
            x, y = 896 + i * 8, 1024
        tx, ty = tilt(t) if callable(tilt) else tilt
        r = rotation(t) if callable(rotation) else rotation
        p = pressure(t) if callable(pressure) else pressure
        points.append({"x": x, "y": y, "pressure": p, "tiltX": round(tx), "tiltY": round(ty),
                       "rotation": r, "timeUs": round(i * 1000000 / 240)})
    return points


def prepare(root, resources, preset_writer, group):
    plans = []
    if group == "tabletsensors":
        for value in (0, 45, 90, 135, 180, 270, 359):
            plans.append(("Rotation", "rotation", "barrel " + str(value), trace("east", rotation=value)))
        plans.append(("Rotation", "rotation", "barrel ramp", trace("east", rotation=lambda t: 180 * t)))
        for sensor, axis in (("xtilt", 0), ("ytilt", 1)):
            for value in (-60, -30, 0, 30, 60):
                plans.append(("Rotation", sensor, sensor + " " + str(value),
                              trace("east", tilt=(value, 0) if axis == 0 else (0, value))))
            plans.append(("Rotation", sensor, sensor + " ramp", trace("east", tilt=(
                (lambda t: (120 * t - 60, 0)) if axis == 0 else (lambda t: (0, 120 * t - 60))))))
        for tx, ty in ((0, 0), (60, 0), (0, 60), (-60, 0), (0, -60), (45, 45), (-45, 45)):
            plans.append(("Rotation", "ascension", "direction " + str(tx) + " " + str(ty), trace("east", tilt=(tx, ty))))
        for tx, ty in ((0, 0), (30, 0), (60, 0), (45, 45)):
            plans.append(("Ratio", "declination", "elevation " + str(tx) + " " + str(ty), trace("east", tilt=(tx, ty))))
        plans.append(("Ratio", "declination", "elevation ramp", trace("east", tilt=lambda t: (60 * t, 0))))
        for direction in ("east", "north", "west", "south", "corner", "reverse"):
            plans.append(("Rotation", "drawingangle", direction, trace(direction)))
    elif group == "spacingdynamics":
        for label, pressure in (("quarter", .25), ("half", .5), ("full", 1), ("ramp", lambda t: .25 + .75 * t)):
            plans.append(("Spacing", "pressure", label, trace("east", pressure=pressure)))
    else:
        raise ValueError("Unknown sensor acquisition group")
    names, cases = [], []
    for target, sensor, label, events in plans:
        for enabled in (False, True):
            for isotropic in ((False, True) if target == "Spacing" else (False,)):
                name = "GZ Original dry sensor " + group + " " + str(len(cases)).zfill(3)
                sensor_xml = Element("params", id=sensor)
                if sensor == "drawingangle":
                    sensor_xml.attrib.update(lockedAngleMode="0", angleOffset="0", fanCornersEnabled="0", fanCornersStep="30")
                SubElement(sensor_xml, "curve").text = "0,0;1,1;"
                overrides = {"Pressure" + target: str(enabled).lower(), target + "UseCurve": "true",
                             target + "UseSameCurve": "true", target + "commonCurve": "0,0;1,1;",
                             target + "curveMode": "0", target + "Value": "1",
                             target + "Sensor": tostring(sensor_xml, encoding="unicode"),
                             "Spacing/Isotropic": str(isotropic).lower()}
                data = preset_writer(name, "dry", False, overrides, 24, 1, 0, 1, "rect", .3, 0)
                (resources / "paintoppresets" / (name + ".kpp")).write_bytes(data)
                names.append(name)
                cases.append({"preset": name, "variant": "baseline", "diameter": 24, "spacing": 1,
                              "pressure": [events[0]["pressure"], events[-1]["pressure"]], "opacity": 1, "flow": 1,
                              "start": [events[0]["x"], events[0]["y"]], "end": [events[-1]["x"], events[-1]["y"]],
                              "background": "transparent", "overrides": overrides, "expectUnchanged": False,
                              "delivery": "Qt tablet events", "canvasCenter": [1024, 1024], "events": events,
                              "sensorProbe": {"target": target, "sensor": sensor, "enabled": enabled, "label": label,
                                              "isotropic": isotropic, "rect": [864, 864, 320, 320],
                                              "serializationEvidence": "provisional barrel sensor identity; enabled/control output must establish recognition"
                                              if sensor == "rotation" else "observed official bundled KPP sensor/option metadata",
                                              "qualification": "source acquisition only; normalization, sign and event-to-dab semantics unqualified"}})
    (root / "output" / "fixtures.json").write_text(json.dumps({
        "version": "5.3.4.1", "group": group, "presets": names, "cases": cases,
        "license": "CC0-1.0", "width": 2048, "height": 2048,
        "input": "Qt5 synthetic tablet events with authored/delivered field validation; no physical tablet claim"}, indent=2))
