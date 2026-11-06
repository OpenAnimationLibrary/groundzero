"""CI-only regression for source-oracle integrity; no reference app required."""
import os
import base64
import copy
import random
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock
import zlib
import zipfile

import oracle_cache as cache


def png():
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(b"\0\xff\0\0\xff")) + chunk(b"IEND", b""))


class OracleCacheTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "fresh"
        self.output = self.root / "output"
        self.output.mkdir(parents=True)
        self.destination = Path(self.temp.name) / "cached"
        self.case = {"preset": "Original dry", "variant": "baseline", "diameter": 1,
                     "opacity": 1, "flow": 1}
        self.fixtures = {"license": "CC0-1.0", "version": "5.3.4.1", "group": "test",
                         "width": 1, "height": 1, "presets": [self.case["preset"]],
                         "cases": [self.case]}
        self.expected = {"schema": cache.VERSION, "applicationSha256": "a" * 64, "group": "test"}
        self.xml = b'<Preset paintopid="paintbrush"/>'
        self.row = {**self.case, "image": "Original_dry.png", "effectivePresetXml": "Original_dry.effective.xml",
                    "effectivePresetSha256": cache.digest(self.xml), "sha256Pixels": "b" * 64,
                    "repeatSha256Pixels": ["b" * 64] * 2,
                    "repeatSemantics": "transparent-rgb-zero-v1", "sha256RepeatPixels": "d" * 64,
                    "repeatSha256CanonicalPixels": ["d" * 64] * 2,
                    "repeatDifferences": [{"hiddenRgbOnlyPixels": 0, "visibleOrAlphaChangedPixels": 0}],
                    "repeatImages": [{"image": "Original_dry.repeat-1.png", "sha256File": cache.digest(png())}], "profile": "sRGB-elle-V2-srgbtrc.icc",
                    "changed": True, "effectiveView": {"diameter": 1, "opacity": 1, "flow": 1,
                    "rotation": 0, "blend": "normal", "eraser": False, "alphaLock": False,
                    "disablePressure": False}}
        self.status = {"status": "complete", "version": "5.3.4.1", "records": [self.row]}
        (self.output / "fixtures.json").write_bytes(cache.canonical(self.fixtures))
        (self.output / "oracle-status.json").write_bytes(cache.canonical(self.status))
        (self.output / "Original_dry.effective.xml").write_bytes(self.xml)
        (self.output / "Original_dry.png").write_bytes(png())
        (self.output / "Original_dry.repeat-1.png").write_bytes(png())

    def test_acquisition_scripts_exclude_only_known_generation_and_tests(self):
        source = Path(self.temp.name) / "scripts"
        source.mkdir()
        names = ("hsv_joint_fixtures.py", "oracle_cache_test.py", "prepare.py",
                 "unknown_fixtures.py", "gzbrush_reference/sensor_probe.py")
        for name in names:
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"original")
        baseline = cache.acquisition_scripts(source)
        for name in names[:2]:
            (source / name).write_bytes(b"unrelated edit")
            self.assertEqual(baseline, cache.acquisition_scripts(source))
        for name in names[2:]:
            (source / name).write_bytes(b"acquisition edit")
            self.assertNotEqual(baseline, cache.acquisition_scripts(source))
            (source / name).write_bytes(b"original")
        before = cache.inventory(self.output)
        (self.output / "fixtures.json").write_bytes(b"changed generated input")
        self.assertNotEqual(before, cache.inventory(self.output))

    def test_matrix_only_edits_do_not_hide_acquisition_changes(self):
        workflow = (b"    runs-on: ubuntu-22.04\n"
                    b"        fixture_group: ${{ fromJSON('[one]') }}\n"
                    b"    env: {LANG: C}\n"
                    b"        run: xvfb-run source-app\n"
                    b"      - name: Seal complete source acquisition\n"
                    b"        run: validate-and-seal\n"
                    b"      - name: Report source parameter probes\n"
                    b"        run: report-metadata\n")
        baseline = cache.acquisition_workflow(workflow)
        self.assertEqual(baseline, cache.acquisition_workflow(
            workflow.replace(b"[one]", b"[one,two]")))
        self.assertEqual(baseline, cache.acquisition_workflow(
            workflow.replace(b"report-metadata", b"expanded-report-metadata")))
        for old, new in ((b"validate-and-seal", b"changed-validation"),
                         (b"ubuntu-22.04", b"ubuntu-24.04"),
                         (b"LANG: C", b"LANG: changed"),
                         (b"xvfb-run", b"changed-render")):
            self.assertNotEqual(baseline, cache.acquisition_workflow(
                workflow.replace(old, new)))
        for malformed in (workflow.replace(b"fixture_group:", b"other:"),
                          workflow + b"        fixture_group: ${{ duplicate }}\n",
                          workflow.replace(b" }}", b"\n          }}"),
                          workflow.replace(b"Seal complete source acquisition", b"missing seal"),
                          workflow.replace(b"Report source parameter probes", b"missing boundary"),
                          workflow + b"      - name: Report source parameter probes\n"):
            with self.assertRaises(ValueError):
                cache.acquisition_workflow(malformed)

    def sealed(self):
        cache.seal(self.root, self.destination, self.expected)
        for name in ("oracle-status.json", "Original_dry.effective.xml", "Original_dry.png", "Original_dry.repeat-1.png"):
            (self.output / name).unlink()

    def rejected(self, expected=None):
        before = cache.inventory(self.output)
        with self.assertRaises((ValueError, OSError, KeyError, TypeError)):
            cache.restore(self.root, self.destination, expected or self.expected)
        self.assertEqual(cache.inventory(self.output), before)

    def test_signed_precision_capture_names_seal_and_restore(self):
        # Original boundary fixtures include signed deltas such as +0000.
        name = "Original dry precision +0000"
        self.case["preset"] = name
        self.fixtures["presets"] = [name]
        self.row["preset"] = name
        stem = name.replace(" ", "_")
        renames = {
            "Original_dry.png": stem + ".png",
            "Original_dry.effective.xml": stem + ".effective.xml",
            "Original_dry.repeat-1.png": stem + ".repeat-1.png"}
        for old, new in renames.items():
            (self.output / old).rename(self.output / new)
        self.row["image"] = stem + ".png"
        self.row["effectivePresetXml"] = stem + ".effective.xml"
        self.row["repeatImages"][0]["image"] = stem + ".repeat-1.png"
        (self.output / "fixtures.json").write_bytes(cache.canonical(self.fixtures))
        (self.output / "oracle-status.json").write_bytes(cache.canonical(self.status))
        cache.seal(self.root, self.destination, self.expected)
        for path in [self.output / "oracle-status.json"] + [self.output / n for n in renames.values()]:
            path.unlink()
        cache.restore(self.root, self.destination, self.expected)
        self.assertTrue(all((self.output / n).is_file() for n in renames.values()))
        self.assertEqual(cache.parse((self.output / "oracle-status.json").read_bytes())["records"][0]["preset"], name)
        for unsafe in ("../original+1.png", "sub/original+1.png", "sub\\original+1.png",
                       "/original+1.png", "C:original+1.png", "original+1.png:stream",
                       "original+1.png\n", "original+1.png ", "original%2B1.png"):
            with self.subTest(name=unsafe), self.assertRaises(ValueError):
                cache.filename(unsafe, ".png")

    def test_complete_restore_excludes_candidate_and_bundled_assets(self):
        (self.output / "comparison.json").write_text("candidate failure")
        (self.output / "bundled.kpp").write_text("must not persist")
        self.sealed()
        self.assertEqual({p.name for p in self.destination.iterdir()},
                         {"manifest.json", "oracle-status.json", "Original_dry.effective.xml", "Original_dry.png", "Original_dry.repeat-1.png"})
        cache.restore(self.root, self.destination, self.expected)
        self.assertEqual((self.output / "Original_dry.png").read_bytes(), png())
        self.assertEqual((self.output / "comparison.json").read_text(), "candidate failure")

    def test_late_write_failure_rolls_back_every_new_output(self):
        self.sealed()
        before = cache.inventory(self.output)
        ordinary_open = Path.open
        writes = 0

        class FailingWrite:
            def __init__(self, stream):
                self.stream = stream

            def __enter__(self):
                return self

            def __exit__(self, *args):
                self.stream.close()

            def write(self, data):
                self.stream.write(data[:8])
                raise OSError("injected failure after a partial second capture")

        def intercept(path, mode="r", *args, **kwargs):
            nonlocal writes
            stream = ordinary_open(path, mode, *args, **kwargs)
            if mode == "xb" and path.parent == self.output:
                writes += 1
                if writes == 2:
                    return FailingWrite(stream)
            return stream

        with mock.patch.object(Path, "open", intercept):
            with self.assertRaises(OSError):
                cache.restore(self.root, self.destination, self.expected)
        self.assertEqual(writes, 2)
        self.assertEqual(cache.inventory(self.output), before)
        # Normal acquisition/reuse can now start with only fresh input files.
        cache.restore(self.root, self.destination, self.expected)

    def test_cpu_dispatch_identity_excludes_identifying_and_transient_fields(self):
        cpu = ("vendor_id : GenuineIntel\ncpu family : 6\nmodel : 85\n"
               "stepping : 7\nmicrocode : 0x5003604\nflags : sse avx2 avx\n")
        first = cache.cpu_dispatch_identity(cpu +
            "processor : 0\nserial : private-a\ncpu MHz : 1200\n")
        second = cache.cpu_dispatch_identity(cpu +
            "processor : 55\nserial : private-b\ncpu MHz : 3300\n")
        self.assertEqual(first, second)
        self.assertNotIn(b"private", cache.canonical(first))
        self.assertNotEqual(first, cache.cpu_dispatch_identity(cpu.replace("avx2", "avx512f")))
        self.assertNotEqual(first, cache.cpu_dispatch_identity(cpu.replace("model : 85", "model : 106")))
        with self.assertRaises(ValueError):
            cache.cpu_dispatch_identity("serial : private\n")

    def test_stale_binary_or_inputs(self):
        self.sealed()
        self.rejected({**self.expected, "applicationSha256": "c" * 64})
        self.rejected({**self.expected, "group": "other"})

    def test_corrupt_missing_and_unexpected(self):
        self.sealed()
        path = self.destination / "Original_dry.png"
        original = path.read_bytes()
        path.write_bytes(original[:-1] + b"x")
        self.rejected()
        path.unlink()
        self.rejected()
        path.write_bytes(original)
        (self.destination / "comparison.json").write_text("not source evidence")
        self.rejected()

    def test_symlink_fifo_hardlink(self):
        self.sealed()
        path = self.destination / "Original_dry.effective.xml"
        path.unlink()
        target = Path(self.temp.name) / "target.xml"
        target.write_bytes(self.xml)
        path.symlink_to(target)
        self.rejected()
        path.unlink()
        os.mkfifo(path)
        self.rejected()
        path.unlink()
        os.link(target, path)
        self.rejected()

    def test_partial_or_failed_acquisition_cannot_be_sealed(self):
        for status in ("running", "failed"):
            self.status["status"] = status
            (self.output / "oracle-status.json").write_bytes(cache.canonical(self.status))
            with self.assertRaises(ValueError):
                cache.seal(self.root, self.destination, self.expected)
            self.assertFalse(self.destination.exists())

    def test_repeat_and_control_failures_cannot_be_sealed(self):
        for change in ({"repeatSha256Pixels": ["b" * 64]},
                       {"repeatSha256CanonicalPixels": ["d" * 64, "c" * 64]},
                       {"repeatSemantics": "unversioned"},
                       {"repeatImages": []},
                       {"repeatDifferences": [{"hiddenRgbOnlyPixels": 0, "visibleOrAlphaChangedPixels": 1}]},
                       {"effectiveView": {**self.row["effectiveView"], "eraser": True}},
                       {"effectiveView": {**self.row["effectiveView"], "opacity": .5}},
                       {"changed": False}):
            status = {**self.status, "records": [{**self.row, **change}]}
            (self.output / "oracle-status.json").write_bytes(cache.canonical(status))
            with self.assertRaises(ValueError):
                cache.seal(self.root, self.destination, self.expected)
            self.assertFalse(self.destination.exists())

    def test_hidden_only_raw_repeat_difference_retains_original_capture(self):
        self.row["repeatSha256Pixels"][1] = "c" * 64
        self.row["repeatDifferences"][0]["hiddenRgbOnlyPixels"] = 1
        (self.output / "oracle-status.json").write_bytes(cache.canonical(self.status))
        self.sealed()
        cache.restore(self.root, self.destination, self.expected)
        self.assertEqual((self.output / "Original_dry.repeat-1.png").read_bytes(), png())

    def test_repeat_pixel_contract(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("repeat_pixels", Path(__file__).parent / "gzbrush_reference/repeat_pixels.py")
        pixels = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(pixels)
        transparent = bytes([0, 0, 0, 0])
        hidden = bytes([255, 17, 3, 0])
        self.assertEqual(pixels.canonical_hash(transparent), pixels.canonical_hash(hidden))
        self.assertEqual(pixels.differences(transparent, hidden),
                         {"hiddenRgbOnlyPixels": 1, "visibleOrAlphaChangedPixels": 0})
        for first, second in ((transparent, bytes([0, 0, 0, 1])),
                              (bytes([0, 0, 0, 1]), bytes([1, 0, 0, 1])),
                              (transparent + bytes([0, 0, 0, 255]), hidden + bytes([0, 1, 0, 255]))):
            self.assertNotEqual(pixels.canonical_hash(first), pixels.canonical_hash(second))
            self.assertEqual(pixels.differences(first, second)["visibleOrAlphaChangedPixels"], 1)

    def spacing_scatter_source(self):
        from spacing_scatter_fixtures import prepare
        from runpy import run_path
        probe = run_path(str(Path(__file__).parent / "gzbrush_reference/spacing_scatter_probe.py"))
        self.root = Path(self.temp.name) / "scatter-fresh"
        self.output = self.root / "output"
        self.output.mkdir(parents=True)
        (self.root / "paintoppresets").mkdir()
        group = "scatter-xy-east"
        prepare(self.root, self.root, lambda *args: b"original metadata placeholder", group)
        self.fixtures = cache.parse((self.output / "fixtures.json").read_bytes())
        self.expected = {**self.expected, "group": group}
        # These are synthetic cache-schema fixtures. They are not source pixels
        # or evidence that the real brush renderer produced a particular mark.
        def full_png():
            def chunk(kind, data):
                return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
            pixels = (b"\0" + bytes(2048 * 4)) * 2048
            return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2048, 2048, 8, 6, 0, 0, 0)) +
                    chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))
        image = full_png()
        pixels = bytearray(1280 * 256 * 4)
        pixels[(128 * 1280 + 128) * 4 + 3] = 255
        encoded = base64.b64encode(zlib.compress(pixels)).decode()
        order = list(self.fixtures["presets"])
        random.Random(0x47494831).shuffle(order)
        ordinal = {name: index for index, name in enumerate(order)}
        rows = []
        for index, case in enumerate(self.fixtures["cases"]):
            name = case["preset"]
            stem = name.replace(" ", "_")
            delivery = {"mode": "Node.paintLine", "events": [
                {"type": "line-start", "position": case["start"], "pressure": 1},
                {"type": "line-end", "position": case["end"], "pressure": 1}]}
            fields = {**case["overrides"], "paintop": "paintbrush", "CompositeOp": "normal"}
            evidence = probe["validate_controls"](case, fields, delivery)
            evidence.update(encoding="zlib-base64-bgra8-row-major", data=encoded,
                            sha256Pixels=cache.digest(pixels), nonzeroAlphaPixels=1)
            count = 1 if case["scatterProbe"]["enabled"] else 2
            row = {**copy.deepcopy(self.row), **case,
                "image": stem + ".png", "effectivePresetXml": stem + ".effective.xml",
                "effectivePresetSha256": cache.digest(self.xml),
                "effectiveView": {**self.row["effectiveView"], "diameter": 8},
                "intendedOrdinal": index, "executionOrdinal": ordinal[name],
                "deliveryReport": delivery, "scatterEvidence": evidence,
                "repeatSha256Pixels": ["b" * 64] * count,
                "repeatSha256CanonicalPixels": ["d" * 64] * count,
                "repeatDifferences": [{"hiddenRgbOnlyPixels": 0, "visibleOrAlphaChangedPixels": 0}] * (count - 1),
                "repeatImages": [{"image": stem + ".repeat-1.png", "sha256File": cache.digest(image)}] if count == 2 else []}
            (self.output / row["image"]).write_bytes(image)
            (self.output / row["effectivePresetXml"]).write_bytes(self.xml)
            if count == 2:
                (self.output / row["repeatImages"][0]["image"]).write_bytes(image)
            rows.append(row)
        self.status = {"status": "complete", "version": "5.3.4.1", "records": rows,
                       "intendedPresetOrder": self.fixtures["presets"],
                       "plannedExecutionOrder": order, "actualExecutionOrder": list(order)}
        (self.output / "oracle-status.json").write_bytes(cache.canonical(self.status))

    def test_complete_scatter_cache_retains_independent_sample_policy(self):
        self.spacing_scatter_source()
        files = cache.acquisition(self.output, self.fixtures)
        self.assertEqual(len(files), 207)  # status + 102 images/XML + 2 deterministic repeats
        cache.seal(self.root, self.destination, self.expected)
        for name in files:
            (self.output / name).unlink()
        cache.restore(self.root, self.destination, self.expected)
        restored = cache.parse((self.output / "oracle-status.json").read_bytes())
        self.assertEqual(restored, self.status)
        self.assertTrue(all(len(row["repeatSha256Pixels"]) == 1
                            for row in restored["records"][:100]))
        self.assertTrue(all(len(row["repeatSha256Pixels"]) == 2
                            for row in restored["records"][100:]))

    def test_scatter_cache_rejects_counts_order_controls_and_pixel_evidence(self):
        self.spacing_scatter_source()
        for mutation in ("count", "replicate", "planned", "actual", "not-random", "ordinal",
                         "intended", "record-order", "controls", "delivery", "hash", "pixels",
                         "disabled-repeat"):
            with self.subTest(mutation=mutation):
                fixtures, status = copy.deepcopy(self.fixtures), copy.deepcopy(self.status)
                if mutation == "count":
                    fixtures["cases"].pop(); fixtures["presets"].pop(); status["records"].pop()
                elif mutation == "replicate":
                    fixtures["cases"][0]["scatterProbe"]["replicate"] = 1
                    status["records"][0]["scatterProbe"]["replicate"] = 1
                elif mutation == "planned": status["plannedExecutionOrder"][0] = status["plannedExecutionOrder"][1]
                elif mutation == "actual": status["actualExecutionOrder"].reverse()
                elif mutation == "not-random":
                    status["plannedExecutionOrder"] = list(fixtures["presets"])
                    status["actualExecutionOrder"] = list(fixtures["presets"])
                    for index, row in enumerate(status["records"]): row["executionOrdinal"] = index
                elif mutation == "ordinal": status["records"][0]["executionOrdinal"] = status["records"][1]["executionOrdinal"]
                elif mutation == "intended": status["records"][0]["intendedOrdinal"] = True
                elif mutation == "record-order": status["records"].reverse()
                elif mutation == "controls": status["records"][0]["scatterEvidence"]["effectiveControls"]["ScatterValue"] = "0"
                elif mutation == "delivery": status["records"][0]["deliveryReport"]["events"][1]["pressure"] = .5
                elif mutation == "hash": status["records"][0]["scatterEvidence"]["sha256Pixels"] = "f" * 64
                elif mutation == "pixels": status["records"][0]["scatterEvidence"]["data"] = base64.b64encode(zlib.compress(b"bad")).decode()
                else:
                    status["records"][-1]["repeatSha256Pixels"] = ["b" * 64]
                    status["records"][-1]["repeatSha256CanonicalPixels"] = ["d" * 64]
                    status["records"][-1]["repeatImages"] = []
                    status["records"][-1]["repeatDifferences"] = []
                (self.output / "oracle-status.json").write_bytes(cache.canonical(status))
                with self.assertRaises((ValueError, KeyError, TypeError)):
                    cache.acquisition(self.output, fixtures)

    def test_scatter_flag_cannot_reduce_unrelated_baseline_repeats(self):
        self.case["scatterProbe"] = {"enabled": True}
        self.row["scatterProbe"] = {"enabled": True}
        self.row["repeatSha256Pixels"] = ["b" * 64]
        self.row["repeatSha256CanonicalPixels"] = ["d" * 64]
        self.row["repeatDifferences"] = []
        self.row["repeatImages"] = []
        (self.output / "oracle-status.json").write_bytes(cache.canonical(self.status))
        with self.assertRaisesRegex(ValueError, "repeat gate"):
            cache.acquisition(self.output, self.fixtures)

    def test_kernel_convergence_cache_retention_and_precision_linkage(self):
        self.fixtures["group"] = "wetkernelcount"
        self.expected["group"] = "wetkernelcount"
        precision = cache.canonical({"status": "complete", "records": []})
        convergence = cache.canonical({"status": "complete", "kernelPrecisionSha256": cache.digest(precision)})
        (self.output / "kernel-precision.json").write_bytes(precision)
        (self.output / "kernel-convergence.json").write_bytes(convergence)
        (self.output / "managed-lab.json").write_bytes(cache.canonical({"status": "complete"}))
        (self.output / "fixtures.json").write_bytes(cache.canonical(self.fixtures))
        # Individual sidecar suites check their full original schemas. These
        # mocks isolate cache retention, mandatory validation and hash linkage.
        validators = {}
        def modules(path):
            name = Path(path).name
            validators.setdefault(name, mock.Mock())
            return {"validate": validators[name]}
        with mock.patch("runpy.run_path", side_effect=modules):
            files = cache.acquisition(self.output, self.fixtures)
            self.assertEqual(files["kernel-convergence.json"], convergence)
            self.assertEqual(set(validators), {"kernel_precision_probe.py", "kernel_convergence_probe.py", "managed_lab_probe.py"})
            for validator in validators.values(): self.assertEqual(validator.call_count, 1)
            validators["kernel_convergence_probe.py"].assert_called_once_with(
                cache.parse(convergence), self.fixtures)
            (self.output / "kernel-convergence.json").unlink()
            with self.assertRaises(OSError): cache.acquisition(self.output, self.fixtures)
            (self.output / "kernel-convergence.json").write_bytes(cache.canonical({"status": "complete", "kernelPrecisionSha256": "f" * 64}))
            with self.assertRaisesRegex(ValueError, "linkage"):
                cache.acquisition(self.output, self.fixtures)
            (self.output / "kernel-convergence.json").write_bytes(convergence)
            cache.seal(self.root, self.destination, self.expected)
            for name in files: (self.output / name).unlink()
            cache.restore(self.root, self.destination, self.expected)
            self.assertEqual((self.output / "kernel-convergence.json").read_bytes(), convergence)

    def test_repeat_canonical_mask_matches_pixelwise_definition(self):
        import hashlib
        import importlib.util
        import random
        spec = importlib.util.spec_from_file_location("repeat_pixels", Path(__file__).parent / "gzbrush_reference/repeat_pixels.py")
        pixels = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(pixels)
        random_source = random.Random(0x52474241)
        for count in (0, 1, 7, 256, 4097):
            raw = bytearray(random_source.randrange(256) for _ in range(count * 4))
            for index in range(count):
                raw[4 * index + 3] = (0, 1, 127, 255)[index % 4]
            expected = bytearray(raw)
            for offset in range(0, len(expected), 4):
                if expected[offset + 3] == 0:
                    expected[offset:offset + 3] = bytes(3)
            self.assertEqual(pixels.canonical_hash(bytes(raw)), hashlib.sha256(expected).hexdigest())
            self.assertEqual(pixels.differences(bytes(raw), bytes(raw)),
                             {"hiddenRgbOnlyPixels": 0, "visibleOrAlphaChangedPixels": 0})
        with self.assertRaises(ValueError):
            pixels.canonical_hash(b"bad")

    def test_repeat_differences_match_pixelwise_definition_across_chunks(self):
        import importlib.util
        import random
        spec = importlib.util.spec_from_file_location("repeat_pixels", Path(__file__).parent / "gzbrush_reference/repeat_pixels.py")
        pixels = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(pixels)
        source = random.Random(0x44494646)
        for count in (0, 1, 7, 256, 65537):
            first = bytearray(source.randrange(256) for _ in range(count * 4))
            second = bytearray(first)
            for index in range(count):
                offset = index * 4
                first[offset + 3] = second[offset + 3] = (0, 1, 127, 255)[index % 4]
                if index % 3:
                    second[offset + index % 4] ^= 255
            hidden = visible = 0
            for offset in range(0, len(first), 4):
                if first[offset:offset + 4] != second[offset:offset + 4]:
                    if first[offset + 3] == second[offset + 3] == 0:
                        hidden += 1
                    else:
                        visible += 1
            self.assertEqual(pixels.differences(bytes(first), bytes(second)),
                             {"hiddenRgbOnlyPixels": hidden, "visibleOrAlphaChangedPixels": visible})
            self.assertEqual(pixels.differences(bytes(second), bytes(first)),
                             {"hiddenRgbOnlyPixels": hidden, "visibleOrAlphaChangedPixels": visible})
        for first, second in ((b"bad", b"bad"), (bytes(4), bytes(8))):
            with self.assertRaises(ValueError):
                pixels.differences(first, second)

    def test_fixed_readback_checkpoint_never_selects_a_favorable_capture(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("repeat_pixels", Path(__file__).parent / "gzbrush_reference/repeat_pixels.py")
        pixels = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(pixels)
        clear, hidden, painted = bytes(4), bytes([7, 9, 11, 0]), bytes([1, 2, 3, 255])
        for first, second, reset, fails in (
                (painted, painted, None, False),
                (clear, hidden, None, False),
                (painted, clear, None, True),
                (clear, painted, None, True),
                (clear, hidden, clear, True),
                (clear, clear, clear, False)):
            with self.subTest(first=first, second=second, reset=reset):
                observations = iter((first, second))
                read = mock.Mock(side_effect=lambda: next(observations))
                wait = mock.Mock()
                capture = pixels.capture_checkpoint(read, wait, 4, "diagnostic", reset)
                self.assertEqual(next(capture), 20)
                if fails:
                    with self.assertRaisesRegex(RuntimeError, "fixed Qt checkpoint"):
                        next(capture)
                else:
                    with self.assertRaises(StopIteration) as result:
                        next(capture)
                    self.assertEqual(result.exception.value[0], first)
                self.assertEqual(read.call_count, 2)
                self.assertEqual(wait.call_count, 2)
        for initial, reset, message in ((b"bad", None, "Incomplete"),
                                         (painted, clear, "reset readback differs")):
            read = mock.Mock(return_value=initial)
            capture = pixels.capture_checkpoint(read, mock.Mock(), 4, "diagnostic", reset)
            with self.assertRaisesRegex(RuntimeError, message):
                next(capture)
            self.assertEqual(read.call_count, 1)

    def test_explicit_source_pressure_mode_is_retained(self):
        self.case["sourcePressureFlag"] = True
        self.row["sourcePressureFlag"] = True
        self.row["effectiveView"]["disablePressure"] = True
        (self.output / "fixtures.json").write_bytes(cache.canonical(self.fixtures))
        (self.output / "oracle-status.json").write_bytes(cache.canonical(self.status))
        self.sealed()
        cache.restore(self.root, self.destination, self.expected)
        restored = cache.parse((self.output / "oracle-status.json").read_bytes())
        self.assertIs(restored["records"][0]["effectiveView"]["disablePressure"], True)

    def test_pressure_mode_mismatch_and_nonbool_are_rejected(self):
        for requested, observed in ((True, False), (False, True),
                                    (True, 1), (False, 0), ("true", True), (1, True)):
            self.case["sourcePressureFlag"] = requested
            self.row["sourcePressureFlag"] = requested
            self.row["effectiveView"]["disablePressure"] = observed
            (self.output / "fixtures.json").write_bytes(cache.canonical(self.fixtures))
            (self.output / "oracle-status.json").write_bytes(cache.canonical(self.status))
            with self.assertRaises(ValueError):
                cache.seal(self.root, self.destination, self.expected)
            self.assertFalse(self.destination.exists())

    def test_rehashed_partial_status_still_rejected(self):
        self.sealed()
        path = self.destination / "oracle-status.json"
        status = cache.parse(path.read_bytes())
        status["records"] = []
        data = cache.canonical(status)
        path.write_bytes(data)
        manifest_path = self.destination / "manifest.json"
        manifest = cache.parse(manifest_path.read_bytes())
        manifest["files"][path.name] = [len(data), cache.digest(data)]
        manifest_path.write_bytes(cache.canonical(manifest))
        self.rejected()

    def test_duplicate_json_and_oversized_manifest(self):
        self.sealed()
        path = self.destination / "manifest.json"
        path.write_bytes(b'{"schema":1,"schema":1}')
        self.rejected()
        path.write_bytes(b" " * (16 * 1024 * 1024 + 1))
        self.rejected()

    def test_fresh_fixture_change_invalidates_capture(self):
        self.sealed()
        self.fixtures["cases"][0]["diameter"] = 2
        (self.output / "fixtures.json").write_bytes(cache.canonical(self.fixtures))
        self.rejected()

    def test_original_package_timestamps_are_deterministic(self):
        packages = self.output / "resource-packages"
        packages.mkdir()
        path = packages / "original.bundle"
        snapshots = []
        for year in (2020, 2025):
            with zipfile.ZipFile(path, "w") as archive:
                entry = zipfile.ZipInfo("brushes/original.gbr", (year, 1, 1, 0, 0, 0))
                entry.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(entry, b"original fixture only")
            cache.normalize_generated_packages(self.root)
            snapshots.append(path.read_bytes())
        self.assertEqual(*snapshots)

    def test_rotation_roundtrip_guard_is_resource_only(self):
        self.row["effectiveView"]["rotation"] = .0002104591497
        (self.output / "oracle-status.json").write_bytes(cache.canonical(self.status))
        with self.assertRaises(ValueError):
            cache.acquisition(self.output, self.fixtures)
        self.case["resourceProbe"] = {"kind": "original"}
        self.row["resourceProbe"] = self.case["resourceProbe"]
        (self.output / "oracle-status.json").write_bytes(cache.canonical(self.status))
        cache.acquisition(self.output, self.fixtures)
        self.row["effectiveView"]["rotation"] = .002
        (self.output / "oracle-status.json").write_bytes(cache.canonical(self.status))
        with self.assertRaises(ValueError):
            cache.acquisition(self.output, self.fixtures)

    def test_cache_inventory_bounds(self):
        self.sealed()
        path = self.destination / "manifest.json"
        manifest = cache.parse(path.read_bytes())
        manifest["files"]["Original_dry.png"][0] = cache.FILE_LIMIT + 1
        path.write_bytes(cache.canonical(manifest))
        self.rejected()


if __name__ == "__main__":
    unittest.main()
