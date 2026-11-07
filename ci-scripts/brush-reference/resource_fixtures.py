"""Original GBR/PNG/GIH source-acquisition fixtures; no engine implementation.

GBR/GIH: https://developer.gimp.org/core/standards/gbr/ and /gih/.
KPP resource fields independently inspected in the official KDE RGBA bundle,
Git blob 318d2861adfe3a5eff9f2d4ab5064e944fd8a2ff. In that serialized corpus,
GIH resources use type gbr_brush. Source effective XML is checked separately.
"""
import hashlib
import json
import struct
import zipfile
import zlib
from xml.etree.ElementTree import Element, SubElement, tostring


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(rgba):
    rows = b"".join(b"\0" + rgba[y * 32:(y + 1) * 32] for y in range(8))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def gbr(name, pixels, channels):
    label = name.encode("ascii") + b"\0"
    if channels not in (1, 4) or len(pixels) != 64 * channels:
        raise ValueError("Original tip must be 8x8 gray or RGBA")
    return struct.pack(">7I", 28 + len(label), 2, 8, 8, channels, 0x47494d50, 400) + label + pixels


def originals():
    # Asymmetry, transparency and intermediate coverage distinguish orientation,
    # mask polarity and premultiplied color handling without borrowed assets.
    alpha = bytes(0 if x == 0 or y == 0 else (64, 128, 192, 255)[(x + 2 * y) % 4]
                  for y in range(8) for x in range(8))
    rgba = bytes(v for y in range(8) for x in range(8)
                 for v in ((255, 0, 0, 255), (0, 255, 0, 128),
                           (0, 0, 255, 64), (255, 255, 255, 0))[(x // 2 + y // 4) % 4])
    result = {"gz-original-alpha.gbr": gbr("GZ Original alpha tip", alpha, 1),
              "gz-original-color.png": png(rgba)}
    frames = [gbr("GZ Original sequence frame " + str(i),
                  bytes(v for a in alpha for v in (*color, a)), 4)
              for i, color in enumerate(((255, 0, 0), (0, 255, 0), (0, 0, 255)))]
    for selection in ("incremental", "random"):
        header = ("GZ Original sequence " + selection + "\n3 ncells:3 step:400 dim:1 "
                  "cols:1 rows:1 cellwidth:8 cellheight:8 rank0:3 selection:" + selection + "\n")
        result["gz-original-" + selection + ".gih"] = header.encode("ascii") + b"".join(frames)
    return result


def prepare(root, resources, preset_writer, group):
    assets = originals()
    (resources / "brushes").mkdir(parents=True, exist_ok=True)
    packages = root / "output" / "resource-packages"
    packages.mkdir(parents=True, exist_ok=True)
    for filename, data in assets.items():
        (resources / "brushes" / filename).write_bytes(data)
    specifications = ([("random", 8, 0, "sequence", i) for i in range(100)]
                      if group in ("gihstatistics", "gihlabels") else
                      [(kind, size, angle, path, 0) for kind in ("alpha", "color-mask", "color-image", "incremental")
                       for size in (8, 16) for angle in (0, 90) for path in ("stamp", "sequence")])
    if group == "gihcadence":
        from gih_cadence_fixtures import plans
        specifications = [(kind, 8, 0, "sequence", replicate) for kind, spacing, mode, replicate in plans()]
    names, cases = [], []
    for index, (kind, size, angle, path, replicate) in enumerate(specifications):
        spacing = 4
        if group == "gihcadence":
            from gih_cadence_fixtures import plans
            spacing = plans()[index][1]
        filename = ("gz-original-alpha.gbr" if kind == "alpha" else "gz-original-color.png"
                    if kind.startswith("color") else "gz-original-" + kind + ".gih")
        data = assets[filename]
        brush_type = "png_brush" if filename.endswith("png") else "gbr_brush"
        mask = kind in ("alpha", "color-mask")
        brush = Element("Brush", type=brush_type, BrushVersion="2", filename=filename,
                        scale=str(size / 8), spacing=str(spacing), angle="0", useAutoSpacing="0",
                        autoSpacingCoeff="1", ColorAsMask="1" if mask else "0", preserveLightness="0")
        definition = tostring(brush, encoding="unicode")
        name = "GZ Original dry resource " + group + " " + str(index).zfill(3)
        overrides = {"brush_definition": definition, "PressureSize": "false", "SizeUseCurve": "false"}
        preset_data = preset_writer(name, "dry", False, overrides, size, spacing, 0)
        (resources / "paintoppresets" / (name + ".kpp")).write_bytes(preset_data)
        ns = "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0"
        manifest = Element("{" + ns + "}manifest", {"{" + ns + "}version": "1.2"})
        SubElement(manifest, "{" + ns + "}file-entry", {
            "{" + ns + "}full-path": "/", "{" + ns + "}media-type": "application/x-krita-resourcebundle"})
        package_name = "resource-" + str(index).zfill(3) + ".bundle"
        with zipfile.ZipFile(packages / package_name, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("mimetype", "application/x-krita-resourcebundle")
            for member, content in (("brushes/" + filename, data), ("paintoppresets/" + name + ".kpp", preset_data)):
                SubElement(manifest, "{" + ns + "}file-entry", {
                    "{" + ns + "}full-path": member, "{" + ns + "}media-type": member.split("/")[0],
                    "{" + ns + "}md5sum": hashlib.md5(content).hexdigest()})
                archive.writestr(member, content)
            archive.writestr("META-INF/manifest.xml", tostring(manifest, encoding="utf-8"))
        names.append(name)
        count = 32 if kind == "random" else 12
        end = 257 if path == "stamp" else 256 + (count - 1) * size * 4
        cases.append({"preset": name, "variant": "random acquisition" if kind == "random" else "baseline",
                      "diameter": size, "spacing": 4, "pressure": [1, 1], "opacity": 1, "flow": 1,
                      "start": [256, 1024], "end": [end, 1024], "brushRotationDegrees": angle,
                      "background": "transparent", "overrides": overrides, "expectUnchanged": False,
                      "candidatePackage": "output/resource-packages/" + package_name,
                      "resourceProbe": {"filename": filename, "kind": kind, "type": brush_type,
                                        "sha256": hashlib.sha256(data).hexdigest(), "md5": hashlib.md5(data).hexdigest(),
                                        "bytes": len(data), "originalDimensions": [8, 8], "replicate": replicate,
                                        "expectedSlots": 1 if path == "stamp" else count,
                                        "rect": [232, 1000, end - 232 + 25, 48],
                                        "qualification": "100 independent strokes; distribution acquisition, not seed parity"
                                        if kind == "random" else "deterministic source repeat; candidate rendering exploratory"}})
    if group == "gihcadence":
        from gih_cadence_fixtures import configure
        for index, case in enumerate(cases):
            configure(case, index)
    if group == "gihlabels":
        from pathlib import Path
        import runpy
        vertices = runpy.run_path(str(Path(__file__).parent / "gzbrush_reference" / "label_probe.py"))["vertices"]
        for case in cases:
            case["labelProbe"] = {"slots": 3072, "bands": 3, "slotsPerBand": 1024}
            case["sourcePath"] = vertices()
            case["start"], case["end"] = case["sourcePath"][0], case["sourcePath"][-1]
            case["resourceProbe"]["expectedSlots"] = 3072
            case["resourceProbe"]["rect"] = [0, 0, 2048, 1536]
    if group == "bitmaptips":
        import bitmap_diagnostics
        bitmap_diagnostics.append(root, resources, preset_writer, names, cases)
    (root / "output" / "fixtures.json").write_text(json.dumps({
        "version": "5.3.4.1", "group": group, "presets": names, "cases": cases,
        "license": "CC0-1.0", "width": 2048, "height": 2048,
        "input": ("Paired Node paths, discrete strokes and Qt tablet cadence; exact per-case delivery recorded"
                  if group == "gihcadence" else
                  "Node.paintPath API; one continuous authored path; no physical tablet claim"
                  if group == "gihlabels" else
                  "Node.paintLine API; authored and delivered endpoints recorded; no physical tablet claim")}, indent=2))
