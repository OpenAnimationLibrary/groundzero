"""Original procedural masked-brush acquisitions; no source equations assumed."""
import copy
import json
from xml.etree.ElementTree import Element, SubElement, tostring


def prepare(root, resources, preset_writer, group):
    if group not in ("maskwash", "maskbuildup"):
        raise RuntimeError("Unknown masked-brush acquisition group")
    action = "2" if group == "maskwash" else "1"
    names, cases = [], []
    for mode in ("off", "multiply", "subtract"):
        for linked, coefficient in ((True, .5), (True, 1), (False, 1)):
            for opacity, flow in ((1, 1), (.5, 1), (1, .5), (.5, .5)):
                for primary_spacing, mask_spacing in ((.25, .5), (.25, 2), (2, .5)):
                    name = "GZ Original dry " + group + " " + str(len(cases)).zfill(3)
                    brush = Element("Brush", type="auto_brush", BrushVersion="2", spacing=str(mask_spacing),
                                    angle="0", randomness="0", density="1", useAutoSpacing="0")
                    SubElement(brush, "MaskGenerator", id="default", type="rect", diameter="12", ratio="0.5",
                               hfade="1", vfade="1", antialiasEdges="0", spikes="2")
                    nested = {"paintop": "paintbrush", "brush_definition": tostring(brush, encoding="unicode"),
                              "OpacityValue": str(opacity), "FlowValue": str(flow),
                              "OpacityUseCurve": "false", "FlowUseCurve": "false",
                              "PressureOpacity": "false", "PressureFlow": "false", "PressureSize": "false",
                              "PressureRatio": "false", "PressureRotation": "false", "PressureSpacing": "false",
                              "PressureScatter": "false", "PressureMirror": "false", "Texture/Pattern/Enabled": "false"}
                    overrides = {"PaintOpAction": action, "MaskingBrush/Enabled": str(mode != "off").lower(),
                                 "MaskingBrush/MaskingCompositeOp": "multiply" if mode == "off" else mode,
                                 "MaskingBrush/UseMasterSize": str(linked).lower(),
                                 "MaskingBrush/MasterSizeCoeff": str(coefficient)}
                    overrides.update({"MaskingBrush/Preset/" + key: value for key, value in nested.items()})
                    (resources / "paintoppresets" / (name + ".kpp")).write_bytes(
                        preset_writer(name, "dry", False, overrides, 24, primary_spacing, 0))
                    names.append(name)
                    case = {"preset": name, "variant": "baseline", "diameter": 24, "spacing": primary_spacing,
                            "pressure": [1, 1], "opacity": 1, "flow": 1,
                            "start": [896, 1024], "end": [1152, 1024], "background": "transparent",
                            "overrides": overrides, "expectUnchanged": False,
                            "maskProbe": {"mode": mode, "rect": [864, 992, 320, 64],
                                          "resourceKind": "original embedded procedural tips",
                                          "qualification": "metadata acquisition; source behavior unverified"}}
                    if mode != "off":
                        case["changeExpectation"] = "acquire enabled mask result, including explicitly recorded empty output"
                    cases.append(case)
    # Heldouts distinguish retained colored paint/background compositing from
    # an alpha-only equation. Preserve the original 108 canonical identities.
    for mode_index in range(3):
        for foreground in ([48, 176, 112, 255], [192, 80, 208, 255]):
            for background in ("transparent", "blend-stripes"):
                for primary_opacity in (1, .5):
                    for offset in (3, 6):  # mask opacity .5/flow1 vs opacity1/flow.5
                        case = copy.deepcopy(cases[mode_index * 36 + offset])
                        name = "GZ Original dry " + group + " " + str(len(cases)).zfill(3)
                        case.update(preset=name, foregroundRGBA8=foreground,
                                    background=background, opacity=primary_opacity)
                        case["overrides"]["OpacityValue"] = str(primary_opacity)
                        case["maskProbe"]["qualification"] = "heldout retained color/background composition"
                        (resources / "paintoppresets" / (name + ".kpp")).write_bytes(
                            preset_writer(name, "dry", False, case["overrides"], 24, .25, 0))
                        names.append(name)
                        cases.append(case)
    (root / "output" / "fixtures.json").write_text(json.dumps({
        "version": "5.3.4.1", "group": group, "presets": names, "cases": cases,
        "license": "CC0-1.0", "width": 2048, "height": 2048,
        "input": "Node.paintLine pressure1, paired independent source strokes"}, indent=2))
