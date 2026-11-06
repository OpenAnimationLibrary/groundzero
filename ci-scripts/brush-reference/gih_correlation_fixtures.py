"""Independent original GIH correlation controls; existing matrices are untouched."""
import hashlib
import json
from pathlib import Path
import shutil
import zipfile
from xml.etree.ElementTree import Element, SubElement, tostring
from gih_correlation_contract import GROUPS, REPLICATES, SLOTS, plan, validate_manifest
from resource_fixtures import originals


def prepare(root, resources, preset_writer, group):
    if group not in GROUPS:
        raise ValueError("Unknown bounded GIH correlation fixture group")
    configuration = group[len("gih-correlation-"):]
    authored = plan(configuration)
    assets = originals()
    (resources / "brushes").mkdir(parents=True, exist_ok=True)
    packages = root / "output" / "resource-packages"
    packages.mkdir(parents=True, exist_ok=True)
    plugin = resources / "pykrita" / "gzbrush_reference"
    plugin.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(Path(__file__).with_name("gih_correlation_contract.py"),
                    plugin / "gih_correlation_contract.py")
    cases = []
    for kind, count in (("random", REPLICATES), ("incremental", 2)):
        filename = "gz-original-" + kind + ".gih"
        data = assets[filename]
        (resources / "brushes" / filename).write_bytes(data)
        sha256, md5 = hashlib.sha256(data).hexdigest(), hashlib.md5(data).hexdigest()
        for replicate in range(count):
            name = "GZ Original dry " + group + " " + kind + " " + str(replicate).zfill(3)
            brush = Element("Brush", type="gbr_brush", BrushVersion="2", filename=filename,
                            scale="1", spacing="4", angle="0", useAutoSpacing="0", autoSpacingCoeff="1",
                            ColorAsMask="0", preserveLightness="0")
            overrides = {"brush_definition": tostring(brush, encoding="unicode"),
                         "PressureSize": "false", "SizeUseCurve": "false", "PressureMirror": "false",
                         "HorizontalMirrorEnabled": "false", "VerticalMirrorEnabled": "false",
                         "MirrorUseCurve": "true", "MirrorUseSameCurve": "true", "MirrorValue": "1",
                         "MirrorcurveMode": "0", "MirrorcommonCurve": "0,0;1,1;",
                         "MirrorSensor": '<params id="pressure"><curve>0,0;1,1;</curve></params>',
                         "Pressureh": "false", "Pressures": "false", "Pressurev": "false",
                         "Spacing/Isotropic": "false"}
            kpp = preset_writer(name, "dry", False, overrides, 8, 4, 0)
            (resources / "paintoppresets" / (name + ".kpp")).write_bytes(kpp)
            package_name = "gih-correlation-" + kind + "-" + str(replicate).zfill(3) + ".bundle"
            ns = "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0"
            manifest = Element("{" + ns + "}manifest", {"{" + ns + "}version": "1.2"})
            SubElement(manifest, "{" + ns + "}file-entry", {
                "{" + ns + "}full-path": "/", "{" + ns + "}media-type": "application/x-krita-resourcebundle"})
            with zipfile.ZipFile(packages / package_name, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("mimetype", "application/x-krita-resourcebundle")
                for member, content in (("brushes/" + filename, data), ("paintoppresets/" + name + ".kpp", kpp)):
                    SubElement(manifest, "{" + ns + "}file-entry", {
                        "{" + ns + "}full-path": member, "{" + ns + "}media-type": member.split("/")[0],
                        "{" + ns + "}md5sum": hashlib.md5(content).hexdigest()})
                    archive.writestr(member, content)
                archive.writestr("META-INF/manifest.xml", tostring(manifest, encoding="utf-8"))
            cases.append({**({"phaseBoundaryProbe": authored["phaseBoundary"]} if authored["phaseBoundary"] else {}),
                          "preset": name, "variant": "random acquisition" if kind == "random" else "baseline",
                          "diameter": 8, "spacing": 4, "pressure": [1, 1], "opacity": 1, "flow": 1,
                          "start": authored["vertices"][0], "end": authored["vertices"][-1],
                          "sourcePath": authored["vertices"], "correlationCenters": authored["centers"],
                          "sourceEngine": "paintbrush", "delivery": authored["mode"], "background": "transparent",
                          "foregroundRGBA8": [173, 91, 47, 255], "brushRotationDegrees": 0,
                          "overrides": overrides, "expectUnchanged": False,
                          "candidatePackage": "output/resource-packages/" + package_name,
                          "resourceProbe": {"filename": filename, "kind": kind, "type": "gbr_brush",
                              "sha256": sha256, "md5": md5, "bytes": len(data), "originalDimensions": [8, 8],
                              "replicate": replicate, "expectedSlots": SLOTS, "rect": authored["rect"],
                              "qualification": "Original source correlation measurement; no inferred RNG model"},
                          "gihCorrelationProbe": {"configuration": configuration, "replicate": replicate,
                              "replicates": count, "stochastic": kind == "random", "slots": SLOTS,
                              "terminalContinuation": authored["terminalContinuation"]}})
    fixtures = {"version": "5.3.4.1", "group": group, "presets": [case["preset"] for case in cases],
                "cases": cases, "license": "CC0-1.0", "width": 2048, "height": 2048,
                "randomizeExecution": True,
                "input": "100 independent random strokes plus two deterministic controls in randomized order; exact public API delivery retained",
                "qualification": "Causal measurement only; no source random-selection implementation or parity claim"}
    validate_manifest(fixtures)
    (root / "output" / "fixtures.json").write_text(json.dumps(fixtures, indent=2), encoding="utf-8")
