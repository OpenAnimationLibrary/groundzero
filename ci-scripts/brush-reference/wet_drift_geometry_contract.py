"""Bounded original observations for Smear raster placement; no parity model."""
import hashlib
import json
import math
from xml.etree.ElementTree import ParseError, fromstring

WIDTH = HEIGHT = 2048
MODELS = ("dry", "legacy-smear", "new-smear")


def _subdivide(vertices):
    result = [vertices[0]]
    for start, end in zip(vertices, vertices[1:]):
        distance = abs(end[0] - start[0]) + abs(end[1] - start[1])
        steps = max(1, math.ceil(distance / 16))
        for index in range(1, steps + 1):
            fraction = index / steps
            result.append([start[0] + (end[0] - start[0]) * fraction,
                           start[1] + (end[1] - start[1]) * fraction])
    return result


def plans():
    """Separate authored path factors without assuming an internal dab lattice."""
    turns = [[256, 328], [768, 328], [768, 344],
             [256, 344], [256, 360], [769, 360]]
    return [
        {"id": "east-y40", "vertices": [[256, 40], [769, 40]]},
        {"id": "east-y328", "vertices": [[256, 328], [769, 328]]},
        {"id": "east-y968", "vertices": [[256, 968], [769, 968]]},
        {"id": "east-y328-minus-eighth", "vertices": [[256, 327.875], [769, 327.875]]},
        {"id": "east-y328-plus-eighth", "vertices": [[256, 328.125], [769, 328.125]]},
        {"id": "west-y328", "vertices": [[768, 328], [255, 328]]},
        {"id": "south-x328", "vertices": [[328, 256], [328, 769]]},
        {"id": "turns-coarse", "vertices": turns},
        {"id": "turns-subdivided", "vertices": _subdivide(turns)},
        {"id": "turns-transposed", "vertices": [[y, x] for x, y in turns]},
    ]


def centers(vertices):
    """Authored 16px observation windows, not a source-internal dab counter."""
    distance = 0.0
    next_sample = 0.0
    result = []
    for first, last in zip(vertices, vertices[1:]):
        length = math.hypot(last[0] - first[0], last[1] - first[1])
        while next_sample <= distance + length:
            fraction = (next_sample - distance) / length
            result.append([first[0] + (last[0] - first[0]) * fraction,
                           first[1] + (last[1] - first[1]) * fraction])
            next_sample += 16
        distance += length
    return result


def validate(case):
    spec = case.get("wetGeometryProbe", {})
    plan = next((row for row in plans() if row["id"] == spec.get("pathId")), None)
    model = spec.get("model")
    if (plan is None or model not in MODELS or
            case.get("sourcePath") != plan["vertices"] or
            case.get("sourceEngine") != ("paintbrush" if model == "dry" else "colorsmudge") or
            case.get("diameter") != 8 or case.get("spacing") != 2 or
            case.get("opacity") != 1 or case.get("flow") != 1 or
            case.get("pressure") != [1, 1] or
            case.get("foregroundRGBA8") != [0, 255, 0, 255] or
            case.get("background") != "wet-halves" or case.get("wetBoundary") != WIDTH):
        raise ValueError("Wet geometry case differs from bounded authored contract")
    for point in plan["vertices"]:
        if not all(math.isfinite(value) and 16 <= value < WIDTH - 16 for value in point):
            raise ValueError("Wet geometry path leaves bounded document interior")
    observed_centers = centers(plan["vertices"])
    if len(observed_centers) > 128:
        raise ValueError("Wet geometry observation budget exceeded")
    return plan["vertices"]


def controls(case, fields):
    validate(case)
    model = case["wetGeometryProbe"]["model"]
    expected = {
        "paintop": case["sourceEngine"], "CompositeOp": "normal",
        "PaintOpAction": "1", "OpacityValue": "1", "FlowValue": "1",
        "OpacityUseCurve": "false", "FlowUseCurve": "false",
        "PressureSize": "false", "PressureOpacity": "false", "PressureFlow": "false",
        "PressureRatio": "false", "PressureRotation": "false",
        "PressureSpacing": "false", "PressureScatter": "false",
        "Texture/Pattern/Enabled": "false", "Spacing/Isotropic": "false",
        "PressureMirror": "false", "Pressureh": "false",
        "Pressures": "false", "Pressurev": "false", "MergedPaint": "false",
        "PaintOpSettings/isAirbrushing": "false", "PaintOpSettings/ignoreSpacing": "false",
        "PaintOpSettings/updateSpacingBetweenDabs": "false",
    }
    if model != "dry":
        expected.update({"SmudgeRateMode": "0", "SmudgeRateSmearAlpha": "true",
                         "SmudgeRateUseNewEngine": str(model == "new-smear").lower(),
                         "PressureSmudgeRate": "true", "SmudgeRateUseCurve": "false",
                         "PressureColorRate": "true", "ColorRateUseCurve": "false",
                         "SmudgeRadiusUseCurve": "false"})
    for key, expected_value in expected.items():
        if fields.get(key) != expected_value:
            raise ValueError("Effective wet geometry setting differs: " + key)
    numeric = {"SmudgeRateValue": .5, "ColorRateValue": .25, "SmudgeRadiusValue": 0}
    if model != "dry":
        for key, expected_value in numeric.items():
            observed = float(fields.get(key, "nan"))
            if not math.isfinite(observed) or observed != expected_value:
                raise ValueError("Effective wet geometry scalar differs: " + key)
    try:
        brush = fromstring(fields.get("brush_definition", ""))
    except ParseError as error:
        raise ValueError("Malformed effective wet geometry brush XML") from error
    generators = brush.findall("MaskGenerator")
    expected_brush = {"type": "auto_brush", "BrushVersion": "2", "useAutoSpacing": "0"}
    if (brush.tag != "Brush" or len(brush) != 1 or len(generators) != 1 or
            len(generators[0]) or
            any(brush.attrib.get(k) != v for k, v in expected_brush.items())):
        raise ValueError("Effective wet geometry brush family differs")
    for key, expected_value in {"spacing": 2, "angle": 0, "randomness": 0, "density": 1}.items():
        observed = float(brush.attrib.get(key, "nan"))
        if not math.isfinite(observed) or observed != expected_value:
            raise ValueError("Effective wet geometry brush scalar differs: " + key)
    generator = generators[0].attrib
    for key, value in {"id": "default", "type": "circle", "antialiasEdges": "0", "spikes": "2"}.items():
        if generator.get(key) != value:
            raise ValueError("Effective wet geometry generator differs: " + key)
    for key, value in {"diameter": 8, "ratio": 1, "hfade": 1, "vfade": 1}.items():
        if float(generator.get(key, "nan")) != value:
            raise ValueError("Effective wet geometry tip geometry differs: " + key)
    return {"effectiveControls": {key: fields[key] for key in expected},
            "effectiveWetScalars": {key: fields.get(key) for key in numeric},
            "effectiveBrushDefinition": fields["brush_definition"]}


def capture(before, after, case):
    validate(case)
    if len(before) != WIDTH * HEIGHT * 4 or len(after) != len(before):
        raise ValueError("Incomplete wet geometry source raster")
    # A nonzero green channel isolates paint from the original opaque-blue canvas.
    if before != bytes((255, 0, 0, 255)) * (WIDTH * HEIGHT):
        raise ValueError("Wet geometry initial canvas differs from opaque blue")
    windows = centers(case["sourcePath"])
    observations = []
    for index, center in enumerate(windows):
        cx, cy = center
        left, top = math.floor(cx) - 6, math.floor(cy) - 6
        pixels = b"".join(after[(y * WIDTH + left) * 4:(y * WIDTH + left + 13) * 4]
                          for y in range(top, top + 13))
        points = [(offset % 13, offset // 13)
                  for offset in range(169) if pixels[offset * 4 + 1]]
        bounds = ([min(x for x, y in points) + left,
                   min(y for x, y in points) + top,
                   max(x for x, y in points) - min(x for x, y in points) + 1,
                   max(y for x, y in points) - min(y for x, y in points) + 1]
                  if points else None)
        observations.append({"index": index, "authoredCenter": center,
                             "greenBounds": bounds, "greenPixels": len(points),
                             "sha256WindowBGRA8": hashlib.sha256(pixels).hexdigest()})
    if not any(row["greenPixels"] for row in observations):
        raise ValueError("Wet geometry source produced no green paint in authored windows: " +
                         case["preset"])
    return {"model": case["wetGeometryProbe"]["model"],
            "pathId": case["wetGeometryProbe"]["pathId"],
            "observationMeaning": "authored document windows; source internal dab coordinates/count unavailable",
            "windows": observations, "sourceParityQualified": False}


def emit(case, evidence):
    # Keep annotations bounded independently of the complete saved PNGs.
    for offset in range(0, len(evidence["windows"]), 6):
        row = {"preset": case["preset"], "capture": "wet-geometry-windows",
               "model": evidence["model"], "pathId": evidence["pathId"],
               "offset": offset, "total": len(evidence["windows"]),
               "sourceParityQualified": False, "windows": evidence["windows"][offset:offset + 6]}
        print("::notice::" + json.dumps(row, separators=(",", ":")), flush=True)
