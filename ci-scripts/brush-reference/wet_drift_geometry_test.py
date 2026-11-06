"""CI-only bounds and effective-control guards for source placement research."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from wet_drift_geometry_contract import WIDTH, HEIGHT, capture, controls, plans, validate
from wet_drift_geometry_fixtures import prepare


class WetGeometryContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "output").mkdir()
        (self.root / "paintoppresets").mkdir()
        self.written = []
        def writer(name, mode, modern, overrides, *args):
            self.written.append((name, mode, modern, overrides, args))
            return b"original fixture metadata placeholder"
        prepare(self.root, self.root, writer)
        self.fixtures = json.loads((self.root / "output" / "fixtures.json").read_text())

    def test_complete_distinct_original_paths_and_engine_identity(self):
        self.assertEqual(len(self.fixtures["cases"]), 30)
        self.assertEqual(len(set(self.fixtures["presets"])), 30)
        self.assertEqual(len(plans()), 10)
        for case, written in zip(self.fixtures["cases"], self.written):
            self.assertEqual(validate(case), case["sourcePath"])
            self.assertEqual(written[1] == "dry", case["sourceEngine"] == "paintbrush")
            self.assertEqual(written[2], case["wetGeometryProbe"]["model"] == "new-smear")
            for key in ("PressureMirror", "Pressureh", "Pressures", "Pressurev"):
                self.assertEqual(written[3][key], "false")
                self.assertEqual(case["overrides"][key], "false")
        by_id = {plan["id"]: plan["vertices"] for plan in plans()}
        coarse, dense = by_id["turns-coarse"], by_id["turns-subdivided"]
        self.assertLess(len(coarse), len(dense))
        self.assertEqual(coarse[0], dense[0])
        self.assertEqual(coarse[-1], dense[-1])
        self.assertIn(327.875, by_id["east-y328-minus-eighth"][0])
        self.assertIn(328.125, by_id["east-y328-plus-eighth"][0])

    def test_mutated_paths_engines_and_sizes_reject(self):
        original = self.fixtures["cases"][10]
        for key, value in (("sourcePath", [[0, 0], [1, 1]]), ("sourceEngine", "paintbrush"),
                           ("diameter", 4096), ("spacing", .01), ("wetBoundary", 128)):
            changed = copy.deepcopy(original)
            changed[key] = value
            with self.assertRaises(ValueError): validate(changed)

    def test_capture_observes_raw_window_pixels_without_promoting_parity(self):
        case = self.fixtures["cases"][0]
        before = bytes((255, 0, 0, 255)) * (WIDTH * HEIGHT)
        after = bytearray(before)
        index = (40 * WIDTH + 272) * 4
        after[index:index + 4] = bytes((0, 255, 0, 255))
        evidence = capture(before, bytes(after), case)
        self.assertFalse(evidence["sourceParityQualified"])
        self.assertEqual(evidence["windows"][1]["greenBounds"], [272, 40, 1, 1])
        self.assertEqual(evidence["windows"][1]["greenPixels"], 1)
        with self.assertRaises(ValueError): capture(before[:-1], bytes(after), case)
        with self.assertRaises(ValueError): capture(bytes(len(before)), bytes(after), case)
        with self.assertRaisesRegex(ValueError, "no green paint"):
            capture(before, before, case)

    def test_effective_control_readback_is_required(self):
        case = self.fixtures["cases"][10]
        fields = {"paintop": "colorsmudge", "CompositeOp": "normal", "PaintOpAction": "1",
                  "OpacityValue": "1", "FlowValue": "1", "OpacityUseCurve": "false", "FlowUseCurve": "false",
                  "PressureSize": "false", "PressureOpacity": "false", "PressureFlow": "false",
                  "PressureRatio": "false", "PressureRotation": "false",
                  "PressureSpacing": "false", "PressureScatter": "false",
                  "Texture/Pattern/Enabled": "false", "Spacing/Isotropic": "false",
                  "PressureMirror": "false", "Pressureh": "false",
                  "Pressures": "false", "Pressurev": "false", "MergedPaint": "false",
                  "PaintOpSettings/isAirbrushing": "false", "PaintOpSettings/ignoreSpacing": "false",
                  "PaintOpSettings/updateSpacingBetweenDabs": "false",
                  "SmudgeRateMode": "0", "SmudgeRateSmearAlpha": "true", "SmudgeRateUseNewEngine": "false",
                  "PressureSmudgeRate": "true", "SmudgeRateUseCurve": "false", "PressureColorRate": "true",
                  "ColorRateUseCurve": "false", "SmudgeRadiusUseCurve": "false",
                  "SmudgeRateValue": ".5", "ColorRateValue": ".25", "SmudgeRadiusValue": "0",
                  "brush_definition": '<Brush type="auto_brush" BrushVersion="2" useAutoSpacing="0" spacing="2" angle="0" randomness="0" density="1"><MaskGenerator id="default" type="circle" antialiasEdges="0" spikes="2" diameter="8" ratio="1" hfade="1" vfade="1"/></Brush>'}
        controls(case, fields)
        for key, value in (("paintop", "paintbrush"), ("SmudgeRateUseNewEngine", "true"),
                           ("SmudgeRateValue", "nan"), ("PressureFlow", "true"),
                           ("PressureRatio", "true"), ("PressureRotation", "true"),
                           ("PressureSpacing", "true"), ("PressureScatter", "true"),
                           ("Texture/Pattern/Enabled", "true"), ("Spacing/Isotropic", "true"),
                           ("PressureMirror", "true"), ("Pressureh", "true"),
                           ("Pressures", "true"), ("Pressurev", "true"),
                           ("MergedPaint", "true"), ("PaintOpSettings/isAirbrushing", "true"),
                           ("PaintOpSettings/ignoreSpacing", "true"),
                           ("PaintOpSettings/updateSpacingBetweenDabs", "true")):
            changed = dict(fields, **{key: value})
            with self.assertRaises(ValueError): controls(case, changed)
        for original, replacement in (
                ('randomness="0"', 'randomness="0.1"'),
                ('density="1"', 'density="0.5"'),
                ('angle="0"', 'angle="nan"'),
                ('<Brush ', '<Other '),
                ('</Brush>', '<MaskGenerator/></Brush>')):
            changed = dict(fields, brush_definition=fields["brush_definition"].replace(
                original, replacement))
            with self.assertRaises(ValueError): controls(case, changed)


if __name__ == "__main__":
    unittest.main()
