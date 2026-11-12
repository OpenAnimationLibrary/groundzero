"""Bounded original wet foreground/transport/texture composition holdouts."""
import base64
import hashlib
from texture_pickup_fixtures import png


def append(resources, preset_writer, cases, names):
    values = [64 if (x + 2 * y) % 3 else 192 for y in range(16) for x in range(16)]
    data = png(values)
    filename = "gz-original-wet-hsv-composition.png"
    (resources / "patterns").mkdir(parents=True, exist_ok=True)
    (resources / "patterns" / filename).write_bytes(data)
    texture = {
        "Texture/Pattern/Enabled": "true", "Texture/Pattern/Scale": "1",
        "Texture/Pattern/Brightness": "0", "Texture/Pattern/Contrast": "1",
        "Texture/Pattern/NeutralPoint": ".5", "Texture/Pattern/OffsetX": "0",
        "Texture/Pattern/OffsetY": "0", "Texture/Pattern/isRandomOffsetX": "false",
        "Texture/Pattern/isRandomOffsetY": "false", "Texture/Pattern/CutoffPolicy": "0",
        "Texture/Pattern/CutoffLeft": "0", "Texture/Pattern/CutoffRight": "255",
        "Texture/Pattern/Invert": "false", "Texture/Pattern/AutoInvertOnErase": "false",
        "Texture/Pattern/UseSoftTexturing": "false", "Texture/Pattern/PatternFileName": filename,
        "Texture/Pattern/Name": "gz-original-wet-hsv-composition",
        "Texture/Pattern/PatternMD5Sum": hashlib.md5(data).hexdigest(),
        "Texture/Pattern/Pattern": base64.b64encode(data).decode("ascii"),
        "PressureTexture/Strength/": "true", "Texture/Strength/Value": ".5",
        "Texture/Strength/UseCurve": "false",
    }
    start = len(cases)
    for model in ("legacy-smear", "legacy-dulling", "new-smear", "new-dulling"):
        specs = []
        for controls in ([.2, .8, .2], ["pressure", .8, .2],
                         [.8, "pressure", "pressure"], ["pressure"] * 3):
            for pressure in ([.2, .8], [.8, .2]):
                specs.append((controls, pressure, None, 1))
        if model != "new-dulling":
            for mode in (0, 1):
                for controls in ([.2, .8, .2], ["pressure", .8, .2]):
                    specs.append((controls, [.2, .8], mode, 1))
        specs.append(([.2, .8, .2], [.2, .8], None, .5))
        for controls, pressure, texture_mode, transfer in specs:
            name = "GZ Original wet HSV composition " + str(len(cases) - start).zfill(3)
            overrides = {"Spacing/Isotropic": "false", "SmudgeRadiusValue": "0",
                         "SmudgeRateValue": "0.5", "ColorRateValue": "0.25"}
            for channel, value in zip("hsv", controls):
                curve = "0,0;1,1;" if value == "pressure" else f"0,{value};1,{value};"
                overrides.update({"Pressure" + channel: "true", channel + "UseCurve": "true",
                    channel + "UseSameCurve": "true", channel + "Value": "1", channel + "curveMode": "0",
                    channel + "commonCurve": curve,
                    channel + "Sensor": '<params id="pressure"><curve>' + curve + '</curve></params>'})
            if texture_mode is not None:
                overrides.update(texture)
                overrides["Texture/Pattern/TexturingMode"] = str(texture_mode)
            modern, mode = model.split("-")
            (resources / "paintoppresets" / (name + ".kpp")).write_bytes(
                preset_writer(name, mode, modern == "new", overrides, 24, 1, 0))
            names.append(name)
            case = {"preset": name, "variant": "baseline", "diameter": 24, "spacing": 1,
                "pressure": pressure, "opacity": transfer, "flow": transfer,
                "foregroundRGBA8": [173, 91, 47, 255], "start": [976, 1024], "end": [1072, 1024],
                "background": "stripes", "overrides": overrides, "expectUnchanged": False,
                "delivery": "Node.paintLine", "sourceEngine": "colorsmudge", "canvasCenter": [1024, 1024],
                "wetHsvProbe": {"model": model, "channel": "h", "colorRate": .25,
                                "composition": controls, "textureMode": texture_mode, "transfer": transfer},
                "sensorProbe": {"target": "h", "targets": ["h", "s", "v"], "sensor": "pressure",
                    "enabled": True, "rect": [864, 864, 320, 320], "isotropic": False,
                    "verifyCurve": True, "captureColorNotices": True, "verifyForeground": True,
                    "qualification": "Wet HSV composition candidate requires full RGBA and strict import gates"}}
            if texture_mode is not None:
                case.update({"texturePatternFilename": filename, "texturePatternExpectedGray8": values,
                             "texturePatternSha256": hashlib.sha256(data).hexdigest()})
            cases.append(case)
    if len(cases) - start != 48:
        raise RuntimeError("Unexpected bounded wet HSV composition count")
