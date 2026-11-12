"""Bounded original GIH causal acquisitions; no source selection equation."""
import base64
import hashlib
import json
import math
import re
import zlib
try:
    from .generated_controls_contract import closed_xml
except ImportError:
    from generated_controls_contract import closed_xml

WIDTH = HEIGHT = 2048
SLOTS = 32
STEP = 32
REPLICATES = 100
BASE_CONFIGURATIONS = ("line-exact", "line-tail", "path-tail", "dabs-tail", "halves-tail",
                       "west-tail", "north-tail", "translated-tail", "corners-tail")
PHASE_GRIDS = (2, 4, 8, 16, 32, 64, 128)
PHASE_CONFIGURATIONS = tuple("phase-" + axis + "-g" + str(grid).zfill(3) + "-" + partition
    for axis in ("x", "y") for grid in PHASE_GRIDS for partition in ("coarse", "dabs"))
CONFIGURATIONS = BASE_CONFIGURATIONS + PHASE_CONFIGURATIONS
GROUPS = tuple("gih-correlation-" + name for name in CONFIGURATIONS)
# Exact original bytes from the unchanged resource_fixtures.originals() corpus,
# also retained by reference run 42248393484; no app-distributed brush asset.
ORIGINALS = {
    "random": {"sha256": "d5c1b2ac401d1021cb214df3c338b61ea7bff5754f370ff83a31fc24fef24913",
               "md5": "0e8c9a8ba59d70531adb13f092e7ca6e", "bytes": 1057},
    "incremental": {"sha256": "5cbaff0356727e33710a5569ae8b2dc69b41924f0be78091d1ad464bc970734b",
                    "md5": "6cf1224fec489a17eaa51347159f8984", "bytes": 1067},
}


def plan(configuration):
    if configuration not in CONFIGURATIONS:
        raise ValueError("Unknown bounded GIH correlation configuration")
    centers = [[256 + index * STEP, 1024] for index in range(SLOTS)]
    phase_boundary = None
    if configuration in PHASE_CONFIGURATIONS:
        _, axis, encoded_grid, partition = configuration.split("-")
        grid = int(encoded_grid[1:])
        offset = 1 / (2 * grid)
        centers = [[x + (offset if axis == "x" else 0), y + (offset if axis == "y" else 0)]
                   for x, y in centers]
        phase_boundary = {"axis": axis, "gridBoundary": grid, "offset": offset,
                          "partition": partition, "sourceGridEstablished": False}
    if configuration == "west-tail":
        centers = list(reversed(centers))
    elif configuration == "north-tail":
        centers = [[1024, 1536 - index * STEP] for index in range(SLOTS)]
    elif configuration == "translated-tail":
        centers = [[x + 13, y + 19] for x, y in centers]
    elif configuration == "corners-tail":
        centers = [[512 + ((index + 1) // 2) * STEP,
                    512 + (index // 2) * STEP] for index in range(SLOTS)]
    continuation = 0 if configuration == "line-exact" else 1
    previous, last = centers[-2:]
    tail = [last[axis] + (last[axis] - previous[axis]) // STEP * continuation
            for axis in range(2)]
    vertices = [centers[0], tail]
    if configuration in ("dabs-tail", "corners-tail") or (phase_boundary and phase_boundary["partition"] == "dabs"):
        vertices = centers + [tail]
    elif configuration == "halves-tail":
        vertices = [[256 + index * STEP // 2, 1024]
                    for index in range((SLOTS - 1) * 2 + 1)] + [tail]
    left = math.floor(min(x for x, y in centers)) - 8
    top = math.floor(min(y for x, y in centers)) - 8
    right = math.ceil(max(x for x, y in centers)) + 9
    bottom = math.ceil(max(y for x, y in centers)) + 9
    return {"configuration": configuration, "centers": centers, "vertices": vertices,
            "mode": "Node.paintLine" if configuration.startswith("line-") else "Node.paintPath",
            "terminalContinuation": continuation, "rect": [left, top, right - left, bottom - top],
            "phaseBoundary": phase_boundary}


def validate_case(case):
    spec = case.get("gihCorrelationProbe", {})
    expected = plan(spec.get("configuration"))
    kind = case.get("resourceProbe", {}).get("kind")
    replicate = spec.get("replicate")
    stochastic = kind == "random"
    resource = case.get("resourceProbe", {})
    if (kind not in ("random", "incremental") or type(replicate) is not int or
            not 0 <= replicate < (REPLICATES if stochastic else 2) or
            spec.get("replicates") != (REPLICATES if stochastic else 2) or
            spec.get("stochastic") is not stochastic or spec.get("slots") != SLOTS or
            spec.get("terminalContinuation") != expected["terminalContinuation"] or
            case.get("phaseBoundaryProbe") != expected["phaseBoundary"] or
            case.get("sourcePath") != expected["vertices"] or
            case.get("correlationCenters") != expected["centers"] or
            case.get("start") != expected["vertices"][0] or case.get("end") != expected["vertices"][-1] or
            case.get("delivery") != expected["mode"] or
            case.get("variant") != ("random acquisition" if stochastic else "baseline") or
            case.get("sourceEngine") != "paintbrush" or case.get("background") != "transparent" or
            case.get("diameter") != 8 or case.get("spacing") != 4 or
            case.get("pressure") != [1, 1] or case.get("opacity") != 1 or case.get("flow") != 1 or
            case.get("brushRotationDegrees") != 0 or case.get("sourcePressureFlag", False) is not False or
            case.get("foregroundRGBA8") != [173, 91, 47, 255] or
            resource.get("filename") != "gz-original-" + kind + ".gih" or
            resource.get("type") != "gbr_brush" or resource.get("originalDimensions") != [8, 8] or
            resource.get("replicate") != replicate or resource.get("expectedSlots") != SLOTS or
            resource.get("rect") != expected["rect"] or
            type(resource.get("bytes")) is not int or not 0 < resource["bytes"] <= 16384 or
            type(resource.get("sha256")) is not str or re.fullmatch(r"[0-9a-f]{64}", resource["sha256"]) is None or
            type(resource.get("md5")) is not str or re.fullmatch(r"[0-9a-f]{32}", resource["md5"]) is None):
        raise ValueError("GIH correlation case differs from original bounded contract")
    if any(resource[key] != value for key, value in ORIGINALS[kind].items()):
        raise ValueError("GIH correlation requires the exact unchanged original resource bytes")
    group = "gih-correlation-" + expected["configuration"]
    if (case.get("preset") != "GZ Original dry " + group + " " + kind + " " + str(replicate).zfill(3) or
            case.get("candidatePackage") != "output/resource-packages/gih-correlation-" + kind + "-" + str(replicate).zfill(3) + ".bundle"):
        raise ValueError("GIH correlation preset/package identity differs from its independent stroke")
    return expected


def validate_manifest(fixtures):
    group = fixtures.get("group")
    if group not in GROUPS:
        raise ValueError("Unknown GIH correlation acquisition group")
    configuration = group[len("gih-correlation-"):]
    cases, names = fixtures.get("cases"), fixtures.get("presets")
    if (fixtures.get("version") != "5.3.4.1" or fixtures.get("width") != WIDTH or
            fixtures.get("height") != HEIGHT or fixtures.get("randomizeExecution") is not True or
            type(cases) is not list or len(cases) != REPLICATES + 2 or type(names) is not list or
            len(names) != len(cases) or any(type(name) is not str or not name for name in names) or
            len(set(names)) != len(names) or [case.get("preset") for case in cases] != names):
        raise ValueError("Incomplete GIH correlation acquisition manifest")
    identities = {}
    observed = {"random": set(), "incremental": set()}
    for case in cases:
        validate_case(case)
        spec, resource = case["gihCorrelationProbe"], case["resourceProbe"]
        kind = resource["kind"]
        if spec["configuration"] != configuration or spec["replicate"] in observed[kind]:
            raise ValueError("Mixed or duplicate GIH correlation configurations")
        observed[kind].add(spec["replicate"])
        identity = tuple(resource[key] for key in ("filename", "sha256", "md5", "bytes"))
        if kind in identities and identity != identities[kind]:
            raise ValueError("GIH correlation configuration changed its original resource")
        identities[kind] = identity
    if observed != {"random": set(range(REPLICATES)), "incremental": {0, 1}}:
        raise ValueError("Missing independent GIH random strokes or paired controls")
    return True


def validate_delivery(case, delivery):
    expected = validate_case(case)
    if expected["mode"] == "Node.paintLine":
        events = [{"type": "line-start", "position": case["start"], "pressure": 1},
                  {"type": "line-end", "position": case["end"], "pressure": 1}]
        valid = delivery.get("mode") == expected["mode"] and delivery.get("events") == events
    else:
        valid = (delivery.get("mode") == expected["mode"] and
                 delivery.get("vertices") == expected["vertices"] and
                 delivery.get("singleContinuousStroke") is True)
    if not valid or delivery.get("physicalTabletClaimed") is not False:
        raise ValueError("GIH correlation delivered API path differs from authored contract")
    return expected


def controls(case, fields, resolution):
    validate_case(case)
    resource = case["resourceProbe"]
    if (resolution.get("filename") != resource["filename"] or
            resolution.get("sha256OriginalFile") != resource["sha256"]):
        raise ValueError("GIH correlation source resource resolution differs")
    fixed = {"paintop": "paintbrush", "CompositeOp": "normal", "EraserMode": "false",
             "PaintOpAction": "1", "OpacityUseCurve": "false", "FlowUseCurve": "false",
             "PressureSize": "false", "SizeUseCurve": "false", "PressureRotation": "false",
             "PressureRatio": "false", "PressureScatter": "false", "PressureSpacing": "false",
             "PressureMirror": "false", "HorizontalMirrorEnabled": "false", "VerticalMirrorEnabled": "false",
             "MirrorUseCurve": "true", "MirrorUseSameCurve": "true", "MirrorValue": "1",
             "MirrorcurveMode": "0", "MirrorcommonCurve": "0,0;1,1;",
             "Pressureh": "false", "Pressures": "false", "Pressurev": "false",
             "Texture/Pattern/Enabled": "false", "MergedPaint": "false", "Spacing/Isotropic": "false",
             "PaintOpSettings/isAirbrushing": "false", "PaintOpSettings/ignoreSpacing": "false",
             "PaintOpSettings/updateSpacingBetweenDabs": "false",
             "KisPrecisionOption/AutoPrecisionEnabled": "false", "KisPrecisionOption/precisionLevel": "5"}
    for key, value in fixed.items():
        if fields.get(key) != value:
            raise ValueError("Effective GIH correlation setting differs: " + key)
    for key in ("OpacityValue", "FlowValue"):
        if float(fields.get(key, "nan")) != 1:
            raise ValueError("Effective GIH transfer scalar differs: " + key)
    sensor = closed_xml(fields.get("MirrorSensor", ""), "Malformed effective GIH Mirror sensor XML")
    brush = closed_xml(fields.get("brush_definition", ""), "Malformed effective GIH brush XML")
    if (sensor.tag != "params" or sensor.attrib != {"id": "pressure"} or len(sensor) != 1 or
            (sensor.text or "").strip() or
            sensor[0].tag != "curve" or sensor[0].attrib or len(sensor[0]) or
            (sensor[0].text or "").strip() != "0,0;1,1;"):
        raise ValueError("Effective disabled GIH Mirror sensor declaration differs")
    attributes = {"type": "gbr_brush", "BrushVersion": "2", "filename": resource["filename"],
                  "useAutoSpacing": "0", "ColorAsMask": "0", "preserveLightness": "0"}
    if (brush.tag != "Brush" or len(brush) or (brush.text or "").strip() or
            any(brush.get(key) != value for key, value in attributes.items())):
        raise ValueError("Effective GIH correlation brush definition differs")
    for key, value in {"scale": 1, "spacing": 4, "angle": 0, "autoSpacingCoeff": 1}.items():
        if float(brush.get(key, "nan")) != value:
            raise ValueError("Effective GIH correlation geometry differs: " + key)
    if any(key not in attributes and key not in ("scale", "spacing", "angle", "autoSpacingCoeff", "md5sum")
           for key in brush.attrib):
        raise ValueError("Unknown effective GIH correlation brush option")
    if brush.get("md5sum", resource["md5"]) != resource["md5"]:
        raise ValueError("Effective GIH correlation resource checksum differs")
    return {"effectiveControls": {key: fields[key] for key in tuple(fixed) + ("MirrorSensor", "OpacityValue", "FlowValue")},
            "effectiveBrushDefinition": fields["brush_definition"],
            "sha256OriginalFile": resolution["sha256OriginalFile"]}


def capture(before, after, width, case, fields, delivery, resolution):
    expected = validate_delivery(case, delivery)
    evidence = controls(case, fields, resolution)
    if (type(width) is not int or width != WIDTH or not isinstance(before, (bytes, bytearray)) or
            not isinstance(after, (bytes, bytearray)) or len(before) != WIDTH * HEIGHT * 4 or
            len(after) != len(before) or any(before)):
        raise ValueError("GIH correlation requires the complete original transparent BGRA canvas")
    left, top, rw, rh = expected["rect"]
    for y in range(HEIGHT):
        row = after[y * WIDTH * 4:(y + 1) * WIDTH * 4]
        outside = row[3::4] if y < top or y >= top + rh else row[3:left * 4:4] + row[(left + rw) * 4 + 3::4]
        if any(outside):
            raise ValueError("GIH correlation source coverage escaped retained bounds")
    pixels = b"".join(after[(y * WIDTH + left) * 4:(y * WIDTH + left + rw) * 4]
                      for y in range(top, top + rh))
    labels = frame_labels(pixels, expected)
    evidence.update({"configuration": expected["configuration"], "replicate": case["gihCorrelationProbe"]["replicate"],
                     "stochastic": case["gihCorrelationProbe"]["stochastic"],
                     "rect": expected["rect"], "encoding": "zlib-base64-bgra8-row-major",
                     "data": base64.b64encode(zlib.compress(pixels)).decode("ascii"),
                     "sha256Pixels": hashlib.sha256(pixels).hexdigest(), "frameSelections": labels,
                     "actualPaintedTipReadback": True, "deliveredPath": delivery,
                     "missingAuthoredSlots": labels.count(None),
                     "sourceSelectionModelVerified": False, "phaseGridEstablished": False, "pixelParityClaimed": False})
    return evidence


def frame_labels(pixels, expected):
    left, top, width, height = expected["rect"]
    if len(pixels) != width * height * 4:
        raise ValueError("Incomplete bounded GIH correlation retained pixels")
    labels = []
    for cx, cy in expected["centers"]:
        cx, cy = math.floor(cx), math.floor(cy)
        sums = [0, 0, 0]
        for y in range(cy - 6, cy + 7):
            for x in range(cx - 6, cx + 7):
                offset = ((y - top) * width + x - left) * 4
                for channel in range(3):
                    sums[channel] += pixels[offset + 2 - channel] * pixels[offset + 3]
        winner = max(range(3), key=lambda channel: sums[channel])
        if any(sums[channel] for channel in range(3) if channel != winner):
            raise ValueError("GIH correlation cell mixes original primary-color frames")
        labels.append(winner if sums[winner] else None)
    if not any(label is not None for label in labels):
        raise ValueError("GIH correlation source painted no identifiable original frames")
    return labels


def validate_evidence(case, evidence):
    expected = validate_delivery(case, evidence.get("deliveredPath", {}))
    controls(case, {**evidence.get("effectiveControls", {}),
                    "brush_definition": evidence.get("effectiveBrushDefinition", "")},
             {"filename": case["resourceProbe"]["filename"],
              "sha256OriginalFile": evidence.get("sha256OriginalFile")})
    if (evidence.get("configuration") != expected["configuration"] or
            evidence.get("replicate") != case["gihCorrelationProbe"]["replicate"] or
            evidence.get("stochastic") is not case["gihCorrelationProbe"]["stochastic"] or
            evidence.get("rect") != expected["rect"] or
            evidence.get("encoding") != "zlib-base64-bgra8-row-major" or
            evidence.get("actualPaintedTipReadback") is not True or
            evidence.get("sourceSelectionModelVerified") is not False or
            evidence.get("phaseGridEstablished") is not False or
            evidence.get("pixelParityClaimed") is not False):
        raise ValueError("Incomplete original GIH correlation source evidence")
    width, height = expected["rect"][2:]
    length = width * height * 4
    data = evidence.get("data")
    if type(data) is not str or not 0 < len(data) <= length * 2 + 128:
        raise ValueError("GIH correlation retained-pixel transport budget differs")
    try:
        compressed = base64.b64decode(data, validate=True)
        decoder = zlib.decompressobj()
        pixels = decoder.decompress(compressed, length + 1)
    except (ValueError, zlib.error) as error:
        raise ValueError("Malformed GIH correlation retained-pixel encoding") from error
    if (len(pixels) != length or not decoder.eof or decoder.unused_data or
            decoder.unconsumed_tail or hashlib.sha256(pixels).hexdigest() != evidence.get("sha256Pixels")):
        raise ValueError("GIH correlation retained-pixel integrity or budget differs")
    labels = frame_labels(pixels, expected)
    if (evidence.get("frameSelections") != labels or
            any(value is not None and type(value) is not int for value in evidence["frameSelections"]) or
            type(evidence.get("missingAuthoredSlots")) is not int or
            evidence["missingAuthoredSlots"] != labels.count(None)):
        raise ValueError("GIH correlation frame labels disagree with retained pixels")
    return True


def validate_execution(fixtures, records):
    validate_manifest(fixtures)
    if type(records) is not list or [row.get("preset") for row in records] != fixtures["presets"]:
        raise ValueError("Incomplete canonical GIH correlation source records")
    ordinals = [row.get("executionOrdinal") for row in records]
    if (any(type(value) is not int for value in ordinals) or
            sorted(ordinals) != list(range(REPLICATES + 2)) or ordinals == sorted(ordinals)):
        raise ValueError("Missing actual randomized GIH correlation execution evidence")
    for case, row in zip(fixtures["cases"], records):
        if any(row.get(key) != value for key, value in case.items()):
            raise ValueError("Source GIH correlation record changed authored inputs")
        evidence = row.get("gihCorrelationEvidence", {})
        validate_evidence(case, evidence)
        if (row.get("deliveryReport") != evidence["deliveredPath"] or
                row.get("input") != evidence["deliveredPath"]["mode"]):
            raise ValueError("GIH correlation API delivery record differs from captured evidence")
        resolution = row.get("resourceResolution", {})
        if (resolution.get("filename") != case["resourceProbe"]["filename"] or
                resolution.get("sha256OriginalFile") != case["resourceProbe"]["sha256"]):
            raise ValueError("GIH correlation recorded resource resolution differs")
        if row.get("resourceReadback") != evidence:
            raise ValueError("GIH correlation resource summary differs from original evidence")
    return True


def emit(case, evidence):
    metadata = {key: value for key, value in evidence.items() if key not in ("data", "deliveredPath")}
    def notice(kind, payload):
        text = json.dumps({"preset": case["preset"], "capture": kind, **payload}, separators=(",", ":"))
        if len(text) > 3900:
            raise ValueError("GIH correlation diagnostic notice exceeds transport budget")
        print("::notice title=GIH correlation source capture::" + text.replace("%", "%25").replace("\n", "%0A").replace("\r", "%0D"), flush=True)
    notice("metadata", metadata)
    for offset in range(0, len(evidence["data"]), 3000):
        notice("pixels", {"rect": evidence["rect"], "encoding": evidence["encoding"],
                          "offset": offset, "total": len(evidence["data"]),
                          "data": evidence["data"][offset:offset + 3000]})
