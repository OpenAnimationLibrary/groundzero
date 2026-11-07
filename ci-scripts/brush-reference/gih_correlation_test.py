"""CI-only bounded original GIH causal-control regressions; no source app needed."""
import base64
import contextlib
import hashlib
import copy
import io
import json
import math
from pathlib import Path
import random
import tempfile
import struct
import unittest
import zipfile
import zlib
from xml.etree.ElementTree import Element, SubElement, tostring, fromstring
import oracle_cache as cache
from gih_correlation_contract import (
    WIDTH, HEIGHT, CONFIGURATIONS, BASE_CONFIGURATIONS, PHASE_CONFIGURATIONS, PHASE_GRIDS, GROUPS, REPLICATES, SLOTS, plan,
    validate_case, validate_manifest, validate_delivery, controls, capture, emit, closed_xml,
    validate_evidence, validate_execution)
from gih_correlation_fixtures import prepare


class GihCorrelationContract(unittest.TestCase):
    def fixtures(self, configuration):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "output").mkdir()
            (root / "paintoppresets").mkdir()
            written = []
            def writer(*args):
                written.append(args)
                return b"original source preset metadata placeholder"
            prepare(root, root, writer, "gih-correlation-" + configuration)
            fixtures = json.loads((root / "output/fixtures.json").read_text())
            first = fixtures["cases"][0]
            with zipfile.ZipFile(root / first["candidatePackage"]) as archive:
                self.assertEqual(archive.read("brushes/" + first["resourceProbe"]["filename"]),
                                 (root / "brushes" / first["resourceProbe"]["filename"]).read_bytes())
            self.assertEqual(len(written), REPLICATES + 2)
            self.assertTrue(all(args[1:3] == ("dry", False) for args in written))
        return fixtures

    def delivery(self, case):
        authored = validate_case(case)
        if authored["mode"] == "Node.paintLine":
            return {"mode": authored["mode"], "physicalTabletClaimed": False,
                    "events": [{"type": "line-start", "position": case["start"], "pressure": 1},
                               {"type": "line-end", "position": case["end"], "pressure": 1}]}
        return {"mode": authored["mode"], "vertices": case["sourcePath"],
                "singleContinuousStroke": True, "physicalTabletClaimed": False}

    def fields(self, case):
        return {**case["overrides"], "paintop": "paintbrush", "CompositeOp": "normal", "EraserMode": "false",
                "PaintOpAction": "1", "OpacityValue": "1", "FlowValue": "1",
                "OpacityUseCurve": "false", "FlowUseCurve": "false", "PressureRotation": "false",
                "PressureRatio": "false", "PressureScatter": "false", "PressureSpacing": "false",
                "Texture/Pattern/Enabled": "false", "MergedPaint": "false",
                "PaintOpSettings/isAirbrushing": "false", "PaintOpSettings/ignoreSpacing": "false",
                "PaintOpSettings/updateSpacingBetweenDabs": "false",
                "KisPrecisionOption/AutoPrecisionEnabled": "false", "KisPrecisionOption/precisionLevel": "5"}

    def resolution(self, case):
        return {"filename": case["resourceProbe"]["filename"],
                "sha256OriginalFile": case["resourceProbe"]["sha256"]}

    def evidence(self, case):
        expected = validate_case(case)
        left, top, width, height = expected["rect"]
        pixels = bytearray(width * height * 4)
        x, y = (math.floor(value) for value in expected["centers"][0])
        offset = ((y - top) * width + x - left) * 4
        pixels[offset:offset + 4] = bytes((0, 0, 255, 255))
        return {**controls(case, self.fields(case), self.resolution(case)),
                "configuration": expected["configuration"],
                "replicate": case["gihCorrelationProbe"]["replicate"],
                "stochastic": case["gihCorrelationProbe"]["stochastic"],
                "rect": expected["rect"], "encoding": "zlib-base64-bgra8-row-major",
                "data": base64.b64encode(zlib.compress(pixels)).decode("ascii"),
                "sha256Pixels": hashlib.sha256(pixels).hexdigest(),
                "frameSelections": [0] + [None] * (SLOTS - 1),
                "missingAuthoredSlots": SLOTS - 1, "actualPaintedTipReadback": True,
                "deliveredPath": self.delivery(case),
                "sourceSelectionModelVerified": False, "phaseGridEstablished": False, "pixelParityClaimed": False}

    def test_complete_independent_matrices_reuse_exact_original_resources(self):
        identities = {"random": set(), "incremental": set()}
        self.assertEqual(len(BASE_CONFIGURATIONS), 9)
        self.assertEqual(len(PHASE_CONFIGURATIONS), 28)
        self.assertEqual(len(GROUPS), 37)
        for configuration in CONFIGURATIONS:
            fixtures = self.fixtures(configuration)
            self.assertTrue(validate_manifest(fixtures))
            for case in fixtures["cases"]:
                resource = case["resourceProbe"]
                identities[resource["kind"]].add((resource["filename"], resource["sha256"], resource["md5"], resource["bytes"]))
                self.assertEqual(validate_delivery(case, self.delivery(case)), plan(configuration))
                controls(case, self.fields(case), self.resolution(case))
        self.assertEqual(len(identities["random"]), 1)
        self.assertEqual(len(identities["incremental"]), 1)

    def test_closed_actual_preset_and_control_xml_reject_coordinated_unknown_content(self):
        case = self.fixtures("path-tail")["cases"][0]
        fields = self.fields(case)
        evidence = self.evidence(case)
        helper = {"closed_xml": closed_xml, "controls": controls}
        def preset(values):
            return ('<?xml version="1.0" encoding="UTF-8"?>\n<Preset name="' + case["preset"] + '" paintopid="paintbrush">' +
                    ''.join('<param name="' + key + '"><![CDATA[' + value + ']]></param>' for key, value in values.items()) + '</Preset>').encode()
        row = {"resourceResolution": self.resolution(case), "gihCorrelationEvidence": evidence}
        cache.validate_gih_correlation_effective_preset(helper, case, row, preset(fields))
        for key in ("brush_definition", "MirrorSensor"):
            for defect in ("text", "tail", "namespace", "empty-namespace", "comment", "pi", "dtd"):
                values = dict(fields)
                tree = fromstring(values[key])
                if defect == "text":
                    tree.text = "unsupported"
                    values[key] = tostring(tree, encoding="unicode")
                elif defect == "tail":
                    if len(tree): tree[0].tail = "unsupported"
                    else: tree.tail = "unsupported"
                    values[key] = tostring(tree, encoding="unicode")
                elif defect == "namespace": values[key] = values[key].replace(" ", ' xmlns:u="urn:unsupported" ', 1)
                elif defect == "empty-namespace": values[key] = values[key].replace(" ", ' xmlns="" ', 1)
                elif defect == "comment": values[key] = "<!--unsupported-->" + values[key]
                elif defect == "pi": values[key] = "<?unsupported payload?>" + values[key]
                else: values[key] = "<!DOCTYPE " + tree.tag + " [<!ENTITY x 'unsupported'>]>" + values[key]
                forged = copy.deepcopy(row)
                if key == "brush_definition": forged["gihCorrelationEvidence"]["effectiveBrushDefinition"] = values[key]
                else: forged["gihCorrelationEvidence"]["effectiveControls"][key] = values[key]
                data = preset(values); forged["effectivePresetSha256"] = cache.digest(data)
                with self.subTest(key=key, defect=defect), self.assertRaises(ValueError):
                    cache.validate_gih_correlation_effective_preset(helper, case, forged, data)
        data = preset(fields)
        for inserted in (b'<!--unsupported-->', b'<?unsupported payload?>', b'unsupported'):
            altered = data.replace(b'</param>', inserted + b'</param>', 1)
            with self.subTest(inserted=inserted), self.assertRaises(ValueError):
                cache.validate_gih_correlation_effective_preset(helper, case, row, altered)

    def test_paths_isolate_partition_direction_translation_and_terminal_continuation(self):
        base = plan("path-tail")
        self.assertEqual(len(base["centers"]), SLOTS)
        for configuration in CONFIGURATIONS:
            authored = plan(configuration)
            length = sum(math.hypot(b[0] - a[0], b[1] - a[1])
                         for a, b in zip(authored["vertices"], authored["vertices"][1:]))
            self.assertEqual(length, 992 + authored["terminalContinuation"])
            self.assertLessEqual(len(authored["vertices"]), 64)
            self.assertTrue(all(16 <= v < WIDTH - 16 for point in authored["vertices"] for v in point))
            left, top, width, height = authored["rect"]
            self.assertGreaterEqual(left, 0)
            self.assertGreaterEqual(top, 0)
            self.assertLessEqual(left + width, WIDTH)
            self.assertLessEqual(top + height, HEIGHT)
            self.assertLessEqual(width * height, 1024 * 1024)
        self.assertEqual(plan("line-exact")["vertices"][-1], base["centers"][-1])
        self.assertEqual(plan("line-tail")["vertices"], base["vertices"])
        self.assertGreater(len(plan("dabs-tail")["vertices"]), len(base["vertices"]))
        self.assertGreater(len(plan("halves-tail")["vertices"]), len(plan("dabs-tail")["vertices"]))
        self.assertEqual(plan("translated-tail")["centers"], [[x + 13, y + 19] for x, y in base["centers"]])
        self.assertEqual(plan("west-tail")["centers"], list(reversed(base["centers"])))

    def test_fractional_phase_heldouts_cover_every_boundary_axis_and_partition(self):
        for axis in ("x", "y"):
            for grid in PHASE_GRIDS:
                coarse = plan("phase-" + axis + "-g" + str(grid).zfill(3) + "-coarse")
                divided = plan("phase-" + axis + "-g" + str(grid).zfill(3) + "-dabs")
                spec = coarse["phaseBoundary"]
                self.assertEqual(spec, {"axis": axis, "gridBoundary": grid, "offset": 1 / (2 * grid),
                                        "partition": "coarse", "sourceGridEstablished": False})
                self.assertEqual(coarse["mode"], "Node.paintPath")
                self.assertEqual(coarse["centers"], divided["centers"])
                self.assertEqual(len(coarse["vertices"]), 2)
                self.assertEqual(len(divided["vertices"]), 33)
                value = coarse["centers"][0][axis == "y"]
                phase = (value + (.5 if axis == "x" else 0)) % 1
                self.assertEqual((phase * grid) % 1, .5)
                # Exact dyadic inputs land on the requested grid's rounding
                # boundary; finer powers of two land at bucket interiors.
                for finer in PHASE_GRIDS:
                    if finer > grid: self.assertEqual((phase * finer) % 1, 0)
        fixture = self.fixtures("phase-y-g128-coarse")
        case = fixture["cases"][0]
        evidence = self.evidence(case)
        self.assertTrue(validate_evidence(case, evidence))
        self.assertIs(evidence["phaseGridEstablished"], False)
        broken = copy.deepcopy(fixture)
        broken["cases"][0]["phaseBoundaryProbe"]["sourceGridEstablished"] = True
        with self.assertRaises(ValueError): validate_manifest(broken)

    def test_mutated_manifest_resource_paths_and_delivery_fail(self):
        original = self.fixtures("path-tail")
        for defect in ("missing", "duplicate", "hash", "all-hashes", "path", "shuffle"):
            fixture = copy.deepcopy(original)
            if defect == "missing":
                fixture["cases"].pop(); fixture["presets"].pop()
            elif defect == "duplicate":
                fixture["cases"][0]["gihCorrelationProbe"]["replicate"] = 1
            elif defect == "hash":
                fixture["cases"][0]["resourceProbe"]["sha256"] = "f" * 64
            elif defect == "all-hashes":
                for case in fixture["cases"]: case["resourceProbe"]["sha256"] = "f" * 64
            elif defect == "path":
                fixture["cases"][0]["sourcePath"][0][0] += 1
            else:
                fixture["randomizeExecution"] = False
            with self.assertRaises(ValueError): validate_manifest(fixture)
        case = original["cases"][0]
        for key, value in (("vertices", [[0, 0], [1, 1]]), ("mode", "Node.paintLine"),
                           ("physicalTabletClaimed", True), ("singleContinuousStroke", False)):
            with self.assertRaises(ValueError): validate_delivery(case, {**self.delivery(case), key: value})
        with self.assertRaises(ValueError):
            controls(case, self.fields(case), {**self.resolution(case), "sha256OriginalFile": "f" * 64})
        for key, value in (("PressureSize", "true"), ("PressureMirror", "true"),
                           ("Pressureh", "true"), ("FlowValue", "nan"), ("MirrorSensor", "<broken"),
                           ("PaintOpSettings/isAirbrushing", "true")):
            with self.assertRaises(ValueError): controls(case, {**self.fields(case), key: value}, self.resolution(case))

    def test_capture_retains_missing_endpoint_labels_without_claiming_a_model(self):
        case = self.fixtures("line-exact")["cases"][0]
        before = bytes(WIDTH * HEIGHT * 4)
        after = bytearray(before)
        x, y = case["correlationCenters"][0]
        offset = (y * WIDTH + x) * 4
        after[offset:offset + 4] = bytes((0, 0, 255, 255))
        evidence = capture(before, after, WIDTH, case, self.fields(case), self.delivery(case), self.resolution(case))
        self.assertTrue(validate_evidence(case, evidence))
        self.assertEqual(evidence["frameSelections"], [0] + [None] * 31)
        self.assertEqual(evidence["missingAuthoredSlots"], 31)
        self.assertIs(evidence["sourceSelectionModelVerified"], False)
        self.assertIs(evidence["pixelParityClaimed"], False)
        output = io.StringIO()
        with contextlib.redirect_stdout(output): emit(case, evidence)
        self.assertTrue(all(len(line) < 4000 for line in output.getvalue().splitlines()))
        for broken in (bytes(len(before) - 1), before):
            with self.assertRaises(ValueError):
                capture(before, broken, WIDTH, case, self.fields(case), self.delivery(case), self.resolution(case))
        after[3] = 255
        with self.assertRaisesRegex(ValueError, "escaped"):
            capture(before, after, WIDTH, case, self.fields(case), self.delivery(case), self.resolution(case))

    def test_retained_evidence_labels_pixels_controls_and_bounds_are_verified(self):
        case = self.fixtures("path-tail")["cases"][0]
        original = self.evidence(case)
        self.assertTrue(validate_evidence(case, original))
        for key, value in (("frameSelections", [1] + [None] * (SLOTS - 1)),
                           ("missingAuthoredSlots", 0), ("sha256Pixels", "f" * 64),
                           ("rect", [0, 0, 1, 1]), ("configuration", "line-exact"),
                           ("replicate", 1), ("stochastic", False),
                           ("actualPaintedTipReadback", False), ("pixelParityClaimed", True),
                           ("data", "invalid base64!")):
            with self.assertRaises(ValueError): validate_evidence(case, {**original, key: value})
        broken = copy.deepcopy(original)
        broken["effectiveControls"]["PressureSize"] = "true"
        with self.assertRaises(ValueError): validate_evidence(case, broken)
        pixels = zlib.decompress(base64.b64decode(original["data"]))
        for malformed in (pixels + b"extra", pixels[:-1]):
            encoded = base64.b64encode(zlib.compress(malformed)).decode("ascii")
            with self.assertRaises(ValueError): validate_evidence(case, {**original, "data": encoded})
        encoded = base64.b64encode(base64.b64decode(original["data"]) + b"trailing").decode("ascii")
        with self.assertRaises(ValueError): validate_evidence(case, {**original, "data": encoded})

    def report(self, fixture):
        order = list(range(REPLICATES + 2))
        random.Random(0x47494831).shuffle(order)
        planned = [None] * len(order)
        rows = []
        for index, (ordinal, case) in enumerate(zip(order, fixture["cases"])):
            evidence = self.evidence(case)
            rows.append({**case, "executionOrdinal": ordinal, "intendedOrdinal": index,
                         "gihCorrelationEvidence": evidence, "resourceReadback": evidence,
                         "resourceResolution": self.resolution(case),
                         "input": evidence["deliveredPath"]["mode"], "deliveryReport": evidence["deliveredPath"]})
            planned[ordinal] = case["preset"]
        sequences = [row["gihCorrelationEvidence"]["frameSelections"] for row in rows
                     if row["gihCorrelationProbe"]["stochastic"]]
        labels = [value for sequence in sequences for value in sequence]
        return {"status": "complete", "version": fixture["version"], "records": rows,
                "intendedPresetOrder": fixture["presets"], "plannedExecutionOrder": planned,
                "actualExecutionOrder": list(planned), "resourceStatistics": {
                    "strokes": 100, "sourceSequences": sequences,
                    "observedDabs": sum(value is not None for value in labels),
                    "missingAuthoredSlots": labels.count(None),
                    "frameCounts": [labels.count(value) for value in range(3)],
                    "seedParityClaimed": False, "uniformityAssumed": False}}

    def effective_xml(self, case):
        preset = Element("Preset", name=case["preset"], paintopid="paintbrush")
        for key, value in self.fields(case).items():
            SubElement(preset, "param", name=key).text = value
        return tostring(preset)

    def test_cache_binds_actual_order_resource_summary_and_effective_xml(self):
        fixture = self.fixtures("path-tail")
        report = self.report(fixture)
        helper = cache.validate_gih_correlation_acquisition(fixture, report)
        case, row = fixture["cases"][0], report["records"][0]
        xml = self.effective_xml(case)
        cache.validate_gih_correlation_effective_preset(helper, case, row, xml)
        for defective in (xml.replace(b">false<", b">true<", 1), b"<broken", xml.replace(b"paintbrush", b"colorsmudge")):
            with self.assertRaises(ValueError): cache.validate_gih_correlation_effective_preset(helper, case, row, defective)
        duplicate = xml.replace(b"</Preset>", b'<param name="PressureSize">false</param></Preset>')
        with self.assertRaises(ValueError): cache.validate_gih_correlation_effective_preset(helper, case, row, duplicate)
        for defect in ("actual-order", "intended-ordinal", "execution-ordinal", "summary", "delivery"):
            broken = copy.deepcopy(report)
            if defect == "actual-order": broken["actualExecutionOrder"] = fixture["presets"]
            elif defect == "intended-ordinal": broken["records"][0]["intendedOrdinal"] = 1
            elif defect == "execution-ordinal":
                broken["plannedExecutionOrder"] = list(reversed(broken["plannedExecutionOrder"]))
                broken["actualExecutionOrder"] = broken["plannedExecutionOrder"]
            elif defect == "summary": broken["resourceStatistics"]["observedDabs"] -= 1
            else: broken["records"][0]["deliveryReport"] = {}
            with self.assertRaises(ValueError): cache.validate_gih_correlation_acquisition(fixture, broken)

    def test_complete_cache_roundtrip_keeps_random_and_incremental_repeat_contracts(self):
        fixture = self.fixtures("path-tail")
        report = self.report(fixture)
        # One original red observation, matching both the PNG and retained BGRA.
        raw = bytearray(WIDTH * HEIGHT * 4)
        x, y = fixture["cases"][0]["correlationCenters"][0]
        offset = (y * WIDTH + x) * 4
        raw[offset:offset + 4] = bytes((0, 0, 255, 255))
        raw_hash = hashlib.sha256(raw).hexdigest()
        raw[offset:offset + 4] = bytes((255, 0, 0, 255))
        scanlines = b"".join(b"\0" + raw[y * WIDTH * 4:(y + 1) * WIDTH * 4] for y in range(HEIGHT))
        def chunk(kind, content):
            return struct.pack(">I", len(content)) + kind + content + struct.pack(">I", zlib.crc32(kind + content))
        png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 6, 0, 0, 0)) +
               chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / "fresh"
            output = root / "output"
            output.mkdir(parents=True)
            for case, row in zip(fixture["cases"], report["records"]):
                stem = case["preset"].replace(" ", "_")
                repeats = 2 if case["variant"] == "baseline" else 1
                xml = self.effective_xml(case)
                row.update(image=stem + ".png", effectivePresetXml=stem + ".effective.xml",
                           effectivePresetSha256=cache.digest(xml), sha256Pixels=raw_hash,
                           sha256RepeatPixels=raw_hash, repeatSha256Pixels=[raw_hash] * repeats,
                           repeatSemantics="transparent-rgb-zero-v1", repeatSha256CanonicalPixels=[raw_hash] * repeats,
                           repeatDifferences=[{"hiddenRgbOnlyPixels": 0, "visibleOrAlphaChangedPixels": 0}] * (repeats - 1),
                           repeatImages=[], profile="sRGB-elle-V2-srgbtrc.icc", changed=True,
                           effectiveView={"diameter": 8, "opacity": 1, "flow": 1, "rotation": 0,
                                          "blend": "normal", "eraser": False, "alphaLock": False, "disablePressure": False})
                (output / row["image"]).write_bytes(png)
                (output / row["effectivePresetXml"]).write_bytes(xml)
                if repeats == 2:
                    repeat_name = stem + ".repeat-1.png"
                    (output / repeat_name).write_bytes(png)
                    row["repeatImages"] = [{"image": repeat_name, "sha256File": cache.digest(png)}]
            (output / "fixtures.json").write_bytes(cache.canonical(fixture))
            (output / "oracle-status.json").write_bytes(cache.canonical(report))
            captured = cache.acquisition(output, fixture)
            self.assertEqual(len(captured), 1 + 2 * (REPLICATES + 2) + 2)
            broken = copy.deepcopy(report)
            broken["records"][-1]["repeatSha256Pixels"].pop()
            (output / "oracle-status.json").write_bytes(cache.canonical(broken))
            with self.assertRaisesRegex(ValueError, "repeat gate"):
                cache.acquisition(output, fixture)
            (output / "oracle-status.json").write_bytes(cache.canonical(report))
            destination = Path(folder) / "cache"
            identity = {"schema": cache.VERSION, "group": fixture["group"], "applicationSha256": "a" * 64}
            cache.seal(root, destination, identity)
            for name in captured: (output / name).unlink()
            cache.restore(root, destination, identity)
            restored = cache.parse((output / "oracle-status.json").read_bytes())
            self.assertEqual(restored, report)
            self.assertEqual(len(restored["records"][0]["repeatSha256Pixels"]), 1)
            self.assertEqual(len(restored["records"][-1]["repeatSha256Pixels"]), 2)

    def test_actual_randomized_execution_and_all_stroke_identities_are_required(self):
        fixture = self.fixtures("path-tail")
        order = list(range(REPLICATES + 2))
        random.Random(0x47494831).shuffle(order)
        rows = []
        for ordinal, case in zip(order, fixture["cases"]):
            evidence = self.evidence(case)
            rows.append({**case, "executionOrdinal": ordinal, "gihCorrelationEvidence": evidence,
                         "resourceReadback": evidence, "resourceResolution": self.resolution(case),
                         "input": evidence["deliveredPath"]["mode"], "deliveryReport": evidence["deliveredPath"]})
        self.assertTrue(validate_execution(fixture, rows))
        broken = copy.deepcopy(rows)
        for index, row in enumerate(broken): row["executionOrdinal"] = index
        with self.assertRaises(ValueError): validate_execution(fixture, broken)
        broken = copy.deepcopy(rows)
        broken[0]["gihCorrelationEvidence"]["pixelParityClaimed"] = True
        with self.assertRaises(ValueError): validate_execution(fixture, broken)
        broken = copy.deepcopy(rows)
        broken[0]["resourceResolution"]["sha256OriginalFile"] = "f" * 64
        with self.assertRaises(ValueError): validate_execution(fixture, broken)
        broken = copy.deepcopy(rows)
        broken[0]["resourceReadback"] = {}
        with self.assertRaises(ValueError): validate_execution(fixture, broken)
        with self.assertRaises(ValueError): validate_execution(fixture, rows[:-1])


if __name__ == "__main__":
    unittest.main()
