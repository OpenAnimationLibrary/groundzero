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


def preset(name, mode, modern, overrides=None, diameter=32, spacing=0.1, antialias=1):
    root = Element("Preset", name=name, paintopid="paintbrush" if mode == "dry" else "colorsmudge")
    fields = {
        "paintop": root.attrib["paintopid"], "CompositeOp": "normal", "EraserMode": "false",
        "OpacityValue": "1", "OpacityUseCurve": "false", "FlowValue": "1", "FlowUseCurve": "false",
        "PressureSize": "false", "PressureRotation": "false", "PressureRatio": "false",
        "PressureScatter": "false", "PressureSpacing": "false", "PressureColorRate": "true", "ColorRateUseCurve": "false",
        "PressureSmudgeRate": "true", "PressureSmudgeRadius": "true",
        "SmudgeRateUseCurve": "false", "SmudgeRadiusUseCurve": "false",
        "Texture/Pattern/Enabled": "false", "MergedPaint": "false", "PaintingMode": "1",
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
if group not in ("parameters", "geometry", "tablet"):
    raise RuntimeError("Unknown reference fixture group")
profiles = [("dry", False)] if group in ("geometry", "tablet") else [("dry", False), ("smear", False), ("dulling", False), ("smear", True), ("dulling", True)]
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
    for label, overrides, diameter, spacing, pressure, opacity, flow in variants:
        name = base if label == "baseline" else base + " " + label
        (resources / "paintoppresets" / (name + ".kpp")).write_bytes(preset(name, mode, modern, overrides, diameter, spacing, int(label.rsplit(" ", 1)[1]) if group == "geometry" else 1))
        names.append(name)
        end_x = 256 + int(label.rsplit(" ", 1)[1]) if label.startswith("short line ") else 1792
        if group == "geometry":
            end_x = 257
        expected_empty = (mode != "dry" and not modern and label == "pickup 0") or (group == "geometry" and diameter == 1 and label.endswith("antialias 0"))
        cases.append({"preset": name, "variant": label, "diameter": diameter, "spacing": spacing, "pressure": pressure, "opacity": opacity, "flow": flow, "start": [256, 1024], "end": [end_x, 1024], "background": "transparent" if group == "geometry" or label.startswith("short line ") else "strips", "overrides": overrides, "expectUnchanged": expected_empty})
        if group == "tablet":
            case = cases[-1]
            case.update({"start": [768, 1024], "end": [1280, 1024], "background": "transparent", "delivery": "Qt tablet events"})
            case["events"] = [{"x": 768 + i * 8, "y": 1024,
                               "pressure": pressure[0] + (pressure[1] - pressure[0]) * i / 64,
                               "tiltX": 0, "tiltY": 0, "rotation": 0,
                               "timeUs": round(i * 1000000 / 240)} for i in range(65)]
(root / "output" / "fixtures.json").write_text(json.dumps({"version": "5.3.4.1", "group": group, "presets": names, "cases": cases, "license": "CC0-1.0", "width": 2048, "height": 2048, "input": "Qt synthetic tablet events; not physical tablet delivery" if group == "tablet" else "Node.paintLine API; not physical tablet delivery"}, indent=2))
