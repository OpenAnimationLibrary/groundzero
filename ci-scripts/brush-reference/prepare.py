"""Create original, redistributable brush fixtures and isolated Krita settings."""
import json
import os
from pathlib import Path
import shutil
import struct
import zlib
from xml.etree.ElementTree import Element, SubElement, tostring


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


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
if group not in ("parameters", "geometry", "tablet", "hardpixel", "wetsteps", "wethold", "wetstate", "pixelopacity", "pixelopacitytablet", "pixelcontrols", "pixelpressure", "pixelsmallpressure", "wettexture", "wettexture3", "wetswitches", "wetcombined", "wetpressure", "wettransfer", "wetkernel", "wetkernelscale", "pixelsoft", "pixelsoftsize", "pixelrectsize", "pixelaarange", "pixelshape", "pixelprecision", "pixelcurves"):
    raise RuntimeError("Unknown reference fixture group")
profiles = [("dry", False)] if group in ("geometry", "tablet", "hardpixel", "pixelopacity", "pixelopacitytablet", "pixelcontrols", "pixelpressure", "pixelsmallpressure", "pixelsoft", "pixelsoftsize", "pixelrectsize", "pixelaarange", "pixelshape", "pixelprecision", "pixelcurves") else [("dry", False), ("smear", False), ("dulling", False), ("smear", True), ("dulling", True)]
if group in ("wetsteps", "wethold", "wetstate", "wettexture", "wettexture3", "wetswitches", "wetcombined", "wetpressure", "wettransfer", "wetkernel", "wetkernelscale"):
    profiles = [("smear", False), ("dulling", False), ("smear", True), ("dulling", True)]
if group in ("wetkernel", "wetkernelscale"):
    profiles = [("dulling", False), ("dulling", True)]
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
    precision_positions = {}
    if group == "pixelprecision":
        pressure_curve = {"PressureSize": "true", "SizeUseCurve": "true", "SizeUseSameCurve": "true", "SizecommonCurve": "0,0;1,1;", "SizeSensor": '<params id="pressure"><curve>0,0;1,1;</curve></params>'}
        variants = []
        for pressure_value in (.05, .5, .9625, .98125):
            for dx, dy in ((0, 0), (.4, 0), (0, .3), (.4, .3)):
                label = f"precision mask pressure {round(pressure_value * 100000):05d} x {dx} y {dy}"
                variants.append((label, pressure_curve, 32, 5, [pressure_value, pressure_value], 1, 1))
                precision_positions[label] = (dx, dy, 1)
        for spacing in (.01, .015625, .02, .1):
            for length in (1, 4):
                label = f"precision spacing {spacing} length {length}"
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
                    for diameter in (1, 2, 3, 4, 7, 8, 9, 10, 12, 16, 24, 32, 64, 128)]
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
    if group in ("wetsteps", "wethold", "wetstate", "wettexture", "wettexture3", "wetswitches", "wetcombined", "wetpressure", "wettransfer", "wetkernel", "wetkernelscale"):
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
    for label, overrides, diameter, spacing, pressure, opacity, flow in variants:
        name = base if label == "baseline" else base + " " + label
        edge = int(label.rsplit(" ", 1)[1]) if group in ("geometry", "pixelsoft", "pixelsoftsize", "pixelrectsize", "pixelaarange") else (0 if group in ("hardpixel", "wetsteps", "wethold", "wetstate", "pixelopacity", "pixelopacitytablet", "pixelcontrols", "pixelpressure", "pixelsmallpressure", "wettexture", "wettexture3", "wetswitches", "wetcombined", "wetpressure", "wettransfer", "wetkernel", "wetkernelscale", "pixelshape", "pixelprecision", "pixelcurves") else 1)
        hardness = int(label.split()[3]) / 100 if group in ("pixelsoft", "pixelsoftsize", "pixelrectsize") else 1
        shape = label.split()[1] if group in ("pixelsoft", "pixelsoftsize", "pixelrectsize") else "circle"
        (resources / "paintoppresets" / (name + ".kpp")).write_bytes(preset(name, mode, modern, overrides, diameter, spacing, edge, hardness, shape,
            float(label.split()[4]) if group == "pixelshape" else 1,
            float(label.split()[6]) * 3.141592653589793 / 180 if group == "pixelshape" else 0))
        names.append(name)
        end_x = 256 + int(label.rsplit(" ", 1)[1]) if label.startswith("short line ") else 1792
        if group in ("geometry", "pixelsoft", "pixelsoftsize", "pixelrectsize", "pixelaarange", "pixelshape", "pixelprecision", "pixelcurves"):
            end_x = 257
        # Explicit action 0 is wash, so zero flow is a real source no-op.
        # The earlier PaintingMode field was inactive; it cannot establish
        # behavior for this correctly serialized setting.
        expected_empty = (group in ("pixelopacity", "pixelopacitytablet") and (opacity == 0 or (overrides.get("PaintOpAction") == "0" and flow == 0))) or (mode != "dry" and not modern and label == "pickup 0") or (group == "geometry" and diameter == 1 and label.endswith("antialias 0"))
        cases.append({"preset": name, "variant": label, "diameter": diameter, "spacing": spacing, "pressure": pressure, "opacity": opacity, "flow": flow, "start": [256, 1024], "end": [end_x, 1024], "brushRotationDegrees": float(label.split()[6]) if group == "pixelshape" else 0, "background": "transparent" if group in ("geometry", "pixelsoft", "pixelsoftsize", "pixelrectsize", "pixelaarange", "pixelshape", "pixelprecision", "pixelcurves") or label.startswith("short line ") else "strips", "overrides": overrides, "expectUnchanged": expected_empty})
        if group == "pixelprecision":
            dx, dy, length = precision_positions[label]
            cases[-1].update({"start": [256 + dx, 1024 + dy],
                              "end": [256 + dx + length, 1024 + dy],
                              "probePositions": [256, 257, 258, 259, 260]})
        if group in ("wetsteps", "wethold", "wetstate", "wettexture", "wettexture3", "wetswitches", "wetcombined", "wetpressure", "wettransfer", "wetkernel", "wetkernelscale"):
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
        if group in ("wetkernel", "wetkernelscale"):
            cases[-1].update({"wetKernelOffset": int(label.split()[5]),
                              "wetPattern": "opaque-black-white-row" if "axis row" in label else "opaque-black-white-column",
                              "probePositions": [256, round(256 + diameter * spacing)],
                              "wetBoundary": 256 + diameter * spacing * .8,
                              "changeExpectation": "measure Dulling pickup kernel; unqualified until independently implemented"})
        if group == "wetpressure":
            cases[-1]["changeExpectation"] = "measure pressure-controlled wet equation; candidate gate enforced"
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
(root / "output" / "fixtures.json").write_text(json.dumps({"version": "5.3.4.1", "group": group, "presets": names, "cases": cases, "license": "CC0-1.0", "width": 2048, "height": 2048, "input": "Qt synthetic tablet events; not physical tablet delivery" if group in ("tablet", "pixelopacitytablet", "pixelcontrols") else "Node.paintLine API; not physical tablet delivery"}, indent=2))
