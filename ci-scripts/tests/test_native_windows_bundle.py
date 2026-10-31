"""Portable binary-retention contracts. These execute only in supported CI."""

import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import time
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import native_windows_bundle as bundle


GUID = bytes.fromhex("00112233445566778899aabbccddeeff")
SHA = "a" * 40


def image_bytes(name="OpenToonz.pdb", guid=GUID, age=7):
    # Minimal PE32+ with a section-mapped IMAGE_DEBUG_DIRECTORY and RSDS.
    image = bytearray(1024)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 60, 128)
    image[128:132] = b"PE\0\0"
    struct.pack_into("<HH", image, 132, 0x8664, 1)
    struct.pack_into("<H", image, 148, 240)
    struct.pack_into("<H", image, 152, 0x20B)
    struct.pack_into("<I", image, 152 + 108, 16)
    struct.pack_into("<II", image, 152 + 112 + 48, 0x1000, 28)
    image[392:400] = b".rdata\0\0"
    struct.pack_into("<IIII", image, 400, 512, 0x1000, 512, 512)
    record = b"RSDS" + guid + struct.pack("<I", age) + name.encode() + b"\0"
    struct.pack_into("<IIII", image, 512 + 12, 2, len(record), 0x1040, 576)
    image[576:576 + len(record)] = record
    return bytes(image)


def pdb_bytes(guid=GUID, age=7):
    # MSF blocks: superblock, both reserved free-block maps, directory
    # block-map, stream directory, and Info header. Stream0 is absent.
    block = 512
    data = bytearray(block * 6)
    data[:32] = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"
    struct.pack_into("<6I", data, 32, block, 1, 6, 16, 0, 3)
    for index in (1, 2):
        data[index * block:(index + 1) * block] = b"\xc0" + b"\xff" * (block - 1)
    struct.pack_into("<I", data, block * 3, 4)
    struct.pack_into("<4I", data, block * 4, 2, 0xFFFFFFFF, 28, 5)
    struct.pack_into("<III", data, block * 5, 20000404, 0, age)
    data[block * 5 + 12:block * 5 + 28] = guid
    return bytes(data)


class NativeWindowsBundle(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.package = self.root / "Opentoonz"
        self.build = self.root / "RelWithDebInfo"
        self.mesa = self.root / "mesa-runtime"
        for path in (self.package, self.build, self.mesa):
            path.mkdir()
        (self.package / "portablestuff").mkdir()
        (self.package / "portablestuff" / "default.ini").write_text("default=1\n")
        for name in ("OpenToonz.exe", "tnzcore.dll"):
            pdb = Path(name).with_suffix(".pdb").name
            data = image_bytes(pdb)
            (self.package / name).write_bytes(data)
            (self.build / name).write_bytes(data)
            (self.build / pdb).write_bytes(pdb_bytes())
        for name in bundle.MESA_FILES:
            (self.mesa / name).write_bytes(b"pinned-fixture-" + name.encode())
        now = time.time()
        self.step_start = now - 3
        self.release = self.root / "ui-integration-output-release.json"
        self.record = {"schema": 1, "source_sha": SHA, "run_id": "42", "run_attempt": "2",
                       "original_pid": 123, "timed_out": True, "original_status": None,
                       "safe_to_release": True, "started_at": now - 2, "finished_at": now - 1}
        self.write_release()
        self.output = self.root / "native-windows.zip"

    def write_release(self):
        self.release.write_text(json.dumps(self.record), encoding="utf-8")

    def prepare(self, **kwargs):
        options = dict(source_sha=SHA, run_id="42", run_attempt="2", native_step_started_at=self.step_start,
                       flags=("--require-vector-workflows", "--require-combined-workflows"),
                       recorder_build_id=SHA + "-42-2")
        options.update(kwargs)
        return bundle.prepare_bundle(self.package, self.build, self.mesa, self.release, self.output, **options)

    def assert_no_partial(self):
        self.assertFalse(self.output.exists())
        self.assertEqual([], list(self.root.glob("native-bundle-*.zip")))

    def test_exact_files_symbols_hashes_and_flags_with_separate_mesa(self):
        (self.package / "OpenToonz.pdb").write_bytes(b"not the build symbols")
        (self.package / "tnzcore.lib").write_bytes(b"not needed at runtime")
        manifest = self.prepare()
        with zipfile.ZipFile(self.output) as archive:
            actual = json.loads(archive.read("manifest.json"))
            self.assertEqual(manifest, actual)
            for entry in actual["files"]:
                content = archive.read(entry["path"])
                self.assertEqual(len(content), entry["bytes"])
                self.assertEqual(hashlib.sha256(content).hexdigest(), entry["sha256"])
            self.assertEqual(pdb_bytes(), archive.read("symbols/OpenToonz.pdb"))
            self.assertEqual(b"default=1\n", archive.read("package/portablestuff/default.ini"))
            self.assertNotIn("package/OpenToonz.pdb", archive.namelist())
            self.assertNotIn("package/tnzcore.lib", archive.namelist())
            self.assertNotIn("package/opengl32.dll", archive.namelist())
            self.assertIn("mesa/opengl32.dll", archive.namelist())
        self.assertEqual(SHA, manifest["source_sha"])
        self.assertEqual(["--require-vector-workflows", "--require-combined-workflows"], manifest["native_flags"])
        self.assertEqual(2, len(manifest["binaries"]))
        self.assertTrue(all(entry["symbols"] for entry in manifest["binaries"]))
        self.assertNotIn(str(self.root), json.dumps(manifest))

    def test_false_missing_stale_or_wrong_run_report_refuses_publication(self):
        cases = ({"safe_to_release": False}, {"safe_to_release": "true"},
                 {"source_sha": "b" * 40}, {"run_id": "41"}, {"run_attempt": "1"},
                 {"original_pid": None}, {"original_pid": True}, {"started_at": self.step_start - 1},
                 {"finished_at": float("nan")}, {"timed_out": True, "original_status": 0})
        original = dict(self.record)
        for values in cases:
            with self.subTest(values=values):
                self.record = {**original, **values}
                self.write_release()
                with self.assertRaises(bundle.BundleError):
                    self.prepare()
                self.assert_no_partial()
        self.release.unlink()
        with self.assertRaises(OSError):
            self.prepare()
        self.assert_no_partial()

    def test_invalid_json_and_success_outcome_are_not_retained(self):
        self.release.write_bytes(b"{truncated")
        with self.assertRaises(json.JSONDecodeError):
            self.prepare()
        self.assert_no_partial()
        self.write_release()
        with self.assertRaises(bundle.BundleError):
            self.prepare(native_outcome="success")
        self.assert_no_partial()

    def test_zero_original_status_can_preserve_later_required_case_failure(self):
        self.record.update(timed_out=False, original_status=0)
        self.write_release()
        self.assertEqual(0, self.prepare()["release"]["original_status"])

    def test_binary_mismatch_is_not_paired_with_symbols(self):
        path = self.package / "tnzcore.dll"
        path.write_bytes(path.read_bytes() + b"changed")
        with self.assertRaisesRegex(bundle.BundleError, "build counterpart"):
            self.prepare()
        self.assert_no_partial()

    def test_real_pe_pdb_guid_and_age_must_match(self):
        path = self.build / "OpenToonz.pdb"
        for content in (pdb_bytes(guid=b"x" * 16), pdb_bytes(age=8), pdb_bytes()[:70]):
            with self.subTest(content_hash=hashlib.sha256(content).hexdigest()):
                path.write_bytes(content)
                with self.assertRaises(bundle.BundleError):
                    self.prepare()
                self.assert_no_partial()

    def test_missing_or_malformed_primary_identity_refuses_bundle(self):
        (self.build / "OpenToonz.pdb").unlink()
        with self.assertRaisesRegex(bundle.BundleError, "matching PDB"):
            self.prepare()
        self.assert_no_partial()
        (self.build / "OpenToonz.pdb").write_bytes(pdb_bytes())
        for root in (self.build, self.package):
            (root / "OpenToonz.exe").write_bytes(b"not a PE")
        with self.assertRaises(bundle.BundleError):
            self.prepare()
        self.assert_no_partial()

    def test_dumps_credentials_fixtures_and_renderer_overlays_never_enter_archive(self):
        for name in ("original.dmp", ".env", "credentials.json", "private.pem", "OpenToonz.exe.local",
                     "opengl32.dll", "SeExprSdkFixture.dll", "groundzero-native-ui-disposable"):
            with self.subTest(name=name):
                path = self.package / name
                path.write_bytes(b"must not be published")
                with self.assertRaises(bundle.BundleError):
                    self.prepare()
                self.assert_no_partial()
                path.unlink()

    def test_reparse_input_is_rejected_without_reading_its_contents(self):
        original = Path.lstat
        target = self.package / "OpenToonz.exe"
        def reparse(path):
            result = original(path)
            if path == target:
                return type("ReparseStat", (), {"st_file_attributes": 0x400, "st_mode": result.st_mode})()
            return result
        with mock.patch.object(Path, "lstat", reparse):
            with self.assertRaisesRegex(bundle.BundleError, "reparse"):
                self.prepare()
        self.assert_no_partial()

    def test_traversal_failure_does_not_silently_omit_runtime_directory(self):
        original = bundle.os.scandir
        def unreadable(path):
            if Path(path) == self.package / "portablestuff":
                raise PermissionError("fixture directory became unreadable")
            return original(path)
        with mock.patch.object(bundle.os, "scandir", side_effect=unreadable):
            with self.assertRaises(PermissionError):
                self.prepare()
        self.assert_no_partial()

    def test_budget_deadline_and_copy_failure_do_not_publish_partial_zip(self):
        for options in ({"max_bytes": 1}, {"max_files": 1}, {"timeout": -1}):
            with self.subTest(options=options):
                with self.assertRaises(bundle.BundleError):
                    self.prepare(**options)
                self.assert_no_partial()
        with mock.patch.object(zipfile.ZipFile, "writestr", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                self.prepare()
        self.assert_no_partial()

    def test_input_mutation_after_hashing_and_publication_error_leave_no_bundle(self):
        original_zip = zipfile.ZipFile
        path = self.package / "OpenToonz.exe"
        original_image = path.read_bytes()
        def changing_zip(*args, **kwargs):
            path.write_bytes(original_image + b"changed after validation")
            return original_zip(*args, **kwargs)
        with mock.patch.object(zipfile, "ZipFile", side_effect=changing_zip):
            with self.assertRaisesRegex(bundle.BundleError, "changed during retention"):
                self.prepare()
        self.assert_no_partial()
        path.write_bytes(original_image)
        with mock.patch.object(bundle.os, "link", side_effect=OSError("publication refused")):
            with self.assertRaises(OSError):
                self.prepare()
        self.assert_no_partial()

    def test_existing_output_is_preserved_and_nested_output_refused(self):
        self.output.write_bytes(b"earlier evidence")
        with self.assertRaises(bundle.BundleError):
            self.prepare()
        self.assertEqual(b"earlier evidence", self.output.read_bytes())
        self.output = self.package / "portablestuff" / "nested.zip"
        with self.assertRaises(bundle.BundleError):
            self.prepare()
        self.assert_no_partial()


if __name__ == "__main__":
    unittest.main()
