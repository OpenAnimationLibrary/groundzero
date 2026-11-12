"""CI-only original acquisition/readback/analysis contract regressions."""
import base64
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zlib
from spacing_scatter_fixtures import prepare, SCATTER_GROUPS
from scatter_statistics import decode, measure, analyze

spec = importlib.util.spec_from_file_location("spacing_scatter_probe",
    Path(__file__).parent / "gzbrush_reference" / "spacing_scatter_probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class SpacingScatterContract(unittest.TestCase):
    def fixtures(self, group):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "output").mkdir()
            (root / "paintoppresets").mkdir()
            calls = []
            def writer(*args):
                calls.append(args)
                return b"original metadata placeholder"
            prepare(root, root, writer, group)
            result = json.loads((root / "output" / "fixtures.json").read_text())
        self.assertEqual(len(calls), len(result["cases"]))
        self.assertTrue(all(c[1] == "dry" for c in calls))
        return result

    def fields_and_delivery(self, case):
        fields = {**case["overrides"], "paintop": "paintbrush", "CompositeOp": "normal"}
        delivery = {"mode": "Node.paintLine", "events": [
            {"type": "line-start", "position": case["start"], "pressure": 1},
            {"type": "line-end", "position": case["end"], "pressure": 1}]}
        return fields, delivery

    def test_automatic_spacing_control_matrix(self):
        fixtures = self.fixtures("autospacing")
        self.assertEqual(len(fixtures["cases"]), 192)
        self.assertEqual(len(set(fixtures["presets"])), 192)
        self.assertFalse(fixtures["randomizeExecution"])
        self.assertTrue(probe.validate_manifest(fixtures))
        controls = set()
        for case in fixtures["cases"]:
            spec = case["autoSpacingProbe"]
            controls.add((case["diameter"], spec["ratio"], spec["coefficient"], spec["direction"], spec["enabled"]))
            fields, delivery = self.fields_and_delivery(case)
            self.assertFalse(probe.validate_controls(case, fields, delivery)["sourceProfileVerified"])
        self.assertEqual(len(controls), 192)

    def test_complete_scatter_replicates_and_axis_controls(self):
        for group in SCATTER_GROUPS:
            fixtures = self.fixtures(group)
            self.assertEqual(len(fixtures["cases"]), 102)
            self.assertTrue(fixtures["randomizeExecution"])
            self.assertTrue(probe.validate_manifest(fixtures))
            broken = copy.deepcopy(fixtures)
            broken["cases"][0]["scatterProbe"]["replicate"] = 1
            with self.assertRaises(RuntimeError): probe.validate_manifest(broken)
            broken = copy.deepcopy(fixtures)
            broken["randomizeExecution"] = False
            with self.assertRaises(RuntimeError): probe.validate_manifest(broken)
            enabled = [c for c in fixtures["cases"] if c["scatterProbe"]["enabled"]]
            disabled = [c for c in fixtures["cases"] if not c["scatterProbe"]["enabled"]]
            self.assertEqual(len(enabled), 100)
            self.assertEqual(len(disabled), 2)
            self.assertEqual({c["scatterProbe"]["replicate"] for c in enabled}, set(range(100)))
            for case in fixtures["cases"]:
                fields, delivery = self.fields_and_delivery(case)
                self.assertFalse(probe.validate_controls(case, fields, delivery)["sourceRngLawIdentified"])

    def test_mutated_source_controls_and_delivery_fail(self):
        for group in ("autospacing", "scatter-xy-east"):
            case = self.fixtures(group)["cases"][0]
            fields, delivery = self.fields_and_delivery(case)
            for key in ("brush_definition", "PressureScatter", "Spacing/Isotropic"):
                broken = dict(fields)
                broken[key] = "different setting"
                with self.assertRaises((RuntimeError, ValueError)):
                    probe.validate_controls(case, broken, delivery)
            changed = copy.deepcopy(delivery)
            changed["events"][1]["pressure"] = .5
            with self.assertRaises(RuntimeError):
                probe.validate_controls(case, fields, changed)

    def test_source_capture_rejects_empty_or_escaped_coverage(self):
        case = self.fixtures("scatter-x-east")["cases"][0]
        fields, delivery = self.fields_and_delivery(case)
        raw = bytearray(2048 * 2048 * 4)
        inside = (1024 * 2048 + 256) * 4
        raw[inside:inside + 4] = bytes((0, 0, 255, 255))
        evidence = probe.capture(case, fields, delivery, raw, 2048)
        self.assertEqual(evidence["nonzeroAlphaPixels"], 1)
        self.assertFalse(evidence["pixelParityClaimed"])
        raw[3] = 255
        with self.assertRaisesRegex(RuntimeError, "escaped"):
            probe.capture(case, fields, delivery, raw, 2048)
        raw[3] = 0
        raw[inside:inside + 4] = bytes(4)
        with self.assertRaisesRegex(RuntimeError, "no coverage"):
            probe.capture(case, fields, delivery, raw, 2048)

    def test_capture_stream_and_geometry_bounds(self):
        for direction, rect in (("east", [128, 896, 1280, 256]), ("north", [896, 128, 256, 1280])):
            size = 1280 * 256
            data = {"rect": rect, "encoding": "zlib-base64-alpha8-row-major",
                    "data": base64.b64encode(zlib.compress(bytes(size))).decode()}
            self.assertEqual(len(decode(data, direction)), size)
            for content in (bytes(size + 1), bytes(size - 1)):
                broken = dict(data, data=base64.b64encode(zlib.compress(content)).decode())
                with self.assertRaises(ValueError): decode(broken, direction)
            broken = dict(data, rect=[0, 0, 10**12, 10**12])
            with self.assertRaises(ValueError): decode(broken, direction)
            self.assertEqual(measure(bytes(size), direction)["0/coverage"], 0)

    def test_missing_or_pooled_rng_evidence_fails_before_measurement(self):
        with self.assertRaises(ValueError): analyze([], [])
        source = [{"preset": "s" + str(i), "scatterProbe": {"replicate": i},
                   "executionOrdinal": i} for i in range(100)]
        candidates = [{"preset": "s" + str(i), "candidateSeed": str(i + 1)} for i in range(100)]
        with self.assertRaisesRegex(ValueError, "randomized"):
            analyze(source, candidates)


if __name__ == "__main__":
    unittest.main()
