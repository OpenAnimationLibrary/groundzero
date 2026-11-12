"""Original probes from bd6e8d0 option serialization facts, not rendering rules."""
import json
from sensor_fixtures import trace


def prepare(root, resources, preset_writer, group, supplied_plans=None):
    plans = [] if supplied_plans is None else supplied_plans
    if supplied_plans is not None:
        pass
    elif group == "hsvdynamics":
        for target in ("h", "s", "v"):
            for color in ([173, 91, 47, 255], [59, 137, 191, 255]):
                # Constant curves avoid assuming synthetic tablet pressure is
                # consumed by the paint engine merely because Qt delivered it.
                for value in (0, .2, .4, .6, .8, 1):
                    curve = "0," + str(value) + ";1," + str(value) + ";"
                    plans.append((target, "pressure", {}, curve, trace("east"), color))
    elif group == "distancesensors":
        for length in (64, 128):
            for periodic in (0, 1):
                for direction in ("east", "north", "diagonal", "reverse"):
                    plans.append(("Size", "distance", {"length": str(length), "periodic": str(periodic)},
                                  "0,0.25;1,1;", trace(direction), [173, 91, 47, 255]))
    else:
        raise RuntimeError("Unknown option acquisition group")
    cases, names = [], []
    for target, sensor, attrs, curve, events, color in plans:
        for enabled in (False, True):
            name = "GZ Original dry " + group + " " + str(len(cases)).zfill(3)
            attributes = "".join(' ' + key + '="' + value + '"' for key, value in attrs.items())
            overrides = {"Pressureh": "false", "Pressures": "false", "Pressurev": "false",
                         "Pressure" + target: str(enabled).lower(), target + "UseCurve": "true",
                         target + "UseSameCurve": "true", target + "Value": "1",
                         target + "curveMode": "0", target + "commonCurve": curve,
                         target + "Sensor": '<params id="' + sensor + '"' + attributes + '><curve>' + curve + '</curve></params>',
                         "Spacing/Isotropic": "false"}
            (resources / "paintoppresets" / (name + ".kpp")).write_bytes(
                preset_writer(name, "dry", False, overrides, 24, 1, 0))
            names.append(name)
            cases.append({"preset": name, "variant": "baseline", "diameter": 24, "spacing": 1,
                          "pressure": [1, 1], "opacity": 1, "flow": 1,
                          "foregroundRGBA8": color, "start": [events[0]["x"], events[0]["y"]],
                          "end": [events[-1]["x"], events[-1]["y"]], "background": "transparent",
                          "overrides": overrides, "expectUnchanged": False,
                          "delivery": "Qt tablet events", "canvasCenter": [1024, 1024], "events": events,
                          "sensorProbe": {"target": target, "sensor": sensor, "enabled": enabled,
                                          "rect": [864, 864, 320, 320], "isotropic": False,
                                          "verifyCurve": True, "captureColorNotices": group in ("hsvdynamics", "hsvheldout"),
                                          "verifyForeground": True,
                                          "qualification": "Metadata-verified acquisition; no source behavioral parity"}})
    (root / "output" / "fixtures.json").write_text(json.dumps({
        "version": "5.3.4.1", "group": group, "presets": names, "cases": cases,
        "license": "CC0-1.0", "width": 2048, "height": 2048,
        "input": "Qt5 tablet events; constant pressure1; effective options and source foreground readback"}, indent=2))
