"""CI-only validation of independent scatter artifact measurements."""
import base64
import math
from pathlib import Path
import sys
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from scatter_capture_analysis import analyze_series, components, decode, moment


class ScatterCaptureAnalysisTest(unittest.TestCase):
    def test_alpha_weighted_centroid_and_no_row_wrap(self):
        # Adjacent linear indexes across a row are not spatial neighbors.
        plane = bytes([0, 0, 0, 255, 255, 0, 0, 0])
        self.assertEqual(len(components(plane, 4, 2, "east")), 2)
        weighted = components(bytes([64, 192]), 2, 1, "east")[0]
        self.assertEqual(weighted["centroid"], [.75, 0])
        self.assertEqual(weighted["alphaMassCodes"], 256)
        self.assertEqual(weighted["nonzeroPixels"], 2)

    def test_north_components_follow_authored_direction(self):
        plane = bytearray(4 * 12)
        plane[1 * 4 + 2] = plane[10 * 4 + 2] = 255
        self.assertEqual([c["centroid"] for c in components(plane, 4, 12, "north")],
                         [[2, 10], [2, 1]])

    def test_source_varies_while_fallback_is_deterministic(self):
        width, height = 256, 16
        control = bytearray(width * height)
        for slot in range(32):
            control[8 * width + 4 + slot * 8] = 255
        planes = []
        for replicate in range(100):
            plane = bytearray(width * height)
            for slot in range(32):
                offset = 1 if (replicate >> (slot % 7)) & 1 else -1
                plane[(8 + offset) * width + 4 + slot * 8] = 255
            planes.append(bytes(plane))
        source = analyze_series(planes, control, width, height, "east", [256, 1024], [1249, 1024])
        candidate = analyze_series([bytes(control)] * 100, control, width, height,
                                   "east", [256, 1024], [1249, 1024])
        self.assertEqual(source["uniqueRasterCount"], 100)
        self.assertEqual(source["uniqueCentroidSequenceCount"], 100)
        self.assertGreater(source["laterDabs"]["perpendicularToAuthoredPath"]["rms"], .9)
        self.assertEqual(candidate["uniqueRasterCount"], 1)
        self.assertEqual(candidate["uniqueCentroidSequenceCount"], 1)
        self.assertEqual(candidate["laterDabs"]["perpendicularToAuthoredPath"]["rms"], 0)
        self.assertEqual(source["laterDabs"]["parallelToAuthoredPath"]["rms"], 0)
        self.assertAlmostEqual(source["globalOriginRadialDirectionHypothesisDegrees"],
                               math.degrees(math.atan2(1024, 256)))
        self.assertFalse(source["directionHypothesisQualified"])

    def test_reject_insufficient_or_merged_dabs(self):
        with self.assertRaisesRegex(ValueError, "100 independent"):
            analyze_series([bytes(4)] * 99, bytes(4), 2, 2, "east", [0, 0], [1, 0])
        with self.assertRaisesRegex(ValueError, "declared disconnected"):
            analyze_series([bytes([255])]*100, bytes([255]), 1, 1, "east", [0, 0], [1, 0])

    def test_strict_bounded_capture_stream(self):
        raw = bytes(1280 * 256)
        def evidence(blob):
            return {"rect": [128, 896, 1280, 256], "encoding": "zlib-base64-alpha8-row-major",
                    "data": base64.b64encode(blob).decode()}
        self.assertEqual(decode(evidence(zlib.compress(raw)), "east"), raw)
        for blob in (zlib.compress(raw[:-1]), zlib.compress(raw + b'x'),
                     zlib.compress(raw) + zlib.compress(b'x'), zlib.compress(raw)[:-1]):
            with self.subTest(length=len(blob)), self.assertRaises(ValueError):
                decode(evidence(blob), "east")
        with self.assertRaises(ValueError):
            decode(evidence(zlib.compress(raw)), "north")

    def test_moments_preserve_sign_and_reject_nonfinite(self):
        result = moment([-2, 0, 2])
        self.assertEqual(result["mean"], 0)
        self.assertEqual(result["sampleSD"], 2)
        self.assertAlmostEqual(result["rms"], math.sqrt(8/3))
        with self.assertRaises(ValueError):
            moment([float('nan')])


if __name__ == "__main__":
    unittest.main()
