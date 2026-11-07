"""Original wet composition input contracts; run in source-reference CI."""
from pathlib import Path
import tempfile
import unittest
from wet_hsv_combo_fixtures import append


class WetCompositionContract(unittest.TestCase):
    def test_bounded_crossproducts_preserve_existing_cases(self):
        cases, names = [{"preset": "existing"}], ["existing"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "paintoppresets").mkdir()
            append(root, lambda *args: b"original fixture stub", cases, names)
            self.assertEqual(len(list((root / "paintoppresets").glob("*.kpp"))), 48)
            self.assertEqual(len(list((root / "patterns").glob("*.png"))), 1)
        self.assertEqual(cases[0], {"preset": "existing"})
        self.assertEqual(names[0], "existing")
        self.assertEqual(len(cases), 49)
        self.assertEqual(len(set(names)), 49)
        added = cases[1:]
        textured = [c for c in added if c["wetHsvProbe"]["textureMode"] is not None]
        self.assertEqual(len(textured), 12)
        self.assertTrue(all(c["wetHsvProbe"]["model"] != "new-dulling" for c in textured))
        self.assertEqual(sum(c["opacity"] == .5 and c["flow"] == .5 for c in added), 4)
        self.assertTrue(any(c["pressure"] == [.8, .2] for c in added))
        for case in added:
            self.assertEqual(case["sensorProbe"]["targets"], ["h", "s", "v"])
            self.assertEqual(case["delivery"], "Node.paintLine")
            for channel, value in zip("hsv", case["wetHsvProbe"]["composition"]):
                self.assertEqual(case["overrides"]["Pressure" + channel], "true")
                curve = "0,0;1,1;" if value == "pressure" else f"0,{value};1,{value};"
                self.assertEqual(case["overrides"][channel + "commonCurve"], curve)
                self.assertIn("<curve>" + curve + "</curve>", case["overrides"][channel + "Sensor"])


if __name__ == "__main__":
    unittest.main()
