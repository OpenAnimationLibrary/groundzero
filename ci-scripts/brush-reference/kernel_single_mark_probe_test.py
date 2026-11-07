"""CI-only single-mark bounds/raw-evidence checks; no source sampler model."""
import base64
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import unittest
import zlib

spec = importlib.util.spec_from_file_location("kernel_single_mark_probe",
    Path(__file__).parent / "gzbrush_reference" / "kernel_single_mark_probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def fixtures():
    original = json.loads((Path(__file__).parent / "wetkernelcount.json").read_text())
    cases = []
    for ordinal, item in enumerate(original["cases"]):
        if item["axis"] != "points" or item["model"] != "legacy":
            continue
        cases.append({"preset": "Original kernel mock " + str(ordinal),
                      "wetPattern": "opaque-black-white-points",
                      "diameter": item["diameter"], "spacing": 4,
                      "overrides": {"SmudgeRadiusValue": str(item["radiusPercent"])},
                      "wetKernelOffset": item["offset"], "wetKernelPoints": item["points"],
                      "wetKernelComplement": item["complement"]})
    return {"version": "5.3.4.1", "group": "wetkernelcount", "cases": cases}


def complete_report():
    manifest = fixtures()
    originals = [{"preset": case["preset"], "effectivePresetSha256": "a" * 64}
                 for case in manifest["cases"]]
    records = []
    for ordinal, item in enumerate(probe.plan(manifest)):
        authored = probe.initial(item)
        rect = probe.crop_rect(item)
        gray = item["backgroundGray"] * 257
        raw = struct.pack("<4H", gray, gray, gray, 65535) * (rect[2] * rect[3])
        second_x = 256 + item["diameter"] * 4
        records.append({"id": ordinal, **item, "colorModel": "RGBA", "depth": "U16",
                        "profile": probe.PROFILE, "canvas": [probe.WIDTH, probe.HEIGHT],
                        "rawInitialRoundTrip": True, "repeatEquivalent": True,
                        "initialRawSha256": hashlib.sha256(authored).hexdigest(),
                        "effectivePresetSha256": "a" * 64,
                        "canonicalSha256": "b" * 64, "rawSha256": ["c" * 64, "c" * 64],
                        "samplesRGBA16": [probe.sample(authored, 256, 128),
                                          [second_x, 128, gray, gray, gray, 65535]],
                        "rect": rect, "encoding": "zlib-base64-bgra16le-row-major",
                        "data": base64.b64encode(zlib.compress(raw)).decode("ascii")})
    return manifest, originals, {"schema": 1, "status": "complete",
        "qualification": probe.QUALIFICATION, "runtimeVersion": "5.3.4.1 (mock contract only)",
        "records": records}


class SingleMarkContract(unittest.TestCase):
    def test_plan_has_independent_marks_polarities_and_uniform_controls(self):
        planned = probe.plan(fixtures())
        self.assertEqual(len(planned), 70)
        self.assertEqual(sum(row["uniform"] for row in planned), 14)
        self.assertEqual({(row["diameter"], row["radiusPercent"]) for row in planned},
                         set(probe.COMBINATIONS))
        for diameter, radius in probe.COMBINATIONS:
            group = [row for row in planned if (row["diameter"], row["radiusPercent"]) == (diameter, radius)]
            marks = [row for row in group if not row["uniform"]]
            self.assertEqual(len({tuple(row["markedCoordinate"]) for row in marks}), 4)
            self.assertEqual({(row["backgroundGray"], row["markedGray"]) for row in marks},
                             {(112, 144), (144, 112)})
            self.assertTrue(all(probe.crop_rect(row)[2] <= 125 for row in group))
        for mutation in ("missing", "duplicate", "coordinates", "baseline"):
            bad = copy.deepcopy(fixtures())
            if mutation == "missing":
                bad["cases"] = [case for case in bad["cases"] if case["diameter"] != 72]
            if mutation == "duplicate": bad["cases"].append(copy.deepcopy(bad["cases"][0]))
            if mutation == "coordinates": bad["cases"][0]["wetKernelPoints"][0] = [1000, 0]
            if mutation == "baseline": bad["version"] = "other"
            with self.assertRaises(ValueError): probe.plan(bad)

    def test_raw_inputs_mark_exactly_one_pixel_and_keep_opaque_sampling_region(self):
        item = probe.plan(fixtures())[0]
        raw = probe.initial(item)
        uniform = probe.initial({**item, "markedCoordinate": None})
        self.assertEqual(len(raw), probe.WIDTH * probe.HEIGHT * 8)
        differing = [index for index in range(0, len(raw), 8) if raw[index:index + 8] != uniform[index:index + 8]]
        self.assertEqual(len(differing), 1)
        dx, dy = item["markedCoordinate"]
        self.assertEqual(differing, [((128 + dy) * probe.WIDTH + 256 + dx) * 8])
        self.assertEqual(probe.sample(raw, 256 + dx, 128 + dy)[2:], [144 * 257] * 3 + [65535])
        second_x = 256 + item["diameter"] * 4
        self.assertEqual(probe.sample(raw, second_x, 128)[2:], [0, 0, 0, 0])

    def test_complete_repeated_evidence_and_reject_changed_inputs(self):
        manifest, originals, report = complete_report()
        probe.validate(report, manifest, originals)
        for mutation in ("missing", "coordinate", "profile", "initial", "preset", "repeat", "sample", "crop", "runtime"):
            bad = copy.deepcopy(report)
            if mutation == "missing": bad["records"].pop()
            if mutation == "coordinate": bad["records"][0]["markedCoordinate"] = [0, 0]
            if mutation == "profile": bad["records"][0]["profile"] = "other"
            if mutation == "initial": bad["records"][0]["initialRawSha256"] = "0" * 64
            if mutation == "preset": bad["records"][0]["effectivePresetSha256"] = "0" * 64
            if mutation == "repeat": bad["records"][0]["repeatEquivalent"] = False
            if mutation == "sample": bad["records"][0]["samplesRGBA16"][1][2] += 1
            if mutation == "crop": bad["records"][0]["data"] = base64.b64encode(zlib.compress(bytes(128 * 128 * 8 + 1))).decode()
            if mutation == "runtime": bad["runtimeVersion"] = ""
            with self.assertRaises(ValueError): probe.validate(bad, manifest, originals)


if __name__ == "__main__":
    unittest.main()
