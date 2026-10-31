"""Original CC0 bitmap filter discriminators; acquisition, not renderer parity."""
import hashlib
import struct
import zipfile
from xml.etree.ElementTree import Element, SubElement, tostring


def append(root, resources, preset_writer, names, cases):
    from resource_fixtures import png

    assets = []
    for label, width, height in (("impulse", 8, 8), ("corner", 8, 8),
                                  ("ramp", 8, 8), ("odd", 5, 7),
                                  ("solid", 5, 7)):
        pixels = bytes(255 if label == "solid" else
                       (x * 31 + y * 7) % 256 if label == "ramp" else
                       255 if (x, y) == ((0, 0) if label == "corner" else
                                        (width // 2, height // 2)) else 0
                       for y in range(height) for x in range(width))
        title = ("GZ Original filter " + label).encode("ascii") + b"\0"
        data = struct.pack(">7I", 28 + len(title), 2, width, height, 1,
                           0x47494d50, 400) + title + pixels
        assets.append((label, width, height, "gbr_brush", data, True))
    # Independent RGB/alpha values expose straight versus premultiplied sampling
    # and PNG-as-mask luminance; the same resource is acquired in both modes.
    rgba = bytes(channel for y in range(8) for x in range(8)
                 for channel in ((x * 31 + 17) % 256, (y * 29 + 23) % 256,
                                 (x * 13 + y * 19 + 41) % 256,
                                 (0, 64, 128, 255)[(x + y) % 4]))
    for mask in (False, True):
        assets.append(("rgba-mask" if mask else "rgba-color", 8, 8,
                       "png_brush", png(rgba), mask))
    for label, width, height, brush_type, data, mask in assets:
        filename = "gz-original-filter-" + label + (".png" if brush_type == "png_brush" else ".gbr")
        (resources / "brushes" / filename).write_bytes(data)
        for scale, phase, angle in ((1, 0, 0), (1, .25, 0), (1.5, 0, 0),
                                    (2, 0, 0), (1, 0, 37)):
            index = len(cases)
            size = max(width, height) * scale
            name = "GZ Original dry resource bitmaptips " + str(index).zfill(3)
            definition = tostring(Element(
                "Brush", type=brush_type, BrushVersion="2", filename=filename,
                scale=str(scale), spacing="4", angle="0", useAutoSpacing="0",
                autoSpacingCoeff="1", ColorAsMask="1" if mask else "0",
                preserveLightness="0"), encoding="unicode")
            overrides = {"brush_definition": definition, "PressureSize": "false",
                         "SizeUseCurve": "false"}
            preset = preset_writer(name, "dry", False, overrides, size, 4, 0)
            (resources / "paintoppresets" / (name + ".kpp")).write_bytes(preset)
            ns = "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0"
            manifest = Element("{" + ns + "}manifest", {"{" + ns + "}version": "1.2"})
            SubElement(manifest, "{" + ns + "}file-entry", {
                "{" + ns + "}full-path": "/",
                "{" + ns + "}media-type": "application/x-krita-resourcebundle"})
            package = "output/resource-packages/resource-" + str(index).zfill(3) + ".bundle"
            with zipfile.ZipFile(root / package, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("mimetype", "application/x-krita-resourcebundle")
                for member, content in (("brushes/" + filename, data),
                                        ("paintoppresets/" + name + ".kpp", preset)):
                    SubElement(manifest, "{" + ns + "}file-entry", {
                        "{" + ns + "}full-path": member,
                        "{" + ns + "}media-type": member.split("/")[0],
                        "{" + ns + "}md5sum": hashlib.md5(content).hexdigest()})
                    archive.writestr(member, content)
                archive.writestr("META-INF/manifest.xml", tostring(manifest, encoding="utf-8"))
            names.append(name)
            cases.append({"preset": name, "variant": "original filter discriminator",
                          "diameter": size, "spacing": 4, "pressure": [1, 1],
                          "opacity": 1, "flow": 1, "start": [256 + phase, 1024 + phase],
                          "end": [257 + phase, 1024 + phase], "brushRotationDegrees": angle,
                          "background": "transparent", "overrides": overrides,
                          "expectUnchanged": False, "candidatePackage": package,
                          "bitmapPathProbe": True,
                          "resourceProbe": {"filename": filename, "kind": "filter-" + label,
                                            "type": brush_type, "sha256": hashlib.sha256(data).hexdigest(),
                                            "md5": hashlib.md5(data).hexdigest(), "bytes": len(data),
                                            "originalDimensions": [width, height], "replicate": 0,
                                            "expectedSlots": 1, "rect": [232, 1000, 48, 48],
                                            "qualification": "original filter discrimination; no source parity"}})
