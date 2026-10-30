"""Original asymmetric Mirror acquisition from public serialized KPP fields."""
import hashlib
import json
import zipfile
from xml.etree.ElementTree import Element, SubElement, tostring
from resource_fixtures import gbr
from sensor_fixtures import trace


def prepare(root, resources, preset_writer):
    filename = "gz-original-mirror-tip.gbr"
    pixels = bytes(255 if (x in (1, 2) and 1 <= y <= 6) or
                   (y in (5, 6) and 1 <= x <= 6) else 0 for y in range(8) for x in range(8))
    tip = gbr("GZ Original mirror tip", pixels, 1)
    (resources / "brushes").mkdir(parents=True, exist_ok=True)
    (resources / "brushes" / filename).write_bytes(tip)
    packages = root / "output" / "resource-packages"
    packages.mkdir(parents=True, exist_ok=True)
    names, cases = [], []
    for axes in ((True, False), (False, True), (True, True)):
        for pressure in (.1, .25, .49, .5, .51, .75, 1):
            for enabled in (False, True):
                index = len(cases)
                name = "GZ Original dry mirror " + str(index).zfill(3)
                brush = Element("Brush", type="gbr_brush", BrushVersion="2", filename=filename,
                                scale="3", spacing="4", angle="0", useAutoSpacing="0",
                                autoSpacingCoeff="1", ColorAsMask="1", preserveLightness="0")
                overrides = {"brush_definition": tostring(brush, encoding="unicode"),
                             "PressureMirror": str(enabled).lower(),
                             "HorizontalMirrorEnabled": str(axes[0]).lower(),
                             "VerticalMirrorEnabled": str(axes[1]).lower(),
                             "MirrorUseCurve": "true", "MirrorUseSameCurve": "true",
                             "MirrorValue": "1", "MirrorcurveMode": "0", "MirrorcommonCurve": "0,0;1,1;",
                             "MirrorSensor": '<params id="pressure"><curve>0,0;1,1;</curve></params>',
                             "Spacing/Isotropic": "false"}
                preset = preset_writer(name, "dry", False, overrides, 24, 4, 0)
                (resources / "paintoppresets" / (name + ".kpp")).write_bytes(preset)
                package_name = "resource-" + str(index).zfill(3) + ".bundle"
                ns = "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0"
                manifest = Element("{" + ns + "}manifest")
                SubElement(manifest, "{" + ns + "}file-entry", {
                    "{" + ns + "}full-path": "/", "{" + ns + "}media-type": "application/x-krita-resourcebundle"})
                with zipfile.ZipFile(packages / package_name, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                    archive.writestr("mimetype", "application/x-krita-resourcebundle")
                    for member, content in (("brushes/" + filename, tip), ("paintoppresets/" + name + ".kpp", preset)):
                        SubElement(manifest, "{" + ns + "}file-entry", {
                            "{" + ns + "}full-path": member, "{" + ns + "}media-type": member.split("/")[0],
                            "{" + ns + "}md5sum": hashlib.md5(content).hexdigest()})
                        archive.writestr(member, content)
                    archive.writestr("META-INF/manifest.xml", tostring(manifest, encoding="utf-8"))
                events = trace("east", pressure=pressure)
                names.append(name)
                cases.append({"preset": name, "variant": "baseline", "diameter": 24, "spacing": 4,
                              "pressure": [pressure, pressure], "opacity": 1, "flow": 1,
                              "start": [896, 1024], "end": [1152, 1024], "background": "transparent",
                              "overrides": overrides, "expectUnchanged": False,
                              "delivery": "Qt tablet events", "canvasCenter": [1024, 1024], "events": events,
                              "mirrorProbe": {"horizontal": axes[0], "vertical": axes[1]},
                              "sensorProbe": {"target": "Mirror", "sensor": "pressure", "enabled": enabled,
                                              "rect": [864, 864, 320, 320], "isotropic": False,
                                              "qualification": "source mirror threshold acquisition, no source parity"},
                              "resourceProbe": {"filename": filename, "kind": "asymmetric-mirror", "type": "gbr_brush",
                                                "sha256": hashlib.sha256(tip).hexdigest(), "md5": hashlib.md5(tip).hexdigest(),
                                                "bytes": len(tip), "originalDimensions": [8, 8], "rect": [864, 864, 320, 320]},
                              "candidatePackage": "output/resource-packages/" + package_name})
    (root / "output" / "fixtures.json").write_text(json.dumps({
        "version": "5.3.4.1", "group": "mirrortips", "presets": names, "cases": cases,
        "license": "CC0-1.0", "width": 2048, "height": 2048,
        "input": "Qt5 synthetic tablet events with exact pressure and resource/effective-setting readback"}, indent=2))
