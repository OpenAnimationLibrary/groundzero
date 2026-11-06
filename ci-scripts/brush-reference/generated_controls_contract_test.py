"""CI-only generated domain scope, actual XML binding and strict-type guards."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from xml.etree.ElementTree import Element, SubElement, tostring, fromstring

import generated_controls_contract as contract
from generated_controls_fixtures import prepare
import oracle_cache as cache


def manifest(group):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        (root / "output").mkdir()
        (root / "paintoppresets").mkdir()
        prepare(root, root, lambda *args: b"original fixture metadata", group)
        return json.loads((root / "output/fixtures.json").read_text())


def delivery(case):
    if case["delivery"] == "Node.paintLine":
        return {"mode": "Node.paintLine", "events": [
            {"type": "line-start", "position": list(map(int, case["start"])), "pressure": case["pressure"][0]},
            {"type": "line-end", "position": list(map(int, case["end"])), "pressure": case["pressure"][1]}]}
    return {"mode": "Qt tablet events", "events": [
        {"type": 92 if index == 0 else 87, "image": [event["x"], event["y"]],
         "pressure": event["pressure"], "tilt": [event["tiltX"], event["tiltY"]],
         "rotation": event["rotation"], "timeMs": event["timeUs"] / 1000}
        for index, event in enumerate(case["events"])]}


def effective_xml(case, fields):
    preset = Element("Preset", name=case["preset"], paintopid="paintbrush")
    for key, value in fields.items():
        SubElement(preset, "param", name=key).text = value
    return tostring(preset, encoding="utf-8")


class GeneratedControlContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifests = {group: manifest(group) for group in contract.COUNTS}

    def test_complete_original_groups_and_truthful_pending_dimensions(self):
        for group, fixture in self.manifests.items():
            self.assertEqual(len(fixture["cases"]), contract.COUNTS[group])
            self.assertEqual(contract.validate_manifest(fixture), contract.status(group))
            for case in fixture["cases"]:
                observed = contract.capture(group, case, case["overrides"], delivery(case))
                self.assertIs(observed["candidateExpected"], group not in contract.PENDING)
                self.assertIs(observed["sourceReferenceVerified"], False)
                self.assertIs(observed["sourceParityQualified"], False)
                self.assertNotIn("sourceAdmission", case)
                if "timedProbe" in case:
                    self.assertNotIn("sensorProbe", case)
        transfer = self.manifests["generated-transfer"]["cases"]
        self.assertEqual(sum("sensorProbe" in case for case in transfer), 80)

    def test_only_explicit_static_zero_flow_controls_expect_unchanged_pixels(self):
        zeros = []
        for group, fixture in self.manifests.items():
            for case in fixture["cases"]:
                expected = group == "generated-transfer" and case["flow"] == 0
                self.assertIs(case["expectUnchanged"], expected)
                if expected:
                    zeros.append(case)
                    self.assertEqual(case["generatedControlProbe"]["objective"], "static flow 0")
                    self.assertEqual(float(case["overrides"]["FlowValue"]), 0)
                    self.assertEqual(case["delivery"], "Node.paintLine")
                    self.assertNotIn("sensorProbe", case)
                    self.assertNotIn("timedProbe", case)
                    self.assertIs(case["generatedControlProbe"]["sourceParityQualified"], False)
        self.assertEqual(len(zeros), 8)  # Four authored families at both measured sizes.

    def test_incomplete_or_forged_matrix_cannot_change_implementation_scope(self):
        for group, fixture in self.manifests.items():
            for defect in ("truncated", "missing", "identity", "numeric-flag", "qualification", "exception"):
                broken = copy.deepcopy(fixture)
                if defect == "truncated":
                    broken["presets"].pop()
                    broken["cases"].pop()
                elif defect == "missing":
                    broken["cases"].pop()
                elif defect == "identity":
                    broken["presets"][0] = broken["cases"][0]["preset"] = "different authored case"
                elif defect == "numeric-flag":
                    broken["cases"][0]["generatedControlProbe"]["candidateExpected"] = int(group not in contract.PENDING)
                elif defect == "qualification":
                    broken["cases"][0]["generatedControlProbe"]["sourceParityQualified"] = 0
                else:
                    broken["cases"][0]["sourceAdmission"] = {"outcome": "expected-rejection"}
                with self.subTest(group=group, defect=defect), self.assertRaises(ValueError):
                    contract.validate_manifest(broken)

    def test_cache_replays_actual_xml_and_delivery_for_all_three_groups(self):
        for group, fixture in self.manifests.items():
            case = fixture["cases"][0]
            reported = delivery(case)
            row = {**copy.deepcopy(case), "deliveryReport": reported,
                   "generatedControlEvidence": contract.capture(group, case, case["overrides"], reported)}
            cache.validate_generated_control_effective_preset(
                contract.__dict__, group, case, row, effective_xml(case, case["overrides"]))

    def test_evidence_version_flags_and_unknown_fields_are_strictly_typed(self):
        case = self.manifests["generated-transfer"]["cases"][0]
        reported = delivery(case)
        baseline = contract.capture("generated-transfer", case, case["overrides"], reported)
        for key, value in (("version", True), ("version", 1.0), ("version", "1"),
                           ("candidateExpected", 1), ("sourceReferenceVerified", 0),
                           ("sourceSemanticallySupported", 0), ("sourceParityQualified", 0),
                           ("unlistedDescriptor", "arbitrary source text")):
            evidence = copy.deepcopy(baseline)
            evidence[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                contract.validate_evidence("generated-transfer", case, case["overrides"], reported, evidence)

    def test_cache_binds_evidence_to_actual_preset_not_an_echoed_dictionary(self):
        case = self.manifests["generated-transfer"]["cases"][0]
        reported = delivery(case)
        baseline = {**copy.deepcopy(case), "deliveryReport": reported,
                    "generatedControlEvidence": contract.capture("generated-transfer", case, case["overrides"], reported)}
        for defect in ("control", "brush", "identity", "duplicate", "malformed", "declaration", "delivery", "case-type"):
            row = copy.deepcopy(baseline)
            fields = dict(case["overrides"])
            data = effective_xml(case, fields)
            if defect == "control":
                fields["OpacityValue"] = ".125"
                data = effective_xml(case, fields)
            elif defect == "brush":
                fields["brush_definition"] = fields["brush_definition"].replace('hfade=".25"', 'hfade=".9"')
                data = effective_xml(case, fields)
            elif defect == "identity":
                data = data.replace(case["preset"].encode(), b"different original preset")
            elif defect == "duplicate":
                data = data.replace(b"</Preset>", b'<param name="OpacityValue">1</param></Preset>')
            elif defect == "malformed":
                data = b"<Preset"
            elif defect == "declaration":
                data = b'<!DOCTYPE Preset [<!ENTITY v "changed">]>' + data
            elif defect == "delivery":
                row["deliveryReport"]["events"][1]["position"][0] += 1
            else:
                row["generatedControlProbe"]["candidateExpected"] = 1
            with self.subTest(defect=defect), self.assertRaises(ValueError):
                cache.validate_generated_control_effective_preset(contract.__dict__, "generated-transfer", case, row, data)

    def test_malformed_authored_or_effective_sensor_xml_is_a_value_error(self):
        case = next(case for case in self.manifests["generated-transfer"]["cases"] if "sensorProbe" in case)
        target = case["sensorProbe"]["target"] + "Sensor"
        for authored in (False, True):
            broken = copy.deepcopy(case)
            fields = dict(case["overrides"])
            if authored:
                broken["overrides"][target] = "<params"
            else:
                fields[target] = "<params"
            with self.subTest(authored=authored), self.assertRaisesRegex(ValueError, "Malformed generated Sensor XML"):
                contract.capture("generated-transfer", broken, fields, delivery(broken))

    def test_closed_settings_reject_coordinated_actual_xml_and_evidence_changes(self):
        case = next(case for case in self.manifests["generated-transfer"]["cases"] if "sensorProbe" in case)
        sent = delivery(case)
        for key in ("brush_definition", case["sensorProbe"]["target"] + "Sensor"):
            for defect in ("tail", "namespace", "empty-namespace", "comment", "pi", "dtd"):
                fields = dict(case["overrides"])
                tree = fromstring(fields[key])
                if defect == "tail":
                    tree[0].tail = "unsupported"
                    fields[key] = tostring(tree, encoding="unicode")
                elif defect == "namespace": fields[key] = fields[key].replace(" ", ' xmlns:unused="urn:unsupported" ', 1)
                elif defect == "empty-namespace": fields[key] = fields[key].replace(" ", ' xmlns="" ', 1)
                elif defect == "comment": fields[key] = "<!--unsupported-->" + fields[key]
                elif defect == "pi": fields[key] = "<?unsupported payload?>" + fields[key]
                else: fields[key] = "<!DOCTYPE " + tree.tag + " [<!ENTITY x 'unsupported'>]>" + fields[key]
                proof = contract.capture("generated-transfer", case, case["overrides"], sent)
                if key != "brush_definition": proof["effectiveControls"][key] = fields[key]
                data = effective_xml(case, fields)
                row = {**copy.deepcopy(case), "deliveryReport": sent, "generatedControlEvidence": proof,
                       "effectivePresetSha256": cache.digest(data)}
                with self.subTest(key=key, defect=defect), self.assertRaises(ValueError):
                    cache.validate_generated_control_effective_preset(contract.__dict__, "generated-transfer", case, row, data)

    def test_actual_preset_header_cdata_and_curve_whitespace_remain_supported(self):
        case = next(case for case in self.manifests["generated-transfer"]["cases"] if "sensorProbe" in case)
        fields = dict(case["overrides"])
        key = case["sensorProbe"]["target"] + "Sensor"
        tree = fromstring(fields[key]); tree.text = "\n  "; tree[0].tail = "\n"
        tree[0].text = "\n  " + tree[0].text + "\n"
        fields[key] = tostring(tree, encoding="unicode")
        sent = delivery(case)
        row = {**case, "deliveryReport": sent, "generatedControlEvidence": contract.capture("generated-transfer", case, fields, sent)}
        data = ('<?xml version="1.0" encoding="UTF-8"?>\n<Preset name="' + case["preset"] + '" paintopid="paintbrush">' +
                ''.join('<param name="' + key + '"><![CDATA[' + value + ']]></param>' for key, value in fields.items()) + '</Preset>').encode()
        cache.validate_generated_control_effective_preset(contract.__dict__, "generated-transfer", case, row, data)
        for defect in ("namespace", "comment", "pi", "param-pi", "param-comment", "dtd", "tail"):
            altered = data
            if defect == "namespace": altered = data.replace(b'<Preset ', b'<Preset xmlns:u="urn:unsupported" ')
            elif defect == "comment": altered = data.replace(b'<Preset ', b'<!--unsupported--><Preset ')
            elif defect == "pi": altered = data.replace(b'<Preset ', b'<?unsupported payload?><Preset ')
            elif defect == "param-pi": altered = data.replace(b'</param>', b'<?unsupported payload?></param>', 1)
            elif defect == "param-comment": altered = data.replace(b'</param>', b'<!--unsupported--></param>', 1)
            elif defect == "dtd": altered = data.replace(b'<Preset ', b'<!DOCTYPE Preset [<!ENTITY x "unsupported">]><Preset ')
            else: altered = data.replace(b'</param>', b'</param>unsupported', 1)
            with self.subTest(defect=defect), self.assertRaises(ValueError):
                cache.validate_generated_control_effective_preset(contract.__dict__, "generated-transfer", case, row, altered)

    def test_contact_tampering_and_nonfinite_clock_cannot_be_replayed(self):
        case = next(case for case in self.manifests["generated-transfer"]["cases"] if "sensorProbe" in case)
        for defect in ("missing", "position", "pressure", "numeric-type", "clock"):
            actual = delivery(case)
            if defect == "missing": actual["events"].pop()
            elif defect == "position": actual["events"][2]["image"][0] += .01
            elif defect == "pressure": actual["events"][2]["pressure"] = .9
            elif defect == "numeric-type": actual["events"][2]["pressure"] = True
            else: actual["events"][2]["timeMs"] = float("inf")
            with self.subTest(defect=defect), self.assertRaises(ValueError):
                contract.capture("generated-transfer", case, case["overrides"], actual)


if __name__ == "__main__":
    unittest.main()
