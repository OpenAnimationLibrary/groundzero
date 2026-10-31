"""Original wet drift checkpoints and observable path-count controls."""
import json
import shutil
from pathlib import Path
from wet_drift_contract import CHECKPOINTS, WIDTH, HEIGHT, path


def prepare(root, resources, preset_writer):
    shutil.copyfile(Path(__file__).with_name("wet_drift_contract.py"),
                    resources / "pykrita/gzbrush_reference/wet_drift_contract.py")
    cases, names = [], []
    for model in ("count-control", "legacy-smear", "legacy-dulling", "new-smear", "new-dulling"):
        for count in CHECKPOINTS:
            calibration = model == "count-control"
            name = "GZ Original wet drift " + model + " checkpoint " + str(count)
            vertices = path(count)
            overrides = {"SmudgeRateValue": ".5", "ColorRateValue": ".25", "SmudgeRadiusValue": "0"}
            (resources / "paintoppresets" / (name + ".kpp")).write_bytes(preset_writer(
                name, "dry" if calibration else model.split("-")[1], model.startswith("new"), overrides, 8, 2, 0))
            names.append(name)
            cases.append({"preset": name, "variant": "baseline", "diameter": 8, "spacing": 2,
                          "pressure": [1, 1], "opacity": .5 if calibration else 1, "flow": 1,
                          "start": vertices[0], "end": vertices[-1], "sourcePath": vertices,
                          "delivery": "Node.paintPath", "background": "transparent" if calibration else "wet-drift",
                          "foregroundRGBA8": [0, 255, 0, 255], "overrides": overrides,
                          "expectUnchanged": count == 1 and not calibration,
                          "driftProbe": {"nominalDabs": count, "calibration": calibration, "model": model},
                          "changeExpectation": "continuous common-prefix drift; observable deposit count required"})
    (root / "output/fixtures.json").write_text(json.dumps({"version": "5.3.4.1", "group": "wetdrift",
        "presets": names, "cases": cases, "license": "CC0-1.0", "width": WIDTH, "height": HEIGHT,
        "input": "One continuous Node.paintPath per prefix; never concatenate independent paintLine strokes"}, indent=2))
