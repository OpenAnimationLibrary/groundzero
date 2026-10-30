"""Original joint-channel and Node-pressure acquisitions; no assumed HSV order."""
import json


def prepare(root, resources, preset_writer):
    cases, names = [], []
    plans = [
        ("hs", (.2, .8, .5), False),
        ("hv", (.8, .5, .2), False),
        ("sv", (.5, .2, .8), False),
        ("hsv", (.2, .8, .2), False),
        ("hsv", (.8, .2, .8), False),
        ("hsv", (.5, .5, .5), False),
        ("h", (.5, .5, .5), True),
        ("s", (.5, .5, .5), True),
        ("v", (.5, .5, .5), True),
    ]
    for color in ([173, 91, 47, 255], [59, 137, 191, 255]):
        for channels, values, dynamic in plans:
            for enabled in (False, True):
                name = "GZ Original dry hsvjoint " + str(len(cases)).zfill(3)
                overrides = {"Spacing/Isotropic": "false"}
                for channel, value in zip("hsv", values):
                    curve = "0,0;1,1;" if dynamic and channel in channels else "0," + str(value) + ";1," + str(value) + ";"
                    overrides.update({
                        "Pressure" + channel: str(enabled and channel in channels).lower(),
                        channel + "UseCurve": "true", channel + "UseSameCurve": "true",
                        channel + "Value": "1", channel + "curveMode": "0",
                        channel + "commonCurve": curve,
                        channel + "Sensor": '<params id="pressure"><curve>' + curve + '</curve></params>',
                    })
                (resources / "paintoppresets" / (name + ".kpp")).write_bytes(
                    preset_writer(name, "dry", False, overrides, 24, 1, 0))
                names.append(name)
                cases.append({
                    "preset": name, "variant": "baseline", "diameter": 24, "spacing": 1,
                    "pressure": [.2, .8] if dynamic else [1, 1], "opacity": 1, "flow": 1,
                    "foregroundRGBA8": color, "start": [896, 1024], "end": [1152, 1024],
                    "background": "transparent", "overrides": overrides, "expectUnchanged": False,
                    "delivery": "Node.paintLine", "canvasCenter": [1024, 1024],
                    "jointHsvProbe": {"channels": channels, "dynamicPressure": dynamic,
                                      "enabled": enabled, "operationOrderAssumed": False},
                    "sensorProbe": {"target": "h", "targets": ["h", "s", "v"],
                                    "sensor": "pressure", "enabled": enabled,
                                    "rect": [864, 864, 320, 320], "isotropic": False,
                                    "verifyCurve": True, "captureColorNotices": True,
                                    "verifyForeground": True,
                                    "qualification": "Joint source acquisition; operation order and dynamics unqualified"},
                })
    (root / "output" / "fixtures.json").write_text(json.dumps({
        "version": "5.3.4.1", "group": "hsvjoint", "presets": names, "cases": cases,
        "license": "CC0-1.0", "width": 2048, "height": 2048,
        "input": "Public Node.paintLine endpoints and pressure; no inferred internal dab pressure or HSV order",
    }, indent=2))
