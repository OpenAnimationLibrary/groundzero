"""CI-only original generated-curve discriminator identity/readback guards."""
import importlib.util
import copy
import json
from pathlib import Path
import tempfile
import unittest
from generated_curve_diagnostics import prepare, plans

spec = importlib.util.spec_from_file_location("generated_tip_probe",
    Path(__file__).parent / "gzbrush_reference/generated_tip_probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


path_spec = importlib.util.spec_from_file_location("generated_curve_probe",
    Path(__file__).parent / "gzbrush_reference/generated_curve_probe.py")
path_probe = importlib.util.module_from_spec(path_spec)
path_spec.loader.exec_module(path_probe)


class CurveDiscriminators(unittest.TestCase):
    def test_original_complete_controls_and_effective_readback(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "output").mkdir()
            (root / "paintoppresets").mkdir()
            writes = []
            def writer(*args):
                writes.append(args)
                return b"original discriminator metadata placeholder"
            prepare(root, root, writer)
            manifest = json.loads((root / "output" / "fixtures.json").read_text())
        self.assertEqual(len(plans()), 124)
        self.assertEqual(len(manifest["cases"]), 124)
        self.assertEqual(len(set(manifest["presets"])), 124)
        self.assertEqual(len(writes), 124)
        for case, written in zip(manifest["cases"], writes):
            self.assertEqual(written[1], "dry")
            self.assertEqual(case["sourceEngine"], "paintbrush")
            self.assertEqual(case["generatedCurveDiscriminator"], plans()[manifest["presets"].index(case["preset"])])
            definition = case["overrides"]["brush_definition"]
            self.assertEqual(probe.controls(case, definition)["effectiveMask"], case["generatedTipProbe"])
            for key in ("id", "type", "diameter", "hfade", "vfade"):
                with self.assertRaises(ValueError):
                    probe.controls(case, definition.replace(key + '="' + case["generatedTipProbe"][key] + '"', key + '="different"'))
        soft = [case for case in manifest["cases"] if case["generatedTipProbe"]["id"] == "soft"]
        self.assertEqual(len(soft), 100)
        self.assertEqual({case["generatedTipProbe"]["softness_curve"] for case in soft},
                         {"0,0;1,0;", "0,0.25;1,0.25;", "0,0.5;1,0.5;", "0,0.75;1,0.75;",
                          "0,1;1,1;", "0,0;0.5,0.5;1,1;", "0,1;1,0;", "0,0;1,1;"})
        fractional = [case for case in soft if case.get("generatedCurvePath")]
        self.assertEqual(len(fractional), 72)
        for case in fractional:
            self.assertEqual(path_probe.validate(case), case["sourcePath"])
            for mutation in ("position", "origin", "delivery", "curve-label"):
                bad = copy.deepcopy(case)
                if mutation == "position": bad["sourcePath"][0][0] += .01
                if mutation == "origin": bad["generatedCurveDiscriminator"]["originDelta"] = [1e20, 0]
                if mutation == "delivery": bad["delivery"] = "Node.paintLine"
                if mutation == "curve-label": bad["generatedTipProbe"]["softness_curve"] = "0,0.5;1,0.5;"
                with self.assertRaises(ValueError): path_probe.validate(bad)


if __name__ == "__main__":
    unittest.main()
