"""CI-only fixed identity, history and input/readback guards for mask repeat research."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from mask_repeat_lifecycle_contract import (GROUPS, TARGET, capture, contract,
    difference_summary, prepare, repeat_count, validate, validate_delivery, validate_manifest,
    TABLET_ENTER, TABLET_LEAVE, TABLET_MOVE, TABLET_PRESS, TABLET_RELEASE)
from masked_fixtures import prepare as prepare_baseline


class MaskLifecycleContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.baseline_root = self.root / "baseline"
        (self.baseline_root / "output").mkdir(parents=True)
        (self.baseline_root / "paintoppresets").mkdir()
        def writer(*args):
            return json.dumps(args, sort_keys=True).encode()
        self.writer = writer
        prepare_baseline(self.baseline_root, self.baseline_root, writer, "maskwash")
        self.baseline = json.loads((self.baseline_root / "output" / "fixtures.json").read_text())

    def generate(self, group):
        root = self.root / group
        (root / "output").mkdir(parents=True)
        (root / "config").mkdir()
        (root / "config" / "kritarc").write_text("[General]\nuseOpenGL=false\n")
        (root / "paintoppresets").mkdir()
        prepare(root, root, self.writer, group)
        return root, json.loads((root / "output" / "fixtures.json").read_text())

    def test_closed_profiles_retain_exact_original_history_and_preset_bytes(self):
        self.assertEqual(len(GROUPS), 12)
        for group in GROUPS:
            root, manifest = self.generate(group)
            spec = contract(group)
            self.assertEqual(manifest["repeatDiagnostic"], spec)
            self.assertEqual(validate_manifest(manifest), spec)
            self.assertEqual(len(manifest["cases"]), len(spec["indices"]))
            for case, index in zip(manifest["cases"], spec["indices"]):
                self.assertEqual(validate(case), spec)
                self.assertEqual(repeat_count(case), 8 if index == TARGET else 2)
                original = self.baseline["cases"][index]
                for key in ("preset", "overrides", "diameter", "spacing", "pressure", "opacity", "flow"):
                    self.assertEqual(case[key], original[key])
                if index != TARGET:
                    self.assertEqual(case["start"], original["start"])
                    self.assertEqual(case["end"], original["end"])
                    self.assertEqual(case["delivery"], "Node.paintLine")
                name = case["preset"] + ".kpp"
                self.assertEqual((root / "paintoppresets" / name).read_bytes(),
                                 (self.baseline_root / "paintoppresets" / name).read_bytes())
            self.assertEqual((root / "config" / "kritarc").read_text().startswith("maxNumberOfThreads=1\n"),
                             spec["configuredMaxThreads"] == 1)
        self.assertIsNone(contract("masklife-unknown"))

    def test_invalid_repetition_identity_and_history_changes_reject(self):
        _, manifest = self.generate("masklife-reused")
        for alteration in ("drop-history", "reorder-history"):
            broken = copy.deepcopy(manifest)
            if alteration == "drop-history":
                broken["cases"] = broken["cases"][-1:]
                broken["presets"] = broken["presets"][-1:]
            else:
                broken["cases"][0], broken["cases"][1] = broken["cases"][1], broken["cases"][0]
                broken["presets"][0], broken["presets"][1] = broken["presets"][1], broken["presets"][0]
            with self.assertRaises(ValueError): validate_manifest(broken)
        for index in (0, TARGET):
            case = manifest["cases"][index]
            for key, value in (("repetitions", 1), ("historyRepetitions", 8), ("targetIndex", 0)):
                broken = copy.deepcopy(case)
                broken["repeatDiagnostic"][key] = value
                with self.assertRaises(ValueError): repeat_count(broken)
            broken = copy.deepcopy(case)
            broken["maskRepeatProbe"]["originalIndex"] = True
            with self.assertRaises(ValueError): repeat_count(broken)
            broken = copy.deepcopy(case)
            broken["start"][0] += 1
            with self.assertRaises(ValueError): repeat_count(broken)

    def test_node_and_actual_tablet_delivery_have_distinct_exact_guards(self):
        _, node = self.generate("masklife-isolated")
        case = node["cases"][0]
        delivery = {"mode": "Node.paintLine", "events": [
            {"type": "line-start", "position": case["start"], "pressure": 1},
            {"type": "line-end", "position": case["end"], "pressure": 1}]}
        validate_delivery(case, delivery)
        broken = copy.deepcopy(delivery)
        broken["events"][1]["position"][0] += 1
        with self.assertRaises(ValueError): validate_delivery(case, broken)
        _, tablet = self.generate("masklife-isolated-tablet")
        case = tablet["cases"][0]
        contacts = [{"type": TABLET_PRESS if i == 0 else TABLET_MOVE,
                     "image": [row["x"], row["y"]],
                     "pressure": row["pressure"], "tilt": [0, 0], "rotation": 0,
                     "timeMs": 1000 + round(row["timeUs"] / 1000)}
                    for i, row in enumerate(case["events"])]
        delivery = {"mode": "Qt tablet events", "events": contacts}
        self.assertEqual(len(validate_delivery(case, delivery, contacts_only=True)), 33)
        # Source has explicit proximity/hover/release events; cache stores only
        # the already checked contact subsequence. Both use authoritative kinds.
        def ancillary(contact, kind):
            return dict(contact, type=kind, pressure=0)
        full = {"mode": "Qt tablet events", "events": [
            ancillary(contacts[0], TABLET_ENTER), ancillary(contacts[0], TABLET_MOVE),
            *contacts, ancillary(contacts[-1], TABLET_RELEASE),
            ancillary(contacts[-1], TABLET_LEAVE)]}
        self.assertEqual(validate_delivery(case, full), contacts)
        with self.assertRaises(ValueError): validate_delivery(case, delivery)
        with self.assertRaises(ValueError): validate_delivery(case, full, contacts_only=True)
        # Values cannot declare their own accepted Qt kinds, omit sensor axes,
        # exploit bool==1, or trigger TypeError with malformed vectors/numbers.
        bad_fields = (("type", 1), ("type", 2), ("type", TABLET_PRESS),
                      ("type", float(TABLET_MOVE)), ("type", True),
                      ("image", [0, 0]), ("image", [904]), ("image", [904, 1024, 0]),
                      ("image", None), ("image", ["904", 1024]),
                      ("image", [float("nan"), 1024]), ("image", [10 ** 400, 1024]),
                      ("tilt", []), ("tilt", [0]), ("tilt", [0, 0, 0]),
                      ("tilt", [False, 0]), ("tilt", "00"),
                      ("pressure", .5), ("pressure", True), ("pressure", "1"),
                      ("pressure", float("inf")), ("pressure", None),
                      ("rotation", None), ("rotation", "0"), ("rotation", True),
                      ("timeMs", 0), ("timeMs", 1004.0), ("timeMs", "1004"),
                      ("timeMs", True), ("timeMs", -1), ("timeMs", 2 ** 64))
        for key, value in bad_fields:
            for contacts_only, original, index in ((True, delivery, 1), (False, full, 3)):
                with self.subTest(key=key, value=repr(value), contacts_only=contacts_only):
                    broken = copy.deepcopy(original)
                    broken["events"][index][key] = value
                    with self.assertRaises(ValueError):
                        validate_delivery(case, broken, contacts_only=contacts_only)
        for index, kind in ((0, TABLET_MOVE), (1, TABLET_PRESS), (32, TABLET_RELEASE)):
            broken = copy.deepcopy(delivery)
            broken["events"][index]["type"] = kind
            with self.assertRaises(ValueError): validate_delivery(case, broken, contacts_only=True)
        for bad_events in (None, {}, "events", [], contacts[:-1], contacts + contacts[-1:],
                           [None] + contacts[1:]):
            with self.assertRaises(ValueError):
                validate_delivery(case, {"mode": "Qt tablet events", "events": bad_events},
                                  contacts_only=True)
        for index, key, value in ((0, "type", TABLET_PRESS), (1, "pressure", 1),
                                   (-2, "type", TABLET_MOVE), (-1, "pressure", 1)):
            broken = copy.deepcopy(full)
            broken["events"][index][key] = value
            with self.assertRaises(ValueError): validate_delivery(case, broken)
        with self.assertRaises(ValueError): validate_delivery(case, {"mode": "Node.paintLine"})

    def test_tile_observation_summarizes_but_does_not_excuse_visible_difference(self):
        first = bytes(2048 * 2048 * 4)
        repeated = bytearray(first)
        for y in range(1021, 1027):
            for x in range(960, 1155):
                offset = (y * 2048 + x) * 4
                repeated[offset:offset + 4] = bytes((30, 40, 220, 128))
        result = difference_summary(first, bytes(repeated), 2048, [864, 992, 320, 64])
        self.assertEqual(result["differenceBounds"], [960, 1021, 195, 6])
        self.assertEqual(result["visibleOrAlphaChangedPixelsInRect"], 1170)
        self.assertEqual(result["alphaTransitions"], [[0, 128, 1170]])
        self.assertEqual(result["firstChangedXModulo64"], 0)
        self.assertEqual(result["documentGrid64Columns"], [[15, 384], [16, 384], [17, 384], [18, 18]])
        with self.assertRaises(ValueError): difference_summary(first[:-1], bytes(repeated), 2048, [864, 992, 320, 64])


if __name__ == "__main__":
    unittest.main()
