"""Original wet color-interaction captures, independent of candidate equations."""
import json


def prepare(root, resources, preset_writer):
    cases, names = [], []
    for modern in (False, True):
        for mode in ("smear", "dulling"):
            model = ("new-" if modern else "legacy-") + mode
            for channel in "hsv":
                for color in ([173, 91, 47, 255], [59, 137, 191, 255]):
                    for value in (.2, .8):
                        for enabled in (False, True):
                            name = "GZ Original " + mode + " " + ("new" if modern else "legacy") + " wethsv " + str(len(cases)).zfill(3)
                            curve = "0," + str(value) + ";1," + str(value) + ";"
                            overrides = {
                                "Pressureh": "false", "Pressures": "false", "Pressurev": "false",
                                "Pressure" + channel: str(enabled).lower(),
                                channel + "UseCurve": "true", channel + "UseSameCurve": "true",
                                channel + "Value": "1", channel + "curveMode": "0",
                                channel + "commonCurve": curve,
                                channel + "Sensor": '<params id="pressure"><curve>' + curve + '</curve></params>',
                                "Spacing/Isotropic": "false", "SmudgeRadiusValue": "0",
                                "SmudgeRateValue": "0.5", "ColorRateValue": "0.25",
                            }
                            (resources / "paintoppresets" / (name + ".kpp")).write_bytes(
                                preset_writer(name, mode, modern, overrides, 24, 1, 0))
                            names.append(name)
                            cases.append({
                                "preset": name, "variant": "baseline", "diameter": 24, "spacing": 1,
                                "pressure": [1, 1], "opacity": 1, "flow": 1,
                                "foregroundRGBA8": color, "start": [976, 1024], "end": [1072, 1024],
                                "background": "stripes", "overrides": overrides, "expectUnchanged": False,
                                "delivery": "Node.paintLine", "canvasCenter": [1024, 1024],
                                "wetHsvProbe": {"model": model, "channel": channel, "value": value,
                                                "enabled": enabled, "colorRate": .25},
                                "sensorProbe": {"target": channel, "sensor": "pressure", "enabled": enabled,
                                                "rect": [864, 864, 320, 320], "isotropic": False,
                                                "verifyCurve": True, "captureColorNotices": True,
                                                "verifyForeground": True,
                                                "qualification": "Wet source interaction acquisition; eligibility and RGBA gate remain required"},
                            })
    (root / "output" / "fixtures.json").write_text(json.dumps({
        "version": "5.3.4.1", "group": "wethsv", "presets": names, "cases": cases,
        "license": "CC0-1.0", "width": 2048, "height": 2048,
        "input": "Node.paintLine constant pressure1; transparent-to-blue boundary; nonzero source color rate",
    }, indent=2))
