"""CI-only guards for joint metadata and Node pressure evidence."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from hsv_joint_fixtures import prepare

spec = importlib.util.spec_from_file_location("joint_sensor_probe", Path(__file__).parent / "gzbrush_reference" / "sensor_probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class JointHsvEvidence(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        (root / "output").mkdir()
        (root / "paintoppresets").mkdir()
        prepare(root, root, lambda *args: b"original fixture placeholder")
        self.fixtures = json.loads((root / "output" / "fixtures.json").read_text())
        self.case = self.fixtures["cases"][35]
        self.case["sensorProbe"]["rect"] = [0, 0, 1, 1]
        self.fields = {**self.case["overrides"], "brush_definition": "<Brush/>"}
        self.delivery = {"mode": "Node.paintLine", "events": [
            {"type": "line-start", "position": self.case["start"], "pressure": .2},
            {"type": "line-end", "position": self.case["end"], "pressure": .8},
        ]}

    def capture(self, fields=None, delivery=None):
        return probe.capture(self.case, self.fields if fields is None else fields,
                             self.delivery if delivery is None else delivery, b"\x11\x22\x33\xff", 1)

    def test_complete_matrix_and_node_evidence(self):
        self.assertEqual(len(self.fixtures["cases"]), 64)
        self.assertEqual(len(set(self.fixtures["presets"])), 64)
        evidence = self.capture()
        self.assertEqual(evidence["deliveryInterface"], "Node.paintLine")
        self.assertEqual(evidence["deliveredContactEvents"], self.delivery["events"])
        self.assertIn("vSensor", evidence["effectiveControls"])

    def test_joint_and_mixed_pressure_append_without_changing_originals(self):
        added = self.fixtures["cases"][36:]
        self.assertEqual(len(added), 28)
        self.assertEqual({tuple(c["pressure"]) for c in added},
                         {(.125, .875), (.875, .125)})
        self.assertEqual({c["jointHsvProbe"]["pressureChannels"] for c in added},
                         {"h", "s", "v", "hs", "hv", "sv", "hsv"})
        for case in added:
            spec = case["jointHsvProbe"]
            for channel in "hsv":
                fields = case["overrides"]
                self.assertEqual(fields["Pressure" + channel],
                    str(spec["enabled"] and channel in spec["channels"]).lower())
                curve = fields[channel + "commonCurve"]
                self.assertEqual(curve == "0,0;1,1;",
                                 channel in spec["pressureChannels"])
                self.assertIn(curve, fields[channel + "Sensor"])

    def test_order_matrix_exercises_noncommuting_controls(self):
        root=Path(self.temp.name)
        prepare(root, root, lambda *args: b"original fixture placeholder", "hsvorder")
        matrix=json.loads((root / "output" / "fixtures.json").read_text())
        self.assertEqual(len(matrix["cases"]),24)
        for case in matrix["cases"]:
            for channel in ("s","v"):
                value=float(case["overrides"][channel+"commonCurve"].split(";")[0].split(",")[1])
                self.assertGreater(value,.5)
            self.assertFalse(case["jointHsvProbe"]["operationOrderAssumed"])

    def test_pressure_heldouts_use_both_ramp_directions(self):
        from hsv_pressure_fixtures import prepare as prepare_pressure
        root=Path(self.temp.name)
        prepare_pressure(root,root,lambda *args:b"original fixture placeholder")
        matrix=json.loads((root/"output/fixtures.json").read_text())
        self.assertEqual(len(matrix["cases"]),24)
        self.assertEqual({tuple(c["pressure"]) for c in matrix["cases"]},
                         {(.125,.875),(.875,.125)})
        self.assertEqual({c["sensorProbe"]["target"] for c in matrix["cases"]},{"h","s","v"})
        self.assertTrue(all(c["delivery"]=="Node.paintLine" and "events" not in c
                            for c in matrix["cases"]))

    def test_low_chroma_heldouts_avoid_both_saturation_caps(self):
        root=Path(self.temp.name)
        prepare(root,root,lambda *args:b"original fixture placeholder","hsvchroma")
        matrix=json.loads((root/"output/fixtures.json").read_text())
        self.assertEqual(len(matrix["cases"]),36)
        for case in matrix["cases"]:
            foreground=case["foregroundRGBA8"][:3]
            # Even the largest possible qualified positive-S multiplier (4)
            # leaves these original chromas below their original maximum.
            self.assertLess((max(foreground)-min(foreground))*4,min(foreground))

    def test_every_channel_payload_is_checked(self):
        for channel in "hsv":
            fields = dict(self.fields)
            fields[channel + "commonCurve"] = "0,.1;1,.9;"
            with self.assertRaises(RuntimeError):
                self.capture(fields=fields)
            fields = dict(self.fields)
            fields[channel + "Sensor"] = '<params id="rotation"><curve>0,0;1,1;</curve></params>'
            with self.assertRaises(RuntimeError):
                self.capture(fields=fields)

    def test_wet_matrix_and_color_rate_readback(self):
        from wet_hsv_fixtures import prepare as prepare_wet
        root=Path(self.temp.name)
        prepare_wet(root, root, lambda *args: b"original fixture placeholder")
        matrix=json.loads((root / "output" / "fixtures.json").read_text())
        self.assertEqual(len(matrix["cases"]),96)
        self.assertEqual({c["wetHsvProbe"]["model"] for c in matrix["cases"]},
                         {"legacy-smear","legacy-dulling","new-smear","new-dulling"})
        self.case=matrix["cases"][-1]
        self.case["sensorProbe"]["rect"]=[0,0,1,1]
        self.fields={**self.case["overrides"], "brush_definition":"<Brush/>",
                     "paintop":"colorsmudge", "CompositeOp":"normal", "PaintOpAction":"1",
                     "SmudgeRateUseNewEngine":"true", "SmudgeRateMode":"1",
                     "SmudgeRateSmearAlpha":"true", "PressureColorRate":"true",
                     "ColorRateUseCurve":"false", "PressureSmudgeRate":"true",
                     "SmudgeRateUseCurve":"false", "SmudgeRadiusUseCurve":"false"}
        self.delivery={"mode":"Node.paintLine", "events":[
            {"type":"line-start","position":self.case["start"],"pressure":1},
            {"type":"line-end","position":self.case["end"],"pressure":1}]}
        self.assertEqual(self.capture()["effectiveControls"]["ColorRateValue"],"0.25")
        for key,value in (("ColorRateValue","0"),("SmudgeRateUseNewEngine","false"),
                          ("ColorRateUseCurve","true")):
            fields=dict(self.fields); fields[key]=value
            with self.assertRaises(RuntimeError):
                self.capture(fields=fields)

    def test_wrong_endpoint_or_pressure_is_rejected(self):
        for key, value in (("pressure", 1), ("position", [0, 0])):
            delivery = copy.deepcopy(self.delivery)
            delivery["events"][1][key] = value
            with self.assertRaises(RuntimeError):
                self.capture(delivery=delivery)


if __name__ == "__main__":
    unittest.main()
