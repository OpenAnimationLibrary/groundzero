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


def preset(name, mode, modern):
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
        "SmudgeRateMode": "0" if mode == "smear" else "1",
        "SmudgeRateUseNewEngine": str(modern).lower(), "SmudgeRateSmearAlpha": "true",
        "SmudgeRateValue": "0.5", "ColorRateValue": "0.25", "SmudgeRadiusValue": "1",
        "brush_definition": '<Brush type="auto_brush" BrushVersion="2" spacing="0.1" angle="0" randomness="0" density="1" useAutoSpacing="0"><MaskGenerator id="default" type="circle" diameter="32" ratio="1" hfade="1" vfade="1" antialiasEdges="1" spikes="2"/></Brush>',
    }
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
names = []
for mode, modern in [("dry", False), ("smear", False), ("dulling", False), ("smear", True), ("dulling", True)]:
    name = "GZ Original " + mode + (" new" if modern else " legacy")
    (resources / "paintoppresets" / (name + ".kpp")).write_bytes(preset(name, mode, modern))
    names.append(name)
(root / "output" / "fixtures.json").write_text(json.dumps({"version": "5.3.4.1", "presets": names, "license": "CC0-1.0", "width": 2048, "height": 2048, "input": "Node.paintLine API; not physical tablet delivery"}, indent=2))
