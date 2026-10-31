"""Public ManagedColor conversion observations; no brush renderer assumptions."""
import json
import math

PROFILE = "sRGB-elle-V2-srgbtrc.icc"
QUALIFICATION = "research-only public color conversion; not a kernel stop rule"


def inputs():
    values = [("gray-" + str(v), [v / 255.] * 3) for v in (0, 32, 64, 72, 128, 194, 210, 255)]
    values += [("gray-mean-72-" + str(n), [72 / n] * 3) for n in (210, 194)]
    values += [(name, rgb) for name, rgb in (("red", [1,0,0]), ("green", [0,1,0]), ("blue", [0,0,1]))]
    for pair, (low, high) in enumerate((([255,0,0], [0,255,0]),
                                      ([0,0,255], [255,255,0]),
                                      ([192,32,64], [32,128,192]))):
        for n in (210,194):
            values.append(("chromatic-mean-" + str(pair) + "-72-" + str(n),
                           [(a + (b-a)*72/n)/255 for a,b in zip(low,high)]))
    return values


def snapshot(color):
    result = {"model": color.colorModel(), "depth": color.colorDepth(),
              "profile": color.colorProfile(), "components": list(color.components()),
              "ordered": list(color.componentsOrdered()),
              "xml": color.toXML(), "display": color.toQString()}
    for key in ("components", "ordered"):
        if len(result[key]) != 4 or not all(math.isfinite(v) for v in result[key]):
            raise ValueError("Invalid public color components")
    if len(result["xml"]) > 4096 or len(result["display"]) > 1024:
        raise ValueError("Oversized public color representation")
    return result


def capture(app, color_type):
    records, profiles = [], {}
    for target in ("U16", "F32"):
        if target not in app.colorDepths("LABA"):
            profiles[target] = {"supported": False, "reason": "depth not registered"}
            continue
        available = sorted(app.profiles("LABA", target))
        if not available:
            profiles[target] = {"supported": False, "reason": "no registered profiles"}
            continue
        default = color_type("LABA", target, "")
        profile = default.colorProfile()
        if not profile or profile not in available or len(available) > 32:
            raise ValueError("Registered default Lab profile unavailable")
        profiles[target] = {"supported": True, "selected": profile, "registered": available}
    for depth in ("U8", "U16"):
        if PROFILE not in app.profiles("RGBA", depth):
            raise ValueError("Exact reference RGB profile unavailable")
        for name, rgb in inputs():
            for target in ("U16", "F32"):
                if not profiles[target]["supported"]:
                    continue
                color = color_type("RGBA", depth, PROFILE)
                # The integer RGBA API uses B,G,R,A storage. Read back both
                # representations to verify that assumption on this runtime.
                color.setComponents([rgb[2], rgb[1], rgb[0], 1.])
                before = snapshot(color)
                quantum = 1 / (255 if depth == "U8" else 65535)
                if before["model"] != "RGBA" or before["depth"] != depth or before["profile"] != PROFILE:
                    raise ValueError("Source color identity changed")
                if any(abs(a-b) > quantum + 1e-6 for a,b in zip(before["ordered"], rgb+[1.])):
                    raise ValueError("RGB display order or normalization differs")
                if before["components"] != list(reversed(before["ordered"][:3])) + before["ordered"][3:]:
                    raise ValueError("RGB storage order differs")
                profile = profiles[target]["selected"]
                if not color.setColorSpace("LABA", target, profile):
                    raise ValueError("Public Lab conversion failed")
                after = snapshot(color)
                if (after["model"], after["depth"], after["profile"]) != ("LABA",target,profile):
                    raise ValueError("Target Lab identity differs")
                records.append({"id": len(records), "name": name, "inputRGB": rgb,
                                "source": before, "lab": after})
    report = {"schema": 1, "status": "complete", "qualification": QUALIFICATION,
              "runtimeVersion": app.version(), "labProfiles": profiles, "records": records,
              "researchStatus": "available" if profiles["U16"]["supported"] else "unsupported-U16"}
    validate(report)
    return report


def validate(report):
    if (report.get("schema"), report.get("status"), report.get("qualification")) != (1,"complete",QUALIFICATION):
        raise ValueError("Incomplete public Lab observations")
    if report.get("runtimeVersion", "").split()[:1] != ["5.3.4.1"]:
        raise ValueError("Unpinned public Lab runtime")
    profiles = report.get("labProfiles", {})
    for target in ("U16", "F32"):
        profile = profiles.get(target, {})
        if profile.get("supported") is False:
            if set(profile) != {"supported", "reason"} or profile["reason"] not in ("depth not registered", "no registered profiles"):
                raise ValueError("Invalid unsupported Lab depth observation")
            continue
        names = profile.get("registered", [])
        if (profile.get("supported") is not True or not 0 < len(names) <= 32 or
            not all(isinstance(name,str) and 0 < len(name) <= 256 for name in names) or
            names != sorted(set(names)) or profile.get("selected") not in names):
            raise ValueError("Invalid registered public Lab profile")
    if report.get("researchStatus") != ("available" if profiles["U16"]["supported"] else "unsupported-U16"):
        raise ValueError("Misclassified Lab availability")
    expected = [(depth,name,rgb,target) for depth in ("U8","U16")
                for name,rgb in inputs() for target in ("U16","F32") if profiles[target]["supported"]]
    records = report.get("records", [])
    if len(records) != len(expected):
        raise ValueError("Incomplete public Lab matrix")
    for i, (row, (depth,name,rgb,target)) in enumerate(zip(records,expected)):
        selected = report["labProfiles"][target]["selected"]
        if row.get("id") != i or row.get("name") != name or row.get("inputRGB") != rgb:
            raise ValueError("Changed public Lab input")
        for key,model,space,profile in (("source","RGBA",depth,PROFILE),("lab","LABA",target,selected)):
            value = row[key]
            if (value["model"],value["depth"],value["profile"]) != (model,space,profile):
                raise ValueError("Changed public Lab color identity")
            for field in ("components","ordered"):
                if len(value[field]) != 4 or not all(isinstance(v,(int,float)) and math.isfinite(v) for v in value[field]):
                    raise ValueError("Invalid public Lab channel payload")
            if not isinstance(value["xml"],str) or not 0 < len(value["xml"]) <= 4096 or not isinstance(value["display"],str) or len(value["display"]) > 1024:
                raise ValueError("Invalid public Lab serialization")
        source = row["source"]
        quantum = 1 / (255 if depth == "U8" else 65535)
        if any(abs(a-b) > quantum + 1e-6 for a,b in zip(source["ordered"],rgb+[1.])):
            raise ValueError("Changed source channel observation")
        if source["components"] != source["ordered"][:3][::-1]+source["ordered"][3:]:
            raise ValueError("Changed source storage observation")



def render(app, output):
    from krita import ManagedColor
    report = capture(app, ManagedColor)
    if capture(app, ManagedColor) != report:
        raise ValueError("Public Lab repeat changed")
    print("::notice::" + json.dumps({"capture":"public-managed-lab-identity",
        **{key:value for key,value in report.items() if key != "records"}}, separators=(",",":")))
    (output / "managed-lab.json").write_text(json.dumps(report, indent=2))
    for row in report["records"]:
        print("::notice::" + json.dumps({"capture":"public-managed-lab", **row}, separators=(",",":")))
