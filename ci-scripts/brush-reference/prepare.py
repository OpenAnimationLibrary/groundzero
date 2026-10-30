"""Create original, redistributable brush fixtures and isolated Krita settings."""
import json
import base64
import hashlib
import os
from pathlib import Path
import shutil
import struct
import zlib
from xml.etree.ElementTree import Element, SubElement, tostring


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def original_texture_pattern():
    # Original opaque four-level vertical steps, not a bundled brush asset.
    pixels = b"".join(b"\0" + b"".join(bytes([(x // 4) * 85] * 3 + [255])
                                          for x in range(16)) for y in range(16))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))


def preset(name, mode, modern, overrides=None, diameter=32, spacing=0.1, antialias=1, hardness=1, shape="circle", ratio=1, angle=0):
    root = Element("Preset", name=name, paintopid="paintbrush" if mode == "dry" else "colorsmudge")
    fields = {
        "paintop": root.attrib["paintopid"], "CompositeOp": "normal", "EraserMode": "false",
        "OpacityValue": "1", "OpacityUseCurve": "false", "FlowValue": "1", "FlowUseCurve": "false",
        "PressureSize": "false", "PressureRotation": "false", "PressureRatio": "false",
        "PressureScatter": "false", "PressureSpacing": "false", "PressureColorRate": "true", "ColorRateUseCurve": "false",
        "PressureSmudgeRate": "true", "PressureSmudgeRadius": "true",
        "SmudgeRateUseCurve": "false", "SmudgeRadiusUseCurve": "false",
        "Texture/Pattern/Enabled": "false", "MergedPaint": "false", "PaintOpAction": "1",
        "PaintOpSettings/isAirbrushing": "false", "PaintOpSettings/ignoreSpacing": "false",
        "PaintOpSettings/updateSpacingBetweenDabs": "false",
        "KisPrecisionOption/AutoPrecisionEnabled": "false",
        "KisPrecisionOption/precisionLevel": "5",
        "SmudgeRateMode": "0" if mode == "smear" else "1",
        "SmudgeRateUseNewEngine": str(modern).lower(), "SmudgeRateSmearAlpha": "true",
        "SmudgeRateValue": "0.5", "ColorRateValue": "0.25", "SmudgeRadiusValue": "1",
        "brush_definition": '<Brush type="auto_brush" BrushVersion="2" spacing="0.1" angle="0" randomness="0" density="1" useAutoSpacing="0"><MaskGenerator id="default" type="circle" diameter="32" ratio="1" hfade="1" vfade="1" antialiasEdges="1" spikes="2"/></Brush>',
    }
    fields["brush_definition"] = fields["brush_definition"].replace('diameter="32"', f'diameter="{diameter}"').replace('spacing="0.1"', f'spacing="{spacing}"').replace('antialiasEdges="1"', f'antialiasEdges="{antialias}"')
    fields["brush_definition"] = fields["brush_definition"].replace('hfade="1"', f'hfade="{hardness}"').replace('vfade="1"', f'vfade="{hardness}"').replace('type="circle"', f'type="{shape}"')
    fields["brush_definition"] = fields["brush_definition"].replace('ratio="1"', f'ratio="{ratio}"').replace('angle="0"', f'angle="{angle}"')
    fields.update(overrides or {})
    for key, value in fields.items():
        SubElement(root, "param", name=key, type="string" if key in ("paintop", "CompositeOp", "brush_definition") else "internal").text = value
    xml = tostring(root, encoding="utf-8")
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 6, 0, 0, 0))
    png += chunk(b"tEXt", b"version\x002.2")
    png += chunk(b"zTXt", b"preset\x00\x00" + zlib.compress(xml))
    png += chunk(b"IDAT", zlib.compress(b"\0" * 18))
    return png + chunk(b"IEND", b"")


root = Path(os.environ["BRUSH_REFERENCE_ROOT"]).resolve()
resources = root / "data" / "krita"
(resources / "paintoppresets").mkdir(parents=True, exist_ok=True)
(resources / "pykrita").mkdir(parents=True, exist_ok=True)
(root / "config").mkdir(parents=True, exist_ok=True)
(root / "output").mkdir(parents=True, exist_ok=True)
source = Path(__file__).resolve().parent
shutil.copytree(source / "gzbrush_reference", resources / "pykrita" / "gzbrush_reference", dirs_exist_ok=True)
(resources / "pykrita" / "gzbrush_reference.desktop").write_text("[Desktop Entry]\nType=Service\nServiceTypes=Krita/PythonPlugin\nX-KDE-Library=gzbrush_reference\nX-Python-2-Compatible=false\nName=Brush Reference Fixtures\nComment=Render original API-line fixtures for brush qualification\n")
(root / "config" / "kritarc").write_text("[General]\nuseOpenGL=false\n[python]\nenable_gzbrush_reference=true\n")
names, cases = [], []
group = os.environ.get("BRUSH_REFERENCE_GROUP", "parameters")
if group not in ("parameters", "geometry", "tablet", "hardpixel", "wetsteps", "wethold", "wetstate", "pixelopacity", "pixelopacitytablet", "pixelcontrols", "pixelpressure", "pixelsmallpressure", "wettexture", "wettexture3", "wetswitches", "wetcombined", "wetpressure", "wettransfer", "wetopacitycombo", "wetboundary", "texturemasks", "wetkernel", "wetkernelscale", "wetkernelcount", "pixelsoft", "pixelsoftsize", "pixelrectsize", "pixelaarange", "pixelshape", "pixelprecision", "pixelcurves", "pixelblend"):
    raise RuntimeError("Unknown reference fixture group")
profiles = [("dry", False)] if group in ("geometry", "tablet", "hardpixel", "pixelopacity", "pixelopacitytablet", "pixelcontrols", "pixelpressure", "pixelsmallpressure", "pixelsoft", "pixelsoftsize", "pixelrectsize", "pixelaarange", "pixelshape", "pixelprecision", "pixelcurves", "pixelblend") else [("dry", False), ("smear", False), ("dulling", False), ("smear", True), ("dulling", True)]
if group in ("wetsteps", "wethold", "wetstate", "wettexture", "wettexture3", "wetswitches", "wetcombined", "wetpressure", "wettransfer", "wetopacitycombo", "wetboundary", "wetkernel", "wetkernelscale", "wetkernelcount"):
    profiles = [("smear", False), ("dulling", False), ("smear", True), ("dulling", True)]
if group in ("wetkernel", "wetkernelscale", "wetkernelcount"):
    profiles = [("dulling", False), ("dulling", True)]
if group == "texturemasks":
    texture_png = original_texture_pattern()
    (resources / "patterns").mkdir(parents=True, exist_ok=True)
    (resources / "patterns" / "gz-original-steps.png").write_bytes(texture_png)
for mode, modern in profiles:
    base = "GZ Original " + mode + (" new" if modern else " legacy")
    variants = [
        ("baseline", {}, 32, .1, [1, 1], 1, 1),
        ("diameter 7", {}, 7, .1, [1, 1], 1, 1),
        ("diameter 128", {}, 128, .1, [1, 1], 1, 1),
        ("spacing 025", {}, 32, .25, [1, 1], 1, 1),
        ("spacing 100", {}, 32, 1, [1, 1], 1, 1),
        ("opacity half", {}, 32, .1, [1, 1], .5, 1),
        ("flow half", {}, 32, .1, [1, 1], 1, .5),
        ("pressure ramp", {"PressureSize": "true", "SizeUseCurve": "true", "SizeUseSameCurve": "true", "SizecommonCurve": "0,0;1,1;", "SizeSensor": '<params id="pressure"><curve>0,0;1,1;</curve></params>'}, 32, .1, [.1, 1], 1, 1),
    ]
    if group == "geometry":
        variants = [(f"geometry diameter {diameter} antialias {edge}", {}, diameter, 5, [1, 1], 1, 1)
                    for diameter in (1, 7, 8, 32, 64, 128) for edge in (0, 1)]
    if group == "tablet":
        variants = [variant for variant in variants if variant[0] in ("baseline", "pressure ramp")]
    if group == "hardpixel":
        variants = [(f"hard diameter {diameter} spacing {round(spacing * 100):03d}", {}, diameter, spacing, [1, 1], 1, 1)
                    for diameter in (7, 32, 128) for spacing in (.1, .25, 1)]
    if group == "pixelshape":
        variants = [(f"ellipse diameter {diameter} ratio {ratio} angle {angle}", {}, diameter, 5, [1, 1], 1, 1)
                    for diameter in (16, 64) for ratio in (.25, .5, 1) for angle in (0, 45, 90)]
    if group in ("pixelopacity", "pixelopacitytablet"):
        variants = [(f"pixel mode {painting} opacity {round(opacity * 100):03d} flow {round(flow * 100):03d}",
                     {"PaintOpAction": str(1 - painting), "OpacityValue": str(opacity), "FlowValue": str(flow)},
                     32, .1, [1, 1], opacity, flow)
                    for painting in (0, 1) for opacity, flow in ((.25, 1), (.5, 1), (1, .25), (1, .5), (.5, .5), (0, 1), (1, 0))]
    if group == "pixelcontrols":
        variants = [(f"pixel action {action} pressureflow {enabled} opacity {round(opacity * 100):03d} flow {round(flow * 100):03d}",
                     {"PaintOpAction": str(action), "PressureFlow": enabled,
                      "OpacityValue": str(opacity), "FlowValue": str(flow)},
                     32, .1, [1, 1], opacity, flow)
                    for action in (0, 1, 2) for enabled in ("false", "true")
                    for opacity, flow in ((.25, 1), (1, .25), (1, 0))]
    if group == "pixelblend":
        variants = []
        for blend in ("normal", "multiply", "screen", "overlay"):
            for action in (0, 1):
                for opacity, flow in ((1, 1), (.5, 1), (1, .25), (.5, .25), (0, 1), (1, 0)):
                    for spacing in ((.25,) if opacity == 0 or flow == 0 else (5, .25)):
                        label = f"blend {blend} action {action} opacity {opacity} flow {flow} spacing {spacing}"
                        variants.append((label, {"CompositeOp": blend, "PaintOpAction": str(action),
                            "OpacityValue": str(opacity), "FlowValue": str(flow)},
                            32, spacing, [1, 1], opacity, flow))
        for tip in ("soft", "aa", "rect"):
            for blend in ("normal", "multiply", "screen", "overlay"):
                for action in (0, 1):
                    for opacity, flow in ((1, 1), (.5, .25)):
                        label = f"blend {blend} action {action} opacity {opacity} flow {flow} spacing .25 tip {tip}"
                        variants.append((label, {"CompositeOp": blend, "PaintOpAction": str(action),
                            "OpacityValue": str(opacity), "FlowValue": str(flow)},
                            32, .25, [1, 1], opacity, flow))
    precision_positions = {}
    if group == "pixelprecision":
        pressure_curve = {"PressureSize": "true", "SizeUseCurve": "true", "SizeUseSameCurve": "true", "SizecommonCurve": "0,0;1,1;", "SizeSensor": '<params id="pressure"><curve>0,0;1,1;</curve></params>'}
        variants = []
        for pressure_value in (.05, .5, .9625, .98125):
            for dx, dy in ((0, 0), (.4, 0), (0, .3), (.4, .3)):
                # Numeric dot suffixes are resource versions in the pinned
                # source loader: "y 0.3.kpp" supersedes "y 0.kpp". Keep
                # original identities distinct using explicit integer units.
                label = f"precision mask pressure {round(pressure_value * 100000):05d} xpermille {round(dx * 1000):04d} ypermille {round(dy * 1000):04d}"
                variants.append((label, pressure_curve, 32, 5, [pressure_value, pressure_value], 1, 1))
                precision_positions[label] = (dx, dy, 1)
        for spacing in (.01, .015625, .02, .1):
            for length in (1, 4):
                label = f"precision spacing ppm {round(spacing * 1000000):06d} length {length}"
                variants.append((label, {**pressure_curve, "OpacityValue": ".5"}, 32, spacing, [1, 1], .5, 1))
                precision_positions[label] = (0, 0, length)
    if group == "pixelcurves":
        variants = []
        for low, high in ((.25, .75), (.75, .25)):
            curve = f"0,{low};1,{high};"
            for common in ("true", "false"):
                overrides = {"PressureSize": "true", "SizeUseCurve": "true", "SizeUseSameCurve": common,
                             "SizecommonCurve": curve if common == "true" else "0,0;1,1;",
                             "SizeSensor": f'<params id="pressure"><curve>{curve}</curve></params>'}
                for pressure_value in (0, .5, 1):
                    label = f"pressure endpoints {low} {high} shared {common} value {pressure_value}"
                    variants.append((label, overrides, 32, 5, [pressure_value, pressure_value], 1, 1))
    if group in ("pixelpressure", "pixelsmallpressure"):
        overrides = {"PressureSize": "true", "SizeUseCurve": "true", "SizeUseSameCurve": "true", "SizecommonCurve": "0,0;1,1;", "SizeSensor": '<params id="pressure"><curve>0,0;1,1;</curve></params>'}
        variants = [(f"pixel pressure {round(start * 100):03d} to {round(end * 100):03d} spacing {round(spacing * 100):03d}",
                     overrides, 32, spacing, [start, end], 1, 1)
                    for start, end in ((0, 1), (1, 0), (.1, 1), (1, .1), (.25, .75), (.5, .5))
                    for spacing in (.1, .25, 1)]
        variants.extend((f"pixel constant pressure {round(value * 1000):04d} spacing 100",
                         overrides, 32, 1, [value, value], 1, 1)
                        for value in (.1, .244, .501, .9))
    if group == "pixelsmallpressure":
        variants = [(f"small pressure {round(start * 10000):04d} to {round(end * 10000):04d} spacing {round(spacing * 100):03d}",
                     overrides, 32, spacing, [start, end], 1, 1)
                    for start, end in ((.04, .04), (.05, .05), (.0625, .0625), (0, .05), (.05, 0))
                    for spacing in (.1, .25, 1)]
    if group == "pixelsoft":
        variants = [(f"soft {shape} hardness {round(hardness * 100):03d} antialias {edge}", {}, 32, 5, [1, 1], 1, 1)
                    for shape in ("circle", "rect") for hardness in (0, .25, .5, .75, 1) for edge in (0, 1)]
    if group == "pixelaarange":
        variants = [(f"geometry diameter {diameter} antialias 1", {}, diameter, 5, [1, 1], 1, 1)
                    for diameter in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 32, 64, 128)]
    if group == "pixelsoftsize":
        variants = [(f"soft circle hardness {round(hardness * 100):03d} diameter {diameter} antialias 0", {}, diameter, 5, [1, 1], 1, 1)
                    for diameter in (7, 8, 9, 12, 15, 16, 17, 24, 64, 128) for hardness in (0, .25, .5, .75)]
    if group == "pixelrectsize":
        variants = [(f"soft rect hardness {round(hardness * 100):03d} diameter {diameter} antialias 0", {}, diameter, 5, [1, 1], 1, 1)
                    for diameter in (7, 64, 128) for hardness in (0, .25, .5, 1)]
    if mode == "dry" and group == "parameters":
        for length in (1, 2, 3, 4, 7, 16, 32):
            variants.append(("short line " + str(length), {}, 32, .1, [1, 1], 1, 1))
    if mode != "dry":
        for value in ("0", "1"):
            variants.append(("pickup " + value, {"SmudgeRateValue": value}, 32, .1, [1, 1], 1, 1))
            variants.append(("color rate " + value, {"ColorRateValue": value}, 32, .1, [1, 1], 1, 1))
        variants.append(("smear alpha off", {"SmudgeRateSmearAlpha": "false"}, 32, .1, [1, 1], 1, 1))
        variants.append(("smudge length off", {"PressureSmudgeRate": "false"}, 32, .1, [1, 1], 1, 1))
        if mode == "dulling":
            for value in ("0", "3", "100", "300"):
                variants.append(("radius " + value, {"SmudgeRadiusValue": value}, 32, .1, [1, 1], 1, 1))
    if group in ("wetsteps", "wethold", "wetstate", "wettexture", "wettexture3", "wetswitches", "wetcombined", "wetpressure", "wettransfer", "wetopacitycombo", "wetkernel", "wetkernelscale", "wetkernelcount"):
        variants = [(f"wet step pickup {round(pickup * 100):03d} color {round(color * 100):03d} dabs {dabs}",
                     {"SmudgeRateValue": str(pickup), "ColorRateValue": str(color), "SmudgeRadiusValue": "0"},
                     32, 5, [1, 1], 1, 1)
                    for pickup, color in ((0, 0), (0, .25), (.5, 0), (.5, .25), (.5, 1), (1, 0), (1, .25), (1, 1))
                    for dabs in (1, 2)]
    if group == "wethold":
        variants = [(f"wet hold diameter {diameter} pickup {round(pickup * 100):03d} color {round(color * 1000):03d} dabs {dabs}",
                     {"SmudgeRateValue": str(pickup), "ColorRateValue": str(color), "SmudgeRadiusValue": "0"},
                     diameter, min(5, 160 / diameter), [1, 1], 1, 1)
                    for diameter, pickup, color in ((7, .5, 0), (32, .5, 0), (128, .5, 0), (32, .25, .125), (32, .75, .75))
                    for dabs in (2, 3)]
    if group == "wetstate":
        variants = [(f"wet state pickup {round(pickup * 100):03d} color {round(color * 1000):03d} third {third}",
                     {"SmudgeRateValue": str(pickup), "ColorRateValue": str(color), "SmudgeRadiusValue": "0"},
                     32, 5, [1, 1], 1, 1)
                    for pickup, color in ((.5, 0), (.5, .25), (1, 0), (.25, .125))
                    for third in ("transparent", "yellow")]
    if group in ("wettexture", "wettexture3"):
        dabs = 3 if group == "wettexture3" else 2
        variants = [(f"wet patch radius {radius} color {round(color * 100):03d} dabs {dabs}",
                     {"SmudgeRateValue": ".5", "ColorRateValue": str(color), "SmudgeRadiusValue": str(radius)},
                     32, 5, [1, 1], 1, 1)
                    for radius in (0, 50, 100, 300) for color in (0, .25)]
    if group == "wetswitches":
        variants = [(f"wet control {control} color {round(color * 100):03d} dabs {dabs}",
                     {"SmudgeRateValue": ".5", "ColorRateValue": str(color), "SmudgeRadiusValue": "0", control: "false"},
                     32, 5, [1, 1], 1, 1)
                    for control in ("SmudgeRateSmearAlpha", "PressureSmudgeRate")
                    for color in (0, .25) for dabs in (1, 2, 3)]
    if group == "wetcombined":
        variants = [(f"wet combined color {round(color * 100):03d} dabs {dabs}",
                     {"SmudgeRateValue": ".5", "ColorRateValue": str(color), "SmudgeRadiusValue": "0",
                      "SmudgeRateSmearAlpha": "false", "PressureSmudgeRate": "false"},
                     32, 5, [1, 1], 1, 1)
                    for color in (0, .25) for dabs in (1, 2, 3)]
    if group == "wetpressure":
        variants = []
        for control in ("SmudgeRate", "ColorRate", "Both"):
            for pressure in (0, .25, .75, 1):
                overrides = {"SmudgeRateValue": ".5", "ColorRateValue": ".25", "SmudgeRadiusValue": "0"}
                for prefix in (("SmudgeRate", "ColorRate") if control == "Both" else (control,)):
                    overrides.update({"Pressure" + prefix: "true", prefix + "UseCurve": "true",
                                      prefix + "UseSameCurve": "true", prefix + "commonCurve": "0,0;1,1;",
                                      prefix + "Sensor": '<params id="pressure"><curve>0,0;1,1;</curve></params>'})
                variants.append((f"wet pressure {control} value {round(pressure * 100):03d} dabs 3",
                                 overrides, 32, 5, [pressure, pressure], 1, 1))
    if group == "wetkernel":
        variants = [(f"wet kernel radius {radius} offset {offset} dabs 2",
                     {"SmudgeRateValue": "1", "ColorRateValue": "0", "SmudgeRadiusValue": str(radius)},
                     32, 5, [1, 1], 1, 1)
                    for radius, offsets in ((0, (0, 4)),
                                            (50, range(-12, 13)),
                                            (100, range(-20, 21)),
                                            (300, range(-100, 101, 4)))
                    for offset in offsets]
    if group == "wetkernel":
        variants.extend((f"wet kernel radius {radius} offset {offset} axis row dabs 2",
                         {"SmudgeRateValue": "1", "ColorRateValue": "0", "SmudgeRadiusValue": str(radius)},
                         32, 5, [1, 1], 1, 1)
                        for radius in (50, 100) for offset in (-16, -8, -4, 0, 4, 8, 16))
    if group == "wettransfer":
        variants = []
        for opacity, flow in ((1, 0), (1, .25), (1, .5), (1, 1),
                              (0, 1), (.25, 1), (.5, 1), (.75, 1)):
            for dabs in (2, 3):
                variants.append((f"wet transfer opacity {round(opacity * 100):03d} flow {round(flow * 100):03d} dabs {dabs}",
                                 {"SmudgeRateValue": ".5", "ColorRateValue": ".25", "SmudgeRadiusValue": "0",
                                  "OpacityValue": str(opacity), "FlowValue": str(flow)},
                                 32, 5, [1, 1], opacity, flow))
        for color_rate in (.25, 1):
            for dabs in (2, 3):
                variants.append((f"wet transfer color disabled value {round(color_rate * 100):03d} dabs {dabs}",
                                 {"SmudgeRateValue": ".5", "ColorRateValue": str(color_rate), "SmudgeRadiusValue": "0",
                                  "PressureColorRate": "false"}, 32, 5, [1, 1], 1, 1))
        for prefix in ("Flow", "Opacity"):
            for enabled in ((False, True) if prefix == "Flow" else (True,)):
                for pressure in (.25, .75):
                    overrides = {"SmudgeRateValue": ".5", "ColorRateValue": ".25", "SmudgeRadiusValue": "0",
                                 "Pressure" + prefix: str(enabled).lower(), prefix + "UseCurve": "true",
                                 prefix + "UseSameCurve": "true", prefix + "commonCurve": "0,0;1,1;",
                                 prefix + "Sensor": '<params id="pressure"><curve>0,0;1,1;</curve></params>'}
                    for dabs in (2, 3):
                        variants.append((f"wet transfer {prefix} curve enabled {int(enabled)} pressure {round(pressure * 100):03d} dabs {dabs}",
                                         overrides, 32, 5, [pressure, pressure], 1, 1))
    if group == "wetboundary":
        variants = [(f"wet boundary {edge} trace {trace} color {round(color * 100):03d}",
                     {"SmudgeRateValue": ".5", "ColorRateValue": str(color), "SmudgeRadiusValue": "0"},
                     32, .5, [1, 1], 1, 1)
                    for edge in ("left", "right", "top", "bottom")
                    for trace in ("entry", "exit", "reentry") for color in (0, .25)]
    if group == "texturemasks":
        variants = []
        settings = [(texturing_mode, soft, invert, strength, 1)
                    for texturing_mode in (0, 1) for soft in (False, True)
                    for invert in (False, True) for strength in (.5, 1)]
        settings.extend((texturing_mode, False, False, 1, .5) for texturing_mode in (0, 1))
        for texturing_mode, soft, invert, strength, opacity in settings:
            overrides = {
                "Texture/Pattern/Enabled": "true", "Texture/Pattern/Scale": "1",
                "Texture/Pattern/Brightness": "0", "Texture/Pattern/Contrast": "1",
                "Texture/Pattern/NeutralPoint": ".5", "Texture/Pattern/OffsetX": "0",
                "Texture/Pattern/OffsetY": "0", "Texture/Pattern/isRandomOffsetX": "false",
                "Texture/Pattern/isRandomOffsetY": "false", "Texture/Pattern/CutoffPolicy": "0",
                "Texture/Pattern/CutoffLeft": "0", "Texture/Pattern/CutoffRight": "255",
                "Texture/Pattern/Invert": str(invert).lower(), "Texture/Pattern/AutoInvertOnErase": "false",
                "Texture/Pattern/TexturingMode": str(texturing_mode),
                "Texture/Pattern/UseSoftTexturing": str(soft).lower(),
                "Texture/Pattern/PatternFileName": "gz-original-steps.png",
                "Texture/Pattern/Name": "gz-original-steps",
                "Texture/Pattern/PatternMD5Sum": hashlib.md5(texture_png).hexdigest(),
                "Texture/Pattern/Pattern": base64.b64encode(texture_png).decode("ascii"),
                "PressureTexture/Strength/": "true", "Texture/Strength/Value": str(strength),
                "Texture/Strength/UseCurve": "false", "OpacityValue": str(opacity),
                "SmudgeRateValue": ".5", "ColorRateValue": ".25", "SmudgeRadiusValue": "0"}
            variants.append((f"texture mask mode {texturing_mode} soft {int(soft)} invert {int(invert)} strength {round(strength * 100):03d} opacity {round(opacity * 100):03d}",
                             overrides, 32, 5, [1, 1], opacity, 1))
    if group == "wetopacitycombo":
        variants = []
        combinations = [
            ("alpha off", {"SmudgeRateSmearAlpha": "false"}, 1),
            ("length off", {"PressureSmudgeRate": "false"}, 1),
            ("alpha length off", {"SmudgeRateSmearAlpha": "false", "PressureSmudgeRate": "false"}, 1),
            ("pickup025 color075", {"SmudgeRateValue": ".25", "ColorRateValue": ".75"}, 1),
            ("pickup075 color0125", {"SmudgeRateValue": ".75", "ColorRateValue": ".125"}, 1),
        ]
        for control in ("SmudgeRate", "ColorRate", "Both"):
            for pressure in (.25, .75):
                controls = {}
                for prefix in (("SmudgeRate", "ColorRate") if control == "Both" else (control,)):
                    controls.update({"Pressure" + prefix: "true", prefix + "UseCurve": "true",
                                     prefix + "UseSameCurve": "true", prefix + "commonCurve": "0,0;1,1;",
                                     prefix + "Sensor": '<params id="pressure"><curve>0,0;1,1;</curve></params>'})
                combinations.append((f"{control} pressure {round(pressure * 100):03d}", controls, pressure))
        for label, controls, pressure in combinations:
            overrides = {"OpacityValue": ".5", "SmudgeRateValue": ".5", "ColorRateValue": ".25", "SmudgeRadiusValue": "0"}
            overrides.update(controls)
            for dabs in (2, 3):
                variants.append((f"wet opacity combined {label} dabs {dabs}", overrides,
                                 32, 5, [pressure, pressure], .5, 1))
    if group == "wetkernelscale":
        variants = []
        regions = ((16, 100), (64, 100)) if modern else (
            (16, 100), (16, 150), (16, 300), (64, 100), (64, 150), (64, 300),
            (32, 125), (32, 150), (32, 200), (32, 250))
        for diameter, radius in regions:
            extent = diameter * radius / 200
            offsets = sorted(set((0, -round(extent), round(extent),
                                  -round(extent / 2), round(extent / 2),
                                  -round(extent) - 1, round(extent) + 1)))
            for axis in ("column", "row"):
                for offset in offsets:
                    variants.append((f"wet kernel radius {radius} offset {offset} diameter {diameter} axis {axis} dabs 2",
                                     {"SmudgeRateValue": "1", "ColorRateValue": "0", "SmudgeRadiusValue": str(radius)},
                                     diameter, 5, [1, 1], 1, 1))
    if group == "wetkernelcount":
        planned = json.loads((source / "wetkernelcount.json").read_text())
        if planned.get("schema") != 1 or len(planned.get("cases", [])) != 163:
            raise RuntimeError("Unexpected bounded Dulling probe plan")
        variants = []
        kernel_objectives = {}
        for probe in planned["cases"]:
            if probe["model"] != ("new" if modern else "legacy"):
                continue
            diameter, radius = probe["diameter"], probe["radiusPercent"]
            axis, offset = probe["axis"], probe["offset"]
            if not (0 < diameter <= 128 and 0 <= radius <= 300 and
                    axis in ("column", "row") and abs(offset) <= 200):
                raise RuntimeError("Out-of-bounds Dulling probe")
            label = f"wet kernel radius {radius} offset {offset} diameter {diameter} axis {axis} dabs 2"
            if label in kernel_objectives:
                raise RuntimeError("Duplicate Dulling probe")
            kernel_objectives[label] = probe["reason"]
            # Half-pixel diameters with spacing four retain integral API
            # endpoints; the source diameter itself must remain fractional.
            variants.append((label, {"SmudgeRateValue": "1", "ColorRateValue": "0",
                                     "SmudgeRadiusValue": str(radius)},
                             diameter, 4, [1, 1], 1, 1))
    for label, overrides, diameter, spacing, pressure, opacity, flow in variants:
        # Resource names also become KPP filenames. The source treats decimal
        # dot segments as resource versions, so all generated identities use
        # "p" for dots. Numeric settings and the descriptive variant label stay
        # unchanged. Reject collisions rather than silently overwrite fixtures.
        name = (base if label == "baseline" else base + " " + label).replace(".", "p")
        if name in names:
            raise RuntimeError("Duplicate generated preset identity: " + name)
        edge = int(label.rsplit(" ", 1)[1]) if group in ("geometry", "pixelsoft", "pixelsoftsize", "pixelrectsize", "pixelaarange") else (0 if group in ("hardpixel", "wetsteps", "wethold", "wetstate", "pixelopacity", "pixelopacitytablet", "pixelcontrols", "pixelpressure", "pixelsmallpressure", "wettexture", "wettexture3", "wetswitches", "wetcombined", "wetpressure", "wettransfer", "wetopacitycombo", "wetboundary", "texturemasks", "wetkernel", "wetkernelscale", "wetkernelcount", "pixelshape", "pixelprecision", "pixelcurves", "pixelblend") else 1)
        hardness = int(label.split()[3]) / 100 if group in ("pixelsoft", "pixelsoftsize", "pixelrectsize") else 1
        shape = label.split()[1] if group in ("pixelsoft", "pixelsoftsize", "pixelrectsize") else "circle"
        if group == "pixelblend":
            edge = int(label.endswith("tip aa"))
            hardness = .5 if label.endswith("tip soft") else 1
            shape = "rect" if label.endswith("tip rect") else "circle"
        (resources / "paintoppresets" / (name + ".kpp")).write_bytes(preset(name, mode, modern, overrides, diameter, spacing, edge, hardness, shape,
            float(label.split()[4]) if group == "pixelshape" else 1,
            float(label.split()[6]) * 3.141592653589793 / 180 if group == "pixelshape" else 0))
        names.append(name)
        end_x = 256 + int(label.rsplit(" ", 1)[1]) if label.startswith("short line ") else 1792
        if group in ("geometry", "pixelsoft", "pixelsoftsize", "pixelrectsize", "pixelaarange", "pixelshape", "pixelprecision", "pixelcurves", "pixelblend"):
            end_x = 257
        # Explicit action 0 is wash, so zero flow is a real source no-op.
        # The earlier PaintingMode field was inactive; it cannot establish
        # behavior for this correctly serialized setting.
        expected_empty = (group in ("pixelopacity", "pixelopacitytablet") and (opacity == 0 or (overrides.get("PaintOpAction") == "0" and flow == 0))) or (mode != "dry" and not modern and label == "pickup 0") or (group == "geometry" and diameter == 1 and label.endswith("antialias 0"))
        cases.append({"preset": name, "variant": label, "diameter": diameter, "spacing": spacing, "pressure": pressure, "opacity": opacity, "flow": flow, "start": [256, 1024], "end": [end_x, 1024], "brushRotationDegrees": float(label.split()[6]) if group == "pixelshape" else 0, "background": "transparent" if group in ("geometry", "pixelsoft", "pixelsoftsize", "pixelrectsize", "pixelaarange", "pixelshape", "pixelprecision", "pixelcurves", "pixelblend") or label.startswith("short line ") else "strips", "overrides": overrides, "expectUnchanged": expected_empty})
        if group == "pixelblend":
            cases[-1].update({"background": "blend-stripes",
                "end": [257 if spacing == 5 else 352, 1024],
                "expectUnchanged": opacity == 0 or (overrides["PaintOpAction"] == "0" and flow == 0)})
        if group == "pixelprecision":
            dx, dy, length = precision_positions[label]
            # Use the same fractional-capable delivery for every precision
            # case, including its zero-offset baseline. Node.paintLine accepts
            # integer QPoint endpoints and cannot measure subpixel geometry.
            case = cases[-1]
            case.update({"start": [256 + dx, 1024 + dy],
                         "end": [256 + dx + length, 1024 + dy],
                         "probePositions": [256, 257, 258, 259, 260],
                         "delivery": "Qt tablet events", "canvasCenter": [256, 1024]})
            case["events"] = [{"x": point[0], "y": point[1],
                               "pressure": pressure[i], "tiltX": 0,
                               "tiltY": 0, "rotation": 0,
                               "timeUs": round(i * 1000000 / 240)}
                              for i, point in enumerate((case["start"], case["end"]))]
        if group in ("wetsteps", "wethold", "wetstate", "wettexture", "wettexture3", "wetswitches", "wetcombined", "wetpressure", "wettransfer", "wetopacitycombo", "wetkernel", "wetkernelscale", "wetkernelcount"):
            case = cases[-1]
            dabs = 3 if group == "wetstate" else int(label.rsplit(" ", 1)[1])
            step = diameter * spacing
            case.update({"end": [257 + step * (dabs - 1), 1024], "background": "wet-halves",
                         "wetBoundary": 256 + step / 2,
                         "probePositions": [256, round(256 + step), round(256 + 2 * step)],
                         "foregroundRGBA8": [0, 255, 0, 255],
                         "expectUnchanged": (not modern and float(overrides["SmudgeRateValue"]) == 0) or
                                            (float(overrides["SmudgeRateValue"]) == 0 and float(overrides["ColorRateValue"]) == 0)})
            if group in ("wettexture", "wettexture3"):
                case.update({"wetPattern": "two-pixel-red-yellow",
                             "probePositions": [416, 572, 574, 576, 578, 580] if group == "wettexture3" else [412, 414, 416, 418, 420]})
            if group == "wetstate":
                case.update({"wetThirdBoundary": 496,
                             "wetThirdRGBA8": [0, 0, 0, 0] if label.endswith("transparent") else [255, 255, 0, 255]})
            if dabs == 1:
                case["changeExpectation"] = "measure first-dab initialization"
        if group in ("wetkernel", "wetkernelscale", "wetkernelcount"):
            cases[-1].update({"wetKernelOffset": int(label.split()[5]),
                              "wetPattern": "opaque-black-white-row" if "axis row" in label else "opaque-black-white-column",
                              "probePositions": [256, round(256 + diameter * spacing)],
                              "wetBoundary": 256 + diameter * spacing * .8,
                              "changeExpectation": "measure Dulling pickup kernel; unqualified until independently implemented"})
        if group == "wetkernelcount":
            cases[-1].update({"kernelProbeObjective": kernel_objectives[label],
                              "sourceCandidateGate": float(diameter).is_integer() and
                                  (modern or float(overrides["SmudgeRadiusValue"]) <= 100),
                              "changeExpectation": "sample-count/window measurement; full-image gate only within existing candidate eligibility"})
        if group == "wetpressure":
            cases[-1]["changeExpectation"] = "measure pressure-controlled wet equation; candidate gate enforced"
        if group == "wetboundary":
            edge, trace = label.split()[2], label.split()[4]
            paths = {"entry": [(-96, 1024), (128, 1024)],
                     "exit": [(128, 1024), (-96, 1024)],
                     "reentry": [(128, 1024), (-96, 1024), (-96, 1088), (128, 1088)]}
            def transform(point):
                x, y = point
                return (x, y) if edge == "left" else (2048 - x, y) if edge == "right" else (y, x) if edge == "top" else (y, 2048 - x)
            points = [paths[trace][0]]
            for first, last in zip(paths[trace], paths[trace][1:]):
                steps = max(abs(last[0] - first[0]), abs(last[1] - first[1])) // 8
                points.extend((first[0] + (last[0] - first[0]) * i // steps,
                               first[1] + (last[1] - first[1]) * i // steps) for i in range(1, steps + 1))
            points = [transform(point) for point in points]
            row = 1088 if trace == "reentry" else 1024
            bounds = {"left": [0, row - 8, 64, 16], "right": [1984, row - 8, 64, 16],
                      "top": [row - 8, 0, 16, 64], "bottom": [row - 8, 1984, 16, 64]}
            outside = transform((-48, row))
            cases[-1].update({"start": points[0], "end": points[-1], "background": "wet-halves",
                              "wetBoundary": 64, "foregroundRGBA8": [0, 255, 0, 255],
                              "delivery": "Qt tablet events", "canvasCenter": transform((16, 1056 if trace == "reentry" else 1024)),
                              "boundaryProbeRect": bounds[edge],
                              "outsideProbeRect": [outside[0] - 8, outside[1] - 8, 16, 16],
                              "changeExpectation": "continuous off-document state candidate gate; source outside-layer state acquired separately",
                              "events": [{"x": x, "y": y, "pressure": 1, "tiltX": 0, "tiltY": 0, "rotation": 0,
                                          "timeUs": round(i * 1000000 / 240)} for i, (x, y) in enumerate(points)]})
        if group == "texturemasks":
            case = cases[-1]
            case.update({"end": [257 if mode == "dry" else 577, 1024],
                         "background": "transparent" if mode == "dry" else "wet-halves",
                         "wetBoundary": 336, "foregroundRGBA8": [0, 255, 0, 255],
                         "probePositions": [244, 248, 252, 256, 260, 264, 268] if mode == "dry"
                                           else [564, 568, 572, 576, 580, 584, 588],
                         "texturePatternSha256": hashlib.sha256(texture_png).hexdigest(),
                         "changeExpectation": "acquire source texture mask equations; no source candidate admitted yet"})
        if group == "wetopacitycombo":
            cases[-1]["changeExpectation"] = "half-opacity cross-control source candidate gate enforced"
        if group == "wettransfer":
            cases[-1]["expectUnchanged"] = opacity == 0
            cases[-1]["wetTransferProbe"] = ("static-flow" if label.startswith("wet transfer opacity") and opacity == 1
                                               else "opacity" if label.startswith("wet transfer opacity")
                                               else "color-disabled" if "color disabled" in label
                                               else "flow-curve" if "Flow curve" in label else "opacity-curve")
            cases[-1]["changeExpectation"] = "static flow candidate gate; other transfer semantics acquired independently"
        if group in ("pixelopacitytablet", "pixelcontrols") and flow == 0:
            cases[-1]["changeExpectation"] = "measure freehand zero-flow boundary; candidate gate remains enforced"
        if group in ("tablet", "pixelopacitytablet", "pixelcontrols"):
            case = cases[-1]
            case.update({"start": [768, 1024], "end": [1280, 1024], "background": "transparent", "delivery": "Qt tablet events"})
            case["events"] = [{"x": 768 + i * 8, "y": 1024,
                               "pressure": pressure[0] + (pressure[1] - pressure[0]) * i / 64,
                               "tiltX": 0, "tiltY": 0, "rotation": 0,
                               "timeUs": round(i * 1000000 / 240)} for i in range(65)]
(root / "output" / "fixtures.json").write_text(json.dumps({"version": "5.3.4.1", "group": group, "presets": names, "cases": cases, "license": "CC0-1.0", "width": 2048, "height": 2048, "input": "Qt synthetic tablet events; not physical tablet delivery" if group in ("tablet", "pixelopacitytablet", "pixelcontrols", "wetboundary", "pixelprecision") else "Node.paintLine API; not physical tablet delivery"}, indent=2))
