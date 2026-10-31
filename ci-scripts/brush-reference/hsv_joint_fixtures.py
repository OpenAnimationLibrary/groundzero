"""Original joint-channel and Node-pressure acquisitions; no assumed HSV order."""
import json


def prepare(root, resources, preset_writer, group="hsvjoint"):
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
    colors = ([173, 91, 47, 255], [59, 137, 191, 255])
    if group == "hsvorder":
        # Earlier paired darkening/desaturation cases commute. Simultaneous
        # saturation increase and lightening distinguish composition order.
        plans = [
            ("sv", (.5, .65, .65), False), ("sv", (.5, .85, .85), False),
            ("sv", (.5, .65, .85), False), ("sv", (.5, .85, .65), False),
            ("hsv", (.2, .65, .85), False), ("hsv", (.8, .85, .65), False),
        ]
        colors = ([101, 137, 157, 255], [173, 91, 47, 255])
    elif group == "hsvchroma":
        # Low original chroma and positive saturation values away from the
        # training controls; neither intermediate nor final chroma cap fires.
        plans=[]
        for saturation in (.55,.7,.9):
            plans.extend([("s",(.5,saturation,.5),False),
                          ("sv",(.5,saturation,.6),False),
                          ("hsv",(.35,saturation,.8),False)])
        colors=([120,128,136,255],[171,177,183,255])
    elif group != "hsvjoint":
        raise RuntimeError("Unknown joint HSV acquisition group")
    workloads = [(color, channels, values, dynamic, [.2, .8])
                 for color in colors for channels, values, dynamic in plans]
    if group == "hsvjoint":
        # Append, preserving all original 36 identities. Pressure channels are
        # explicit; fixed companions must not accidentally inherit the ramp.
        mixed = [(channels, (.5, .5, .5), channels)
                 for channels in ("hs", "hv", "sv", "hsv")]
        mixed += [("hsv", (.3, .7, .8), channel) for channel in "hsv"]
        for color, pressure in (([120,128,136,255], [.125,.875]),
                                ([173,91,47,255], [.875,.125])):
            workloads.extend((color, channels, values, dynamic, pressure)
                             for channels, values, dynamic in mixed)
    for color, channels, values, dynamic, pressure in workloads:
        dynamic_channels = channels if dynamic is True else dynamic or ""
        for enabled in (False, True):
            name = "GZ Original dry " + group + " " + str(len(cases)).zfill(3)
            overrides = {"Spacing/Isotropic": "false"}
            for channel, value in zip("hsv", values):
                curve = "0,0;1,1;" if channel in dynamic_channels else "0," + str(value) + ";1," + str(value) + ";"
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
                "pressure": pressure if dynamic else [1, 1], "opacity": 1, "flow": 1,
                "foregroundRGBA8": color, "start": [896, 1024], "end": [1152, 1024],
                "background": "transparent", "overrides": overrides, "expectUnchanged": False,
                "delivery": "Node.paintLine", "canvasCenter": [1024, 1024],
                "jointHsvProbe": {"channels": channels, "dynamicPressure": bool(dynamic),
                                  "pressureChannels": dynamic_channels,
                                  "enabled": enabled, "operationOrderAssumed": False},
                "sensorProbe": {"target": "h", "targets": ["h", "s", "v"],
                                "sensor": "pressure", "enabled": enabled,
                                "rect": [864, 864, 320, 320], "isotropic": False,
                                "verifyCurve": True, "captureColorNotices": True,
                                "verifyForeground": True,
                                "qualification": "Joint source acquisition; operation order and dynamics unqualified"},
            })
    (root / "output" / "fixtures.json").write_text(json.dumps({
        "version": "5.3.4.1", "group": group, "presets": names, "cases": cases,
        "license": "CC0-1.0", "width": 2048, "height": 2048,
        "input": "Public Node.paintLine endpoints and pressure; no inferred internal dab pressure or HSV order",
    }, indent=2))
