"""Fixed masked-source lifecycle diagnostics; original parity gates stay separate."""
import copy
import hashlib
import json
import math
from pathlib import Path
import shutil
from xml.etree.ElementTree import ParseError, fromstring

TARGET = 39
REPETITIONS = 8
# Public Qt 5 QEvent::Type values, fixed independently of observed evidence:
# https://github.com/qt/qtbase/blob/v5.15.2/src/corelib/kernel/qcoreevent.h
TABLET_MOVE, TABLET_PRESS, TABLET_RELEASE = 87, 92, 93
TABLET_ENTER, TABLET_LEAVE = 171, 172
# node policy, worker limit, resource policy, isolated case, target API, translation
GROUPS = {
    "masklife-reused": ("reused", None, "unchanged", False, "Node.paintLine", (0, 0)),
    "masklife-freshnode": ("fresh", None, "unchanged", False, "Node.paintLine", (0, 0)),
    "masklife-single-reused": ("reused", 1, "unchanged", False, "Node.paintLine", (0, 0)),
    "masklife-single-freshnode": ("fresh", 1, "unchanged", False, "Node.paintLine", (0, 0)),
    "masklife-reselect": ("reused", None, "reselect-per-stroke", False, "Node.paintLine", (0, 0)),
    "masklife-isolated": ("reused", None, "unchanged", True, "Node.paintLine", (0, 0)),
    "masklife-tablet": ("reused", None, "unchanged", False, "Qt tablet events", (0, 0)),
    "masklife-isolated-tablet": ("reused", None, "unchanged", True, "Qt tablet events", (0, 0)),
    "masklife-offset-xminus1": ("reused", None, "unchanged", False, "Node.paintLine", (-1, 0)),
    "masklife-offset-xplus1": ("reused", None, "unchanged", False, "Node.paintLine", (1, 0)),
    "masklife-offset-xplus32": ("reused", None, "unchanged", False, "Node.paintLine", (32, 0)),
    "masklife-offset-yplus1": ("reused", None, "unchanged", False, "Node.paintLine", (0, 1)),
}


def contract(group):
    if group not in GROUPS:
        return None
    node, workers, resources, isolated, api, translation = GROUPS[group]
    return {"version": 2, "sourceGroup": "maskwash", "indices": [TARGET] if isolated else list(range(TARGET + 1)),
            "targetIndex": TARGET, "repetitions": REPETITIONS, "historyRepetitions": 2, "nodePolicy": node,
            "configuredMaxThreads": workers, "resourcePolicy": resources,
            "targetDelivery": api, "targetTranslation": list(translation),
            "sourceParityQualified": False,
            "historyPolicy": "isolated target" if isolated else "all original cases 000-039 retained and checked"}


def _tablet_events(start, end):
    return [{"x": start[0] + (end[0] - start[0]) * index / 32,
             "y": start[1] + (end[1] - start[1]) * index / 32,
             "pressure": 1, "tiltX": 0, "tiltY": 0, "rotation": 0,
             "timeUs": round(index * 1000000 / 240)} for index in range(33)]


def prepare(root, resources, preset_writer, group):
    from masked_fixtures import prepare as prepare_masked
    diagnostic = contract(group)
    if diagnostic is None:
        raise ValueError("Unknown mask lifecycle diagnostic profile")
    prepare_masked(root, resources, preset_writer, "maskwash")
    path = root / "output" / "fixtures.json"
    baseline = json.loads(path.read_text())
    cases = []
    for index in diagnostic["indices"]:
        case = copy.deepcopy(baseline["cases"][index])
        case["repeatDiagnostic"] = diagnostic
        mask_probe = copy.deepcopy(case["maskProbe"])
        target = index == TARGET
        translation = diagnostic["targetTranslation"] if target else [0, 0]
        for key in ("start", "end"):
            case[key] = [value + delta for value, delta in zip(case[key], translation)]
        rect = mask_probe["rect"]
        mask_probe["rect"] = [rect[0] + translation[0], rect[1] + translation[1], rect[2], rect[3]]
        case["maskProbe"] = mask_probe
        case["delivery"] = diagnostic["targetDelivery"] if target else "Node.paintLine"
        case["maskRepeatProbe"] = {"version": 1, "group": group, "originalIndex": index,
                                  "role": "target" if target else "retained original history",
                                  "translation": translation, "maskProbe": mask_probe}
        if case["delivery"] == "Qt tablet events":
            # The ordinary mask helper explicitly qualifies Node endpoint calls.
            # This alternate input has its own exact delivered-contact validation.
            case.pop("maskProbe")
            # Match the Node control's constant-full-pressure source modifier;
            # actual Qt contacts still carry and retain pressure=1 explicitly.
            case["variant"] = "baseline pressure-flag false control"
            case["events"] = _tablet_events(case["start"], case["end"])
            case["canvasCenter"] = [(case["start"][i] + case["end"][i]) / 2 for i in range(2)]
        cases.append(case)
    manifest = {**baseline, "group": group, "cases": cases,
                "presets": [case["preset"] for case in cases], "repeatDiagnostic": diagnostic,
                "input": "Fixed lifecycle controls; original KPP bytes, all first/repeat captures retained"}
    path.write_text(json.dumps(manifest, indent=2))
    destination = resources / "pykrita" / "gzbrush_reference"
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(Path(__file__), destination / Path(__file__).name)
    if diagnostic["configuredMaxThreads"] == 1:
        config = root / "config" / "kritarc"
        config.write_text("maxNumberOfThreads=1\n" + config.read_text())


def validate(case):
    spec = case.get("maskRepeatProbe", {})
    diagnostic = contract(spec.get("group"))
    index = spec.get("originalIndex")
    if (set(spec) != {"version", "group", "originalIndex", "role", "translation", "maskProbe"} or
            diagnostic is None or case.get("repeatDiagnostic") != diagnostic or
            type(index) is not int or index not in diagnostic["indices"] or
            case.get("preset") != "GZ Original dry maskwash " + str(index).zfill(3)):
        raise ValueError("Mask lifecycle identity differs from fixed diagnostic contract")
    target = index == TARGET
    translation = diagnostic["targetTranslation"] if target else [0, 0]
    expected_api = diagnostic["targetDelivery"] if target else "Node.paintLine"
    if (spec.get("version") != 1 or spec.get("translation") != translation or
            spec.get("role") != ("target" if target else "retained original history") or
            case.get("start") != [896 + translation[0], 1024 + translation[1]] or
            case.get("end") != [1152 + translation[0], 1024 + translation[1]] or
            case.get("delivery") != expected_api or case.get("background") != "transparent" or
            case.get("diameter") != 24 or case.get("pressure") != [1, 1] or
            case.get("opacity") != 1 or case.get("flow") != 1):
        raise ValueError("Mask lifecycle endpoints or source controls changed")
    expected_rect = [864 + translation[0], 992 + translation[1], 320, 64]
    if spec.get("maskProbe", {}).get("rect") != expected_rect:
        raise ValueError("Mask lifecycle capture rectangle changed")
    if expected_api == "Qt tablet events":
        if case.get("variant") != "baseline pressure-flag false control":
            raise ValueError("Paired lifecycle tablet pressure modifier changed")
        if case.get("events") != _tablet_events(case["start"], case["end"]):
            raise ValueError("Mask lifecycle authored tablet contacts changed")
    elif case.get("events") is not None:
        raise ValueError("Mask lifecycle Node profile acquired a tablet trace")
    return diagnostic


def validate_manifest(manifest):
    diagnostic = contract(manifest.get("group"))
    cases = manifest.get("cases", [])
    if (diagnostic is None or manifest.get("repeatDiagnostic") != diagnostic or
            len(cases) != len(diagnostic["indices"]) or
            manifest.get("presets") != ["GZ Original dry maskwash " + str(i).zfill(3)
                                        for i in diagnostic["indices"]]):
        raise ValueError("Mask lifecycle manifest dropped or reordered fixed original history")
    for case, index in zip(cases, diagnostic["indices"]):
        validate(case)
        if case["maskRepeatProbe"]["originalIndex"] != index:
            raise ValueError("Mask lifecycle original history index changed")
    return diagnostic


def repeat_count(case):
    """Closed per-case rule: exact two-stroke history, eight target captures."""
    diagnostic = validate(case)
    return (diagnostic["repetitions"] if case["maskRepeatProbe"]["originalIndex"] == TARGET
            else diagnostic["historyRepetitions"])


def _finite_number(value):
    # Bound before arithmetic; bool/string, NaN/Inf and arbitrarily large JSON
    # integers must reject cleanly instead of truncating zip or raising TypeError.
    return type(value) in (int, float) and -1e9 <= value <= 1e9 and math.isfinite(value)


def _tablet_fields(actual, authored, pressure, origin):
    if (not isinstance(actual, dict) or
            type(actual.get("timeMs")) is not int or
            not 0 <= actual["timeMs"] < 2 ** 64 or
            not isinstance(actual.get("image"), list) or len(actual["image"]) != 2 or
            not isinstance(actual.get("tilt"), list) or len(actual["tilt"]) != 2):
        raise ValueError("Malformed mask lifecycle tablet sample")
    observed = (*actual["image"], actual.get("pressure"), *actual["tilt"], actual.get("rotation"))
    expected = (authored["x"], authored["y"], pressure, 0, 0, 0)
    if any(not _finite_number(value) or abs(value - wanted) > 1e-7
           for value, wanted in zip(observed, expected)):
        raise ValueError("Mask lifecycle tablet contact values changed")
    if actual["timeMs"] - origin != round(authored["timeUs"] / 1000):
        raise ValueError("Mask lifecycle relative tablet clock changed")


def validate_delivery(case, delivery, *, contacts_only=False):
    """Validate the source's full trace, or the cache's retained contacts.

    Both paths use fixed public Qt enum values and require one press followed
    by exactly 32 moves. Cache evidence cannot define its own accepted kinds.
    """
    validate(case)
    if not isinstance(delivery, dict) or delivery.get("mode") != case["delivery"]:
        raise ValueError("Mask lifecycle input API differs")
    events = delivery.get("events")
    if case["delivery"] == "Node.paintLine":
        expected = [{"type": "line-start", "position": case["start"], "pressure": 1},
                    {"type": "line-end", "position": case["end"], "pressure": 1}]
        if (not isinstance(events, list) or len(events) != 2 or events != expected or
                any(not isinstance(event, dict) or
                    not isinstance(event.get("position"), list) or len(event["position"]) != 2 or
                    any(not _finite_number(value) for value in event["position"]) or
                    not _finite_number(event.get("pressure")) for event in events)):
            raise ValueError("Mask lifecycle Node endpoints changed")
        return events
    kinds = [TABLET_PRESS] + [TABLET_MOVE] * 32
    if not contacts_only:
        kinds = [TABLET_ENTER, TABLET_MOVE] + kinds + [TABLET_RELEASE, TABLET_LEAVE]
    if (not isinstance(events, list) or len(events) != len(kinds) or
            any(not isinstance(event, dict) or type(event.get("type")) is not int or
                event["type"] != kind for event, kind in zip(events, kinds))):
        raise ValueError("Mask lifecycle tablet event count or kind sequence changed")
    contacts = events if contacts_only else events[2:-2]
    origin = contacts[0].get("timeMs")
    if type(origin) is not int or not 0 <= origin < 2 ** 64:
        raise ValueError("Malformed mask lifecycle tablet origin clock")
    for authored, actual in zip(case["events"], contacts):
        _tablet_fields(actual, authored, 1, origin)
    if not contacts_only:
        for actual, authored in ((events[0], case["events"][0]),
                                 (events[1], case["events"][0]),
                                 (events[-2], case["events"][-1]),
                                 (events[-1], case["events"][-1])):
            _tablet_fields(actual, authored, 0, origin)
    return contacts


def controls(case, fields):
    validate(case)
    actual = {}
    for key, wanted in case["overrides"].items():
        observed = fields.get(key)
        if key.endswith("brush_definition"):
            try:
                requested, effective = fromstring(wanted), fromstring(observed or "")
            except ParseError as error:
                raise ValueError("Malformed effective mask lifecycle tip XML") from error
            for tag in ("Brush", "MaskGenerator"):
                a = effective if tag == "Brush" else effective.find(tag)
                b = requested if tag == "Brush" else requested.find(tag)
                if a is None or b is None or a.tag != b.tag:
                    raise ValueError("Mask lifecycle tip family changed")
                for attribute, value in b.attrib.items():
                    if a.get(attribute) != value:
                        try:
                            valid = math.isfinite(float(a.get(attribute))) and float(a.get(attribute)) == float(value)
                        except (TypeError, ValueError):
                            valid = False
                        if not valid:
                            raise ValueError("Mask lifecycle tip geometry changed: " + attribute)
        elif observed != wanted:
            raise ValueError("Mask lifecycle effective setting changed: " + key)
        actual[key] = observed
    try:
        primary = fromstring(fields.get("brush_definition", ""))
    except ParseError as error:
        raise ValueError("Malformed effective primary lifecycle tip XML") from error
    mask = primary.find("MaskGenerator")
    if (primary.tag != "Brush" or primary.get("type") != "auto_brush" or mask is None or
            mask.get("type") != "circle" or float(mask.get("diameter", "nan")) != 24 or
            mask.get("antialiasEdges") != "0" or
            float(primary.get("spacing", "nan")) != case["spacing"]):
        raise ValueError("Mask lifecycle primary geometry changed")
    return {"effectiveControls": actual, "effectivePrimaryBrush": fields["brush_definition"]}


def _alpha_runs(raw, width, y, left, right):
    result = []
    begin = left
    previous = raw[(y * width + left) * 4 + 3]
    for x in range(left + 1, right + 1):
        value = raw[(y * width + x) * 4 + 3] if x < right else None
        if value != previous:
            result.append([begin, x - 1, previous])
            begin, previous = x, value
    return result


def capture(case, fields, delivery, raw, width, initial):
    diagnostic = validate(case)
    evidence = controls(case, fields)
    contacts = validate_delivery(case, delivery)
    if width != 2048 or len(raw) != width * width * 4 or len(initial) != len(raw) or any(initial):
        raise ValueError("Mask lifecycle raster differs from fixed transparent RGBA8 canvas")
    rect = case["maskRepeatProbe"]["maskProbe"]["rect"]
    left, top, w, h = rect
    pixels = b"".join(raw[(y * width + left) * 4:(y * width + left + w) * 4] for y in range(top, top + h))
    points = [(i % w + left, i // w + top) for i in range(w * h) if pixels[i * 4 + 3]]
    for y in range(width):
        line = raw[y * width * 4:(y + 1) * width * 4]
        if top <= y < top + h:
            if any(line[3:left * 4:4]) or any(line[(left + w) * 4 + 3::4]):
                raise ValueError("Mask lifecycle paint leaves bounded capture region")
        elif any(line[3::4]):
            raise ValueError("Mask lifecycle paint leaves bounded capture region")
    bounds = ([min(x for x, y in points), min(y for x, y in points),
               max(x for x, y in points) - min(x for x, y in points) + 1,
               max(y for x, y in points) - min(y for x, y in points) + 1] if points else None)
    return {**evidence, "inputApi": case["delivery"], "deliveredContacts": contacts,
            "deliveryValuesMatchAuthored": True, "repeatDiagnostic": diagnostic,
            "rect": rect, "nonzeroAlphaPixels": len(points), "alphaBounds": bounds,
            "alphaRunsAtStrokeY": _alpha_runs(raw, width, case["start"][1], left, left + w),
            "sha256Pixels": hashlib.sha256(pixels).hexdigest(), "sourceParityQualified": False}


def difference_summary(first, repeated, width, rect):
    if (type(width) is not int or width != 2048 or len(first) != width * width * 4 or
            len(repeated) != len(first) or len(rect) != 4 or
            any(type(value) is not int for value in rect)):
        raise ValueError("Invalid lifecycle difference raster geometry")
    left, top, w, h = rect
    if w != 320 or h != 64 or left < 0 or top < 0 or left + w > width or top + h > width:
        raise ValueError("Invalid lifecycle difference capture rectangle")
    changed, transitions, columns = [], {}, {}
    for y in range(top, top + h):
        for x in range(left, left + w):
            index = (y * width + x) * 4
            a, b = first[index:index + 4], repeated[index:index + 4]
            if a == b or not (a[3] or b[3]):
                continue
            changed.append((x, y))
            key = (a[3], b[3])
            transitions[key] = transitions.get(key, 0) + 1
            columns[x // 64] = columns.get(x // 64, 0) + 1
    bounds = ([min(x for x, y in changed), min(y for x, y in changed),
               max(x for x, y in changed) - min(x for x, y in changed) + 1,
               max(y for x, y in changed) - min(y for x, y in changed) + 1] if changed else None)
    return {"differenceBounds": bounds, "visibleOrAlphaChangedPixelsInRect": len(changed),
            "alphaTransitions": [[a, b, count] for (a, b), count in sorted(transitions.items())],
            "documentGrid64Columns": [[column, count] for column, count in sorted(columns.items())],
            "firstChangedXModulo64": bounds[0] % 64 if bounds else None,
            "interpretation": "recorded document-grid observations; no claim that readback proves engine scheduler state"}
