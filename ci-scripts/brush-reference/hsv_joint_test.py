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
        self.case = self.fixtures["cases"][-1]
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
        self.assertEqual(len(self.fixtures["cases"]), 36)
        self.assertEqual(len(set(self.fixtures["presets"])), 36)
        evidence = self.capture()
        self.assertEqual(evidence["deliveryInterface"], "Node.paintLine")
        self.assertEqual(evidence["deliveredContactEvents"], self.delivery["events"])
        self.assertIn("vSensor", evidence["effectiveControls"])

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

    def test_wrong_endpoint_or_pressure_is_rejected(self):
        for key, value in (("pressure", 1), ("position", [0, 0])):
            delivery = copy.deepcopy(self.delivery)
            delivery["events"][1][key] = value
            with self.assertRaises(RuntimeError):
                self.capture(delivery=delivery)


if __name__ == "__main__":
    unittest.main()
