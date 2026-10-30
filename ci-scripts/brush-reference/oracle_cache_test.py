"""CI-only regression for source-oracle integrity; no reference app required."""
import os
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
        self.expected = {"schema": 1, "applicationSha256": "a" * 64, "group": "test"}
        self.xml = b'<Preset paintopid="paintbrush"/>'
        self.row = {**self.case, "image": "Original_dry.png", "effectivePresetXml": "Original_dry.effective.xml",
                    "effectivePresetSha256": cache.digest(self.xml), "sha256Pixels": "b" * 64,
                    "repeatSha256Pixels": ["b" * 64] * 2, "profile": "sRGB-elle-V2-srgbtrc.icc",
                    "changed": True, "effectiveView": {"diameter": 1, "opacity": 1, "flow": 1,
                    "rotation": 0, "blend": "normal", "eraser": False, "alphaLock": False,
                    "disablePressure": False}}
        self.status = {"status": "complete", "version": "5.3.4.1", "records": [self.row]}
        (self.output / "fixtures.json").write_bytes(cache.canonical(self.fixtures))
        (self.output / "oracle-status.json").write_bytes(cache.canonical(self.status))
        (self.output / "Original_dry.effective.xml").write_bytes(self.xml)
        (self.output / "Original_dry.png").write_bytes(png())

    def sealed(self):
        cache.seal(self.root, self.destination, self.expected)
        for name in ("oracle-status.json", "Original_dry.effective.xml", "Original_dry.png"):
            (self.output / name).unlink()

    def rejected(self, expected=None):
        before = cache.inventory(self.output)
        with self.assertRaises((ValueError, OSError, KeyError, TypeError)):
            cache.restore(self.root, self.destination, expected or self.expected)
        self.assertEqual(cache.inventory(self.output), before)

    def test_complete_restore_excludes_candidate_and_bundled_assets(self):
        (self.output / "comparison.json").write_text("candidate failure")
        (self.output / "bundled.kpp").write_text("must not persist")
        self.sealed()
        self.assertEqual({p.name for p in self.destination.iterdir()},
                         {"manifest.json", "oracle-status.json", "Original_dry.effective.xml", "Original_dry.png"})
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
                       {"repeatSha256Pixels": ["b" * 64, "c" * 64]},
                       {"effectiveView": {**self.row["effectiveView"], "eraser": True}},
                       {"effectiveView": {**self.row["effectiveView"], "opacity": .5}},
                       {"changed": False}):
            status = {**self.status, "records": [{**self.row, **change}]}
            (self.output / "oracle-status.json").write_bytes(cache.canonical(status))
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

    def test_cache_inventory_bounds(self):
        self.sealed()
        path = self.destination / "manifest.json"
        manifest = cache.parse(path.read_bytes())
        manifest["files"]["Original_dry.png"][0] = cache.FILE_LIMIT + 1
        path.write_bytes(cache.canonical(manifest))
        self.rejected()


if __name__ == "__main__":
    unittest.main()
