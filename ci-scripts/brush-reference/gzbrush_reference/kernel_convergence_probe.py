"""Public-color convergence diagnostics, independent of the brush renderer.

The candidate sample counts are hypotheses for selecting informative colors.
Nothing here selects a native sampling rule or changes a source fidelity gate.
"""
import hashlib
import json
import math
from xml.etree.ElementTree import fromstring

PROFILE = "sRGB-elle-V2-srgbtrc.icc"
QUALIFICATION = "research-only public Lab convergence; not a kernel stop rule"
BASES = (99, 100)
BATCH = 16
STEPS = 17
ENDPOINTS = (
    ("gray", (0, 0, 0), (255, 255, 255)),
    ("red-green", (255, 0, 0), (0, 255, 0)),
    ("blue-yellow", (0, 0, 255), (255, 255, 0)),
    ("near-lightness", (192, 32, 64), (32, 128, 192)),
)


def masks(fixtures):
    if fixtures.get("version") != "5.3.4.1" or fixtures.get("group") != "wetkernelcount":
        raise ValueError("Convergence diagnostics require the pinned kernel fixture group")
    selected = [case for case in fixtures.get("cases", [])
                if case.get("wetPattern") == "opaque-black-white-points"
                and case.get("diameter") == 64
                and case.get("overrides", {}).get("SmudgeRadiusValue") == "100"
                and case.get("wetKernelOffset") in (25, 75)]
    selected.sort(key=lambda case: (case["wetKernelOffset"], case["wetKernelComplement"]))
    if [(case["wetKernelOffset"], case.get("wetKernelComplement")) for case in selected] != [
            (25, False), (25, True), (75, False), (75, True)]:
        raise ValueError("Missing distinct kernel prefix/complement masks")
    for case in selected:
        points = case.get("wetKernelPoints", [])
        if (len(points) != case["wetKernelOffset"] or
                not all(isinstance(point, list) and len(point) == 2 and
                        all(type(value) is int and abs(value) <= 32 for value in point)
                        for point in points)):
            raise ValueError("Changed original kernel mask coordinates")
        if len({tuple(point) for point in points}) != len(points):
            raise ValueError("Duplicate original kernel mask coordinates")
    return selected


def plans(fixtures):
    result = []
    for case in masks(fixtures):
        count = len(case["wetKernelPoints"])
        complement = case["wetKernelComplement"]
        for pair, low, high in ENDPOINTS:
            for base in BASES:
                for step in range(STEPS):
                    denominator = base + step * BATCH
                    fraction = count / denominator
                    if complement:
                        fraction = 1 - fraction
                    rgb = [(a + (b - a) * fraction) / 255 for a, b in zip(low, high)]
                    result.append({"preset": case["preset"], "pointCount": count,
                                   "complement": complement, "pair": pair,
                                   "rgbLow": list(low), "rgbHigh": list(high),
                                   "baseCandidate": base, "batchIndex": step,
                                   "countCandidate": denominator, "inputRGB": rgb})
    return result


def snapshot(color):
    data = {"model": color.colorModel(), "depth": color.colorDepth(),
            "profile": color.colorProfile(), "ordered": list(color.componentsOrdered()),
            "components": list(color.components()), "xml": color.toXML()}
    if (len(data["xml"]) > 4096 or not all(
            len(data[key]) == 4 and all(isinstance(value, (int, float)) and math.isfinite(value)
                                       for value in data[key])
            for key in ("ordered", "components"))):
        raise ValueError("Invalid public convergence color payload")
    return data


def physical_lab(xml):
    if not isinstance(xml, str) or len(xml) > 4096 or "<!" in xml:
        raise ValueError("Invalid public Lab XML")
    root = fromstring(xml)
    if root.tag != "Color" or len(root) != 1 or root[0].tag != "Lab":
        raise ValueError("Public Lab physical channels unavailable")
    lab = [float(root[0].attrib[key]) for key in ("L", "a", "b")]
    if not all(math.isfinite(value) for value in lab):
        raise ValueError("Invalid public Lab physical channels")
    return lab


def capture(app, color_type, fixtures):
    profiles = {}
    for target in ("U16", "F32"):
        registered = sorted(app.profiles("LABA", target)) if target in app.colorDepths("LABA") else []
        if not registered:
            profiles[target] = {"supported": False}
            continue
        selected = color_type("LABA", target, "").colorProfile()
        if not 0 < len(registered) <= 32 or selected not in registered:
            raise ValueError("Registered default Lab profile unavailable")
        profiles[target] = {"supported": True, "registered": registered, "selected": selected}
    result = []
    for depth in ("U8", "U16"):
        if PROFILE not in app.profiles("RGBA", depth):
            raise ValueError("Pinned convergence source profile unavailable")
        for item in plans(fixtures):
            rgb = item["inputRGB"]
            for target in ("U16", "F32"):
                if not profiles[target]["supported"]:
                    continue
                color = color_type("RGBA", depth, PROFILE)
                color.setComponents([rgb[2], rgb[1], rgb[0], 1.])
                before = snapshot(color)
                if not color.setColorSpace("LABA", target, profiles[target]["selected"]):
                    raise ValueError("Public convergence Lab conversion failed")
                after = snapshot(color)
                result.append({"id": len(result), **item, "source": before, "lab": after,
                               "physicalLab": physical_lab(after["xml"])})
    tracks = {}
    for row in result:
        key = (row["preset"], row["pair"], row["baseCandidate"],
               row["source"]["depth"], row["lab"]["depth"])
        previous = tracks.get(key)
        row["previousCountCandidate"] = previous["countCandidate"] if previous else None
        row["deltaLab76FromPreviousBatch"] = math.sqrt(sum(
            (a-b)**2 for a, b in zip(row["physicalLab"], previous["physicalLab"]))) if previous else None
        tracks[key] = row
    report = {"schema": 1, "status": "complete", "qualification": QUALIFICATION,
              "runtimeVersion": app.version(), "labProfiles": profiles,
              "objective": "Measure public Lab distance between hypothetical cumulative means in 16-sample batches; infer no hidden source count",
              "records": result}
    validate(report, fixtures)
    return report


def validate(report, fixtures):
    if (report.get("schema"), report.get("status"), report.get("qualification")) != (1, "complete", QUALIFICATION):
        raise ValueError("Incomplete or misclassified kernel convergence observations")
    if report.get("runtimeVersion", "").split()[:1] != ["5.3.4.1"]:
        raise ValueError("Unpinned convergence runtime")
    profiles = report.get("labProfiles", {})
    for target in ("U16", "F32"):
        profile = profiles.get(target, {})
        if profile.get("supported") is False and set(profile) == {"supported"}:
            continue
        names = profile.get("registered", [])
        if (profile.get("supported") is not True or not 0 < len(names) <= 32 or
                names != sorted(set(names)) or profile.get("selected") not in names):
            raise ValueError("Invalid convergence target profile identity")
    expected = [(depth, item, target) for depth in ("U8", "U16") for item in plans(fixtures)
                for target in ("U16", "F32") if profiles[target]["supported"]]
    rows = report.get("records", [])
    if len(rows) != len(expected) or len(rows) > 2176:
        raise ValueError("Incomplete or oversized convergence matrix")
    tracks = {}
    for ordinal, (row, (depth, item, target)) in enumerate(zip(rows, expected)):
        if row.get("id") != ordinal or any(row.get(key) != value for key, value in item.items()):
            raise ValueError("Changed convergence inputs")
        for field, model, storage, profile in (
                ("source", "RGBA", depth, PROFILE),
                ("lab", "LABA", target, profiles[target]["selected"])):
            value = row.get(field, {})
            if (value.get("model"), value.get("depth"), value.get("profile")) != (model, storage, profile):
                raise ValueError("Changed convergence color identity")
            if (not isinstance(value.get("xml"), str) or len(value["xml"]) > 4096 or
                    any(len(value.get(key, [])) != 4 or
                        not all(isinstance(v, (int, float)) and math.isfinite(v) for v in value[key])
                        for key in ("ordered", "components"))):
                raise ValueError("Invalid convergence raw color payload")
        source = row["source"]
        quantum = 1 / (255 if depth == "U8" else 65535)
        if any(abs(a-b) > quantum + 1e-6 for a, b in zip(source["ordered"], item["inputRGB"] + [1.])):
            raise ValueError("Convergence source components changed")
        if source["components"] != source["ordered"][:3][::-1] + source["ordered"][3:]:
            raise ValueError("Convergence source storage order changed")
        if row.get("physicalLab") != physical_lab(row["lab"]["xml"]):
            raise ValueError("Convergence physical Lab channels changed")
        key = (row["preset"], row["pair"], row["baseCandidate"], depth, target)
        previous = tracks.get(key)
        expected_count = previous["countCandidate"] if previous else None
        expected_delta = math.sqrt(sum((a-b)**2 for a, b in zip(
            row["physicalLab"], previous["physicalLab"]))) if previous else None
        if (row.get("previousCountCandidate") != expected_count or
                row.get("deltaLab76FromPreviousBatch") != expected_delta):
            raise ValueError("Convergence batch observation changed")
        tracks[key] = row


def render(app, output, fixtures):
    from krita import ManagedColor
    report = capture(app, ManagedColor, fixtures)
    if capture(app, ManagedColor, fixtures) != report:
        raise ValueError("Public convergence conversion repeat changed")
    precision = output / "kernel-precision.json"
    if precision.is_file():
        data = precision.read_bytes()
        observed = json.loads(data)
        if observed.get("status") != "complete" or len(observed.get("records", [])) != 73:
            raise ValueError("Incomplete original kernel precision observations")
        report["kernelPrecisionSha256"] = hashlib.sha256(data).hexdigest()
    (output / "kernel-convergence.json").write_text(json.dumps(report, indent=2))
    print("::notice::" + json.dumps({"capture": "public-kernel-convergence",
        "qualification": QUALIFICATION, "records": len(report["records"]),
        "runtimeVersion": report["runtimeVersion"], "labProfiles": report["labProfiles"],
        "kernelPrecisionSha256": report.get("kernelPrecisionSha256")}, separators=(",", ":")))
