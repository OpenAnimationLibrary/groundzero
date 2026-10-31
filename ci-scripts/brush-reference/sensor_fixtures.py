"""Original tablet sensor acquisition; no source normalization equations assumed."""
import json
import hashlib
import zipfile
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
        elif kind == "diagonal":
            x, y = 896 + i * 8, 896 + i * 8
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
    # Append discriminators without changing the original 76/16 case IDs.
    details = {}
    if group == "tabletsensors":
        for sensor, axis in (("xtilt", 0), ("ytilt", 1)):
            for value in (-20, -10, 10, 20):
                label = "offgrid " + sensor + " " + str(value)
                plans.append(("Rotation", sensor, label, trace("east", tilt=(value, 0) if axis == 0 else (0, value))))
                details[label] = {"spacing": 4}
        asymmetric = [("rotation", "asymmetric barrel " + str(v), trace("east", rotation=v))
                      for v in (0, 45, 90, 135, 180, 225, 270, 315)]
        asymmetric += [("ascension", "asymmetric direction " + str(tx) + " " + str(ty), trace("east", tilt=(tx, ty)))
                       for tx, ty in ((0, 0), (60, 0), (0, 60), (-60, 0), (0, -60),
                                      (45, 45), (-45, 45), (-45, -45), (45, -45))]
        asymmetric += [("drawingangle", "asymmetric drawing " + d, trace(d))
                       for d in ("east", "north", "west", "south", "corner", "reverse")]
        for sensor, label, events in asymmetric:
            plans.append(("Rotation", sensor, label, events))
            details[label] = {"spacing": 4, "asymmetric": True}
    else:
        for spacing in (4, 8):
            for direction in ("east", "north", "diagonal"):
                for label, pressure in (("quarter", .25), ("half", .5), ("full", 1), ("ramp", lambda t: .25 + .75 * t)):
                    label = "sparse " + str(spacing) + " " + direction + " " + label
                    plans.append(("Spacing", "pressure", label, trace(direction, pressure=pressure)))
                    details[label] = {"spacing": spacing}
    if group == "tabletsensors":
        for angle in (0, 30, 75, 135, 270, 315):
            label = "qualified ellipse barrel " + str(angle)
            plans.append(("Rotation", "rotation", label, trace("east", rotation=angle)))
            details[label] = {"spacing": 4, "sourceBarrel": True}
        label = "qualified ellipse barrel wrap ramp"
        plans.append(("Rotation", "rotation", label,
                      trace("east", rotation=lambda t: (315 + 90 * t) % 360)))
        details[label] = {"spacing": 4, "sourceBarrel": True}
    if group == "tabletsensors":
        for sensor, axis in (("xtilt", 0), ("ytilt", 1)):
            for value in (-20, -10, 10, 20):
                label = "qualified ellipse " + sensor + " " + str(value)
                events = trace("east", tilt=(value, 0) if axis == 0 else (0, value))
                plans.append(("Rotation", sensor, label, events))
                details[label] = {"spacing": 4, "sourceBarrel": True}
            # Rectangle evidence only identifies orientation modulo180. The
            # original arrow distinguishes otherwise identical sensor models.
            for value in (-20, -10, 0, 10, 20):
                label = "asymmetric " + sensor + " " + str(value)
                plans.append(("Rotation", sensor, label,
                              trace("east", tilt=(value, 0) if axis == 0 else (0, value))))
                details[label] = {"spacing": 4, "asymmetric": True}
    names, cases = [], []
    for target, sensor, label, events in plans:
        spacing = details.get(label, {}).get("spacing", 1)
        asymmetric = details.get(label, {}).get("asymmetric", False)
        source_barrel = details.get(label, {}).get("sourceBarrel", False)
        for enabled in (False, True):
            for isotropic in ((True,) if source_barrel else (False, True) if target == "Spacing" else (False,)):
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
                resource_spec = None
                if asymmetric:
                    from resource_fixtures import gbr
                    filename = "gz-original-sensor-arrow.gbr"
                    # Original notched L; 0/180-degree rotations are distinct.
                    pixels = bytes(255 if (x in (1, 2) and 1 <= y <= 6) or
                                   (y in (5, 6) and 1 <= x <= 6) else 0
                                   for y in range(8) for x in range(8))
                    brush_data = gbr("GZ Original asymmetric sensor tip", pixels, 1)
                    (resources / "brushes").mkdir(parents=True, exist_ok=True)
                    (resources / "brushes" / filename).write_bytes(brush_data)
                    brush = Element("Brush", type="gbr_brush", BrushVersion="2", filename=filename,
                                    scale="3", spacing=str(spacing), angle="0", useAutoSpacing="0",
                                    autoSpacingCoeff="1", ColorAsMask="1", preserveLightness="0")
                    overrides["brush_definition"] = tostring(brush, encoding="unicode")
                    resource_spec = {"filename": filename, "kind": "asymmetric-sensor", "type": "gbr_brush",
                                     "sha256": hashlib.sha256(brush_data).hexdigest(),
                                     "md5": hashlib.md5(brush_data).hexdigest(), "bytes": len(brush_data),
                                     "originalDimensions": [8, 8], "rect": [864, 864, 320, 320],
                                     "qualification": "original asymmetric tip, source sensor evidence only"}
                data = preset_writer(name, "dry", False, overrides, 24, spacing, 0, 1,
                                     "circle" if source_barrel else "rect", .5 if source_barrel else .3, 0)
                (resources / "paintoppresets" / (name + ".kpp")).write_bytes(data)
                names.append(name)
                # K5341 bd6e8d0 acquisition: enabled identity declination at
                # constant Qt tilt(60,0) produced an empty ratio mask, after
                # effective-control and delivered-event validation completed.
                known_noop = (target == "Ratio" and sensor == "declination" and enabled and
                              all(e["tiltX"] == 60 and e["tiltY"] == 0 for e in events))
                cases.append({"preset": name, "variant": "baseline", "diameter": 24, "spacing": spacing,
                              "pressure": [events[0]["pressure"], events[-1]["pressure"]], "opacity": 1, "flow": 1,
                              "start": [events[0]["x"], events[0]["y"]], "end": [events[-1]["x"], events[-1]["y"]],
                              "background": "transparent", "overrides": overrides, "expectUnchanged": known_noop,
                              "delivery": "Qt tablet events", "canvasCenter": [1024, 1024], "events": events,
                              "sensorProbe": {"target": target, "sensor": sensor, "enabled": enabled, "label": label,
                                              "isotropic": isotropic, "rect": [864, 864, 320, 320],
                                              "serializationEvidence": "provisional barrel sensor identity; enabled/control output must establish recognition"
                                              if sensor == "rotation" else "observed official bundled KPP sensor/option metadata",
                                              "qualification": "source acquisition only; normalization, sign and event-to-dab semantics unqualified"}})
                cases[-1]["sourceBarrelCandidate"] = source_barrel and enabled
                if resource_spec is not None:
                    package_name = "resource-" + str(len(cases) - 1).zfill(3) + ".bundle"
                    packages = root / "output" / "resource-packages"
                    packages.mkdir(parents=True, exist_ok=True)
                    ns = "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0"
                    manifest = Element("{" + ns + "}manifest", {"{" + ns + "}version": "1.2"})
                    SubElement(manifest, "{" + ns + "}file-entry", {
                        "{" + ns + "}full-path": "/", "{" + ns + "}media-type": "application/x-krita-resourcebundle"})
                    with zipfile.ZipFile(packages / package_name, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                        archive.writestr("mimetype", "application/x-krita-resourcebundle")
                        for member, content in (("brushes/" + filename, brush_data), ("paintoppresets/" + name + ".kpp", data)):
                            SubElement(manifest, "{" + ns + "}file-entry", {
                                "{" + ns + "}full-path": member, "{" + ns + "}media-type": member.split("/")[0],
                                "{" + ns + "}md5sum": hashlib.md5(content).hexdigest()})
                            archive.writestr(member, content)
                        archive.writestr("META-INF/manifest.xml", tostring(manifest, encoding="utf-8"))
                    cases[-1]["resourceProbe"] = resource_spec
                    cases[-1]["candidatePackage"] = "output/resource-packages/" + package_name
    (root / "output" / "fixtures.json").write_text(json.dumps({
        "version": "5.3.4.1", "group": group, "presets": names, "cases": cases,
        "license": "CC0-1.0", "width": 2048, "height": 2048,
        "input": "Qt5 synthetic tablet events with authored/delivered field validation; no physical tablet claim"}, indent=2))
