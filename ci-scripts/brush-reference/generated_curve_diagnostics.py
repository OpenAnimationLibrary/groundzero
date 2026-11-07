"""Original scalar/curve discriminators for Gaussian and curve-Soft masks.

These are source observations, never a candidate equation or verification claim.
Only existing public serialized mask controls are varied.
"""
import json
from xml.etree.ElementTree import Element, SubElement, tostring


def plans():
    controls = []
    for shape in ("circle", "rect"):
        for diameter in (16, 64):
            for label, curve in (
                    ("constant-zero", "0,0;1,0;"),
                    ("constant-quarter", "0,0.25;1,0.25;"),
                    ("constant-half", "0,0.5;1,0.5;"),
                    ("constant-three-quarters", "0,0.75;1,0.75;"),
                    ("constant-one", "0,1;1,1;"),
                    ("identity-three-knots", "0,0;0.5,0.5;1,1;"),
                    ("descending-linear", "0,1;1,0;")):
                controls.append({"id": "soft-" + shape + "-" + str(diameter) + "-" + label,
                                 "generator": "soft", "shape": shape, "diameter": diameter,
                                 "hfade": .5, "vfade": .5, "curve": curve})
            for fade in (0, .1, .5, .9, 1):
                controls.append({"id": "gauss-" + shape + "-" + str(diameter) + "-fade-" + str(fade),
                                 "generator": "gauss", "shape": shape, "diameter": diameter,
                                 "hfade": fade, "vfade": fade, "curve": None})
    for shape in ("circle", "rect"):
        for horizontal, vertical in ((.1, .9), (.9, .1)):
            controls.append({"id": "gauss-" + shape + "-64-axes-" + str(horizontal) + "-" + str(vertical),
                             "generator": "gauss", "shape": shape, "diameter": 64,
                             "hfade": horizontal, "vfade": vertical, "curve": None})
    for shape in ("circle", "rect"):
        for diameter in (16, 64):
            for label, curve in (("constant-one", "0,1;1,1;"),
                                 ("identity-two-knots", "0,0;1,1;"),
                                 ("descending-linear", "0,1;1,0;")):
                for dx, dy in ((-.125, 0), (.125, 0), (0, -.125), (0, .125), (.5, 0), (0, .5)):
                    controls.append({"id": "soft-" + shape + "-" + str(diameter) + "-" + label + "-origin-" + str(dx) + "-" + str(dy),
                                     "generator": "soft", "shape": shape, "diameter": diameter,
                                     "hfade": .5, "vfade": .5, "curve": curve,
                                     "originDelta": [dx, dy]})
    return controls


def prepare(root, resources, writer):
    cases = []
    for index, control in enumerate(plans()):
        name = "GZ Original dry generated curve discriminator " + str(index).zfill(3)
        definition = Element("Brush", type="auto_brush", BrushVersion="2", spacing="1",
                             angle="0", randomness="0", density="1", useAutoSpacing="0")
        attributes = {"id": control["generator"], "type": control["shape"],
                      "diameter": str(control["diameter"]), "ratio": "1",
                      "hfade": str(control["hfade"]), "vfade": str(control["vfade"]),
                      "antialiasEdges": "1", "spikes": "2"}
        if control["curve"] is not None:
            attributes["softness_curve"] = control["curve"]
        SubElement(definition, "MaskGenerator", attributes)
        overrides = {"brush_definition": tostring(definition, encoding="unicode"),
                     "Spacing/Isotropic": "false"}
        (resources / "paintoppresets" / (name + ".kpp")).write_bytes(
            writer(name, "dry", False, overrides, control["diameter"], 1, 1))
        dx, dy = control.get("originDelta", [0, 0])
        start, end = [1024 + dx, 1024 + dy], [1025 + dx, 1024 + dy]
        case = {"preset": name, "sourceEngine": "paintbrush", "variant": "baseline",
                      "diameter": control["diameter"], "spacing": 1,
                      "pressure": [1, 1], "opacity": 1, "flow": 1,
                      "foregroundRGBA8": [173, 91, 47, 255], "start": start,
                      "end": end, "background": "transparent", "overrides": overrides,
                      "delivery": "Node.paintPath" if dx or dy else "Node.paintLine", "canvasCenter": [1024, 1024],
                      "expectUnchanged": False, "generatedTipProbe": attributes,
                      "generatedCurveDiscriminator": control,
                      "changeExpectation": "Observe constant/linear curve and fade limits, including empty outputs; no source profile admitted"}
        if dx or dy:
            case["generatedCurvePath"] = True
            case["sourcePath"] = [start, end]
        cases.append(case)
    (root / "output" / "fixtures.json").write_text(json.dumps({
        "version": "5.3.4.1", "group": "generatedcurves", "presets": [case["preset"] for case in cases],
        "cases": cases, "license": "CC0-1.0", "width": 2048, "height": 2048,
        "input": "Public Node.paintLine integer controls and Node.paintPath fractional-origin discriminators; deterministic repeat required"}, indent=2))
