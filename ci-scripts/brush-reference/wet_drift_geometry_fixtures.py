"""Original Smear placement diagnostics, independent of candidate equations."""
import json
from pathlib import Path
import shutil
from wet_drift_geometry_contract import MODELS, plans


def prepare(root, resources, preset_writer):
    destination = resources / "pykrita" / "gzbrush_reference"
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(Path(__file__).with_name("wet_drift_geometry_contract.py"),
                    destination / "wet_drift_geometry_contract.py")
    cases = []
    for model in MODELS:
        for plan in plans():
            name = "GZ Original wet geometry " + model + " " + plan["id"]
            overrides = {"SmudgeRateValue": ".5", "ColorRateValue": ".25", "SmudgeRadiusValue": "0",
                         "PressureOpacity": "false", "PressureFlow": "false", "Spacing/Isotropic": "false",
                         "PressureMirror": "false", "Pressureh": "false",
                         "Pressures": "false", "Pressurev": "false"}
            (resources / "paintoppresets" / (name + ".kpp")).write_bytes(preset_writer(
                name, "dry" if model == "dry" else "smear", model == "new-smear", overrides, 8, 2, 0))
            cases.append({"preset": name, "variant": "baseline", "diameter": 8, "spacing": 2,
                          "pressure": [1, 1], "opacity": 1, "flow": 1,
                          "start": plan["vertices"][0], "end": plan["vertices"][-1],
                          "sourcePath": plan["vertices"], "sourceEngine": "paintbrush" if model == "dry" else "colorsmudge",
                          "delivery": "Node.paintPath", "background": "wet-halves", "wetBoundary": 2048,
                          "foregroundRGBA8": [0, 255, 0, 255], "overrides": overrides,
                          "expectUnchanged": False,
                          "wetGeometryProbe": {"pathId": plan["id"], "model": model},
                          "changeExpectation": "source-only placement discriminator; no geometry equation or parity assumed"})
    (root / "output" / "fixtures.json").write_text(json.dumps({
        "version": "5.3.4.1", "group": "wetgeometry", "presets": [case["preset"] for case in cases],
        "cases": cases, "license": "CC0-1.0", "width": 2048, "height": 2048,
        "input": "One original Node.paintPath per case; exact authored vertices and source readback retained"}, indent=2))
