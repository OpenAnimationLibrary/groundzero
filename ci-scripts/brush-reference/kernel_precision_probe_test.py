"""Pure input-contract checks; no reference renderer or project build needed."""
import base64
import copy
import zlib
import importlib.util
import json
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("kernel_precision", ROOT / "gzbrush_reference/kernel_precision_probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class KernelPrecisionContract(unittest.TestCase):
    def fixtures(self):
        cases = []
        for i, item in enumerate(json.loads((ROOT / "wetkernelcount.json").read_text())["cases"]):
            if item["axis"] == "points":
                cases.append({"preset": "Original " + str(i), "diameter": item["diameter"],
                              "overrides": {"SmudgeRadiusValue": str(item["radiusPercent"])},
                              "wetPattern": "opaque-black-white-points",
                              "wetKernelOffset": item["offset"], "wetKernelPoints": item["points"],
                              "spacing": 4, "wetKernelComplement": item["complement"]})
        return {"cases": cases, "version": "5.3.4.1"}

    def test_bounded_balanced_plan(self):
        cases = probe.plan(self.fixtures())
        self.assertEqual(len(cases), 58)
        self.assertEqual(sum(c["depth"] == "U16" for c in cases), 25)
        gray = [c for c in cases if c["low"] == 64 and c["high"] == 192]
        self.assertEqual(len(gray), 8)
        self.assertTrue(all(c["depth"] == "U8" for c in gray))
        self.assertEqual(sum(c["low"] == c["high"] for c in cases), 10)
        self.assertEqual(sum(c["dabs"] == 3 for c in cases), 8)

    def test_missing_mask_rejected(self):
        fixtures = self.fixtures()
        fixtures["cases"].pop()
        with self.assertRaises(RuntimeError):
            probe.plan(fixtures)

    def complete_report(self):
        fixtures = self.fixtures()
        originals = [{"preset": c["preset"], "effectivePresetSha256": "a" * 64,
                      "centerlineRGBA8": [[256 + c["diameter"] * c["spacing"], 0, 0, 0, 0],
                                           [1808, 17, 23, 31, 255]]} for c in fixtures["cases"]]
        rows = []
        for ordinal, item in enumerate(probe.plan(fixtures)):
            case, depth = item["case"], item["depth"]
            radius = int(case["diameter"] * max(1, float(case["overrides"]["SmudgeRadiusValue"]) / 100) / 2) + 2
            x = 256 + case["diameter"] * 4 * (item["dabs"] - 1)
            rect = [x - radius, 128 - radius, radius * 2 + 1, radius * 2 + 1]
            rows.append({"id": ordinal, "preset": case["preset"], "depth": depth, "profile": probe.PROFILE,
                "low": item["low"], "high": item["high"], "dabs": item["dabs"],
                "pointCount": len(case["wetKernelPoints"]), "colorModel": "RGBA",
                "runtimeVersion": fixtures["version"], "canvas": [probe.WIDTH, probe.HEIGHT],
                "maximumComponent": 255 if depth == "U8" else 65535,
                "samplesRGBA": [[256 + case["diameter"] * 4 * d, 128, 0, 0, 0, 0] for d in range(item["dabs"])],
                "translationControl": depth == "U8" and item["low"] == 0 and item["high"] == 255 and item["dabs"] == 2,
                "rawInitialRoundTrip": True, "repeatEquivalent": True,
                "initialRawSha256": "a" * 64, "canonicalSha256": "a" * 64,
                "effectivePresetSha256": "a" * 64, "rawSha256": ["a" * 64, "a" * 64],
                "rect": rect, "encoding": "zlib-base64-bgra" + ("8" if depth == "U8" else "16le") + "-row-major",
                "data": base64.b64encode(zlib.compress(bytes(rect[2] * rect[3] * (4 if depth == "U8" else 8)))).decode()})
        return fixtures, originals, {"schema": 1, "status": "complete",
            "qualification": "research-only; original U8 parity gates remain separate", "records": rows}

    def test_cache_completeness_and_raw_crop_bounds(self):
        fixtures, originals, report = self.complete_report()
        probe.validate(report, fixtures, originals)
        for mutation in ("missing", "profile", "roundtrip", "oversized"):
            broken = copy.deepcopy(report)
            if mutation == "missing":
                broken["records"].pop()
            elif mutation == "profile":
                broken["records"][0]["profile"] = "unverified profile"
            elif mutation == "roundtrip":
                broken["records"][0]["rawInitialRoundTrip"] = False
            else:
                broken["records"][0]["data"] = base64.b64encode(zlib.compress(bytes(200000))).decode()
            with self.assertRaises(ValueError):
                probe.validate(broken, fixtures, originals)

    def test_second_sample_is_selected_by_coordinate_not_scan_tail(self):
        case = {"diameter": 56, "spacing": 4}
        record = {"centerlineRGBA8": [[256, 0, 0, 0, 255], [480, 40, 40, 40, 255],
                                       [1808, 0, 0, 0, 0]]}
        self.assertEqual(probe.original_second_sample(record, case), [40, 40, 40, 255])
        record["centerlineRGBA8"].append([480, 1, 1, 1, 255])
        with self.assertRaises(ValueError):
            probe.original_second_sample(record, case)
        record["centerlineRGBA8"] = [[1808, 0, 0, 0, 0]]
        with self.assertRaises(ValueError):
            probe.original_second_sample(record, case)

    def test_u16_repeat_uses_full_alpha_channel(self):
        invisible = struct.pack("<4H", 65535, 123, 456, 0)
        visible = struct.pack("<4H", 65535, 123, 456, 256)
        raw = invisible + visible
        self.assertEqual(probe.canonical(raw, "U16"), bytes(8) + visible)
        self.assertEqual(raw[:8], invisible)

    def test_u8_repeat_keeps_visible_values(self):
        self.assertEqual(probe.canonical(bytes([1, 2, 3, 0, 4, 5, 6, 1]), "U8"),
                         bytes([0, 0, 0, 0, 4, 5, 6, 1]))


if __name__ == "__main__":
    unittest.main()
