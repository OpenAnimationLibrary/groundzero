"""CI-only validation of original generated-mask research input contracts."""
import base64
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generated_mask_analysis import RECT, captures, curve, png_crop, predict


def case(shape="circle", softness="0,0;1,1;"):
    return {"generatedTipProbe": {"id": "soft", "type": shape, "diameter": "16", "ratio": "1",
            "hfade": "0.5", "vfade": "0.5", "antialiasEdges": "1", "spikes": "2", "softness_curve": softness},
            "start": [1024, 1024], "end": [1025, 1024], "pressure": [1, 1],
            "opacity": 1, "flow": 1, "background": "transparent", "spacing": 1}


def notices(raw):
    data = base64.b64encode(zlib.compress(raw)).decode()
    return [{"preset": "source", "capture": "generated-tip-bgra", "rect": RECT,
             "encoding": "zlib-base64-bgra8", "offset": offset, "total": len(data), "data": data[offset:offset + 1800]}
            for offset in range(0, len(data), 1800)]


def runtime(items):
    return "\n".join("::notice::" + json.dumps(item) for item in items)


class GeneratedMaskAnalysisTest(unittest.TestCase):
    def test_curve_endpoint_values_and_natural_cubic_knots(self):
        C = curve("0,1;0.5,0.2;1,0;")
        self.assertEqual(C(0), 1)
        self.assertAlmostEqual(C(.5), .2)
        self.assertEqual(C(1), 0)
        # Barycentric products and their sum may round by one binary64 ULP.
        self.assertAlmostEqual(curve("0,0.75;1,0.75;")(.3), .75,
                               delta=math.ulp(.75))
        for value in ("0,0;0,1;", "0,0;1,nan;", "0.1,0;1,1;", "0,0;1,2;"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                curve(value)

    def test_independent_original_pixel_anchors(self):
        actual = predict(case(softness="0,0;0.25,0.75;1,1;"))
        for x, y, source in ((0, 0, 3), (4, 0, 191), (5, 0, 254), (7, 0, 251), (5, 5, 149)):
            self.assertLessEqual(abs(actual[(48 + y) * 96 + 48 + x] - source), 1)
        shifted = case("rect")
        shifted["start"] = [1024.125, 1024]
        shifted["end"] = [1025.125, 1024]
        actual = predict(shifted)
        # Captured asymmetry distinguishes the fixed source block boundary.
        for x, y, source in ((-7, -8, 78), (2, -8, 12), (7, -8, 166), (8, -8, 13)):
            self.assertLessEqual(abs(actual[(48 + y) * 96 + 48 + x] - source), 1)

    def test_constant_rectangle_limits_are_observations(self):
        self.assertFalse(any(predict(case("rect", "0,1;1,1;"))))
        half = predict(case("rect", "0,0.5;1,0.5;"))
        self.assertEqual(half[48 * 96 + 48], 16)
        self.assertEqual(half[40 * 96 + 41], 8)

    def test_research_domain_is_not_general_admission(self):
        for key, value in (("diameter", "32"), ("ratio", "0.5"), ("antialiasEdges", "0")):
            changed = case()
            changed["generatedTipProbe"][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                predict(changed)

    def test_original_chunks_reject_gaps_duplicates_and_overexpansion(self):
        raw = bytes(96 * 96 * 4)
        items = notices(raw)
        self.assertEqual(captures(runtime(items), ["source"])["source"], raw)
        with self.assertRaises(ValueError):
            captures(runtime(items + items), ["source"])
        wrong = dict(items[0], offset=1)
        with self.assertRaises(ValueError):
            captures(runtime([wrong]), ["source"])
        with self.assertRaises(ValueError):
            captures(runtime(notices(raw + b'x')), ["source"])
        with self.assertRaises(ValueError):
            captures("", ["source"])
        self.assertEqual(captures("", ["source"], allow_missing=True), {})

    def test_original_png_crop_preserves_alpha_and_rejects_corrupt_crc(self):
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
        raw = bytearray((b'\0' + bytes(2048 * 4)) * 2048)
        point = 1024 * (2048 * 4 + 1) + 1 + 1024 * 4
        raw[point:point + 4] = bytes([17, 83, 129, 207])
        png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 2048, 2048, 8, 6, 0, 0, 0)) +
               chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'original.png'
            path.write_bytes(png)
            result = png_crop(path)
            self.assertEqual(result[(48 * 96 + 48) * 4:(48 * 96 + 48) * 4 + 4], bytes([129, 83, 17, 207]))
            self.assertEqual(sum(result[3::4]), 207)
            path.write_bytes(png[:-1] + bytes([png[-1] ^ 1]))
            with self.assertRaises(ValueError):
                png_crop(path)


if __name__ == "__main__":
    unittest.main()
