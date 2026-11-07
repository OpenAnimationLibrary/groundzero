"""Unexecuted protocol regressions; synthetic bytes are NOT Photoshop evidence."""
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

import photoshop_export_contract as contract


def png(width=2, height=1, color=0):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    channels = {0: 1, 6: 4}[color]
    rows = (bytes([0]) + bytes(width * channels)) * height
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def unresolved(kind):
    return {"status": "unresolved", "kind": kind, "opaqueHex": None,
            "locator": "Not structurally observed in this protocol fixture", "evidence": None}


def state():
    controls = {name: {"available": False, "enabled": False, "value": None,
        "unit": unit, "label": "Unobserved fixture UI", "choices": [], "evidence": ["ui"]}
        for name, (_, unit) in contract.CONTROL_SPECS.items()}
    controls["tip.resource"].update(available=True, enabled=True, value="mask")
    controls["options.opacity"].update(available=True, enabled=True, value=100)
    return {"controls": controls, "unrepresented": {}, "inventoryComplete": True}


class PhotoshopInputContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gz-photoshop-input-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.data = {
            "schema": contract.SCHEMA, "profile": "P2711", "collection": "original-contract-fixture",
            "producer": {"product": "Adobe Photoshop", "version": "27.11", "reportedVersion": "27.11.0",
                "build": "Synthetic protocol fixture - no application executed",
                "platform": "windows", "architecture": "x86_64", "uiLanguage": "en-US",
                "executableSha256": "1" * 64, "executableBytes": 1234,
                "executableMatchedRunningApp": True, "identityEvidenceSha256": "5" * 64,
                "presetSaveOptions": {"includeSize": True, "includeColor": True, "includeToolSettings": True}},
            "attestation": {"authorizedExecution": True, "originalAssets": True, "privacyReviewed": True},
            "files": [], "assets": [{"id": "mask", "kind": "sampled-mask",
                "creation": "Original synthetic protocol mask", "rights": {"operatorOriginal": True,
                    "license": "CC0-1.0", "redistribution": "permitted"}, "file": "mask", "definition": None}],
            "snapshots": {"full": state(), "half": state()}, "cases": []}
        self.add_file("ui", "ui.png", "ui-png", png(color=6))
        self.add_file("mask", "mask.png", "asset-png", png())
        # Header ingestion deliberately does not pretend to decode this body.
        self.add_file("base", "base.abr", "abr-export", struct.pack(">HH", 6, 2) + b"original protocol body base")
        self.add_file("half", "half.abr", "abr-export", struct.pack(">HH", 6, 2) + b"original protocol body half")
        baseline = self.case("baseline", "base", "full")
        changed = self.case("opacity-half", "half", "half")
        changed["parent"] = "baseline"
        changed["change"] = {"preset": "preset", "control": "options.opacity", "before": 100,
            "after": 50, "derivedStateChanges": []}
        self.data["snapshots"]["half"]["controls"]["options.opacity"]["value"] = 50
        self.data["cases"] = [baseline, changed]

    def add_file(self, file_id, path, kind, payload):
        (self.root / path).write_bytes(payload)
        row = {"id": file_id, "path": path, "kind": kind, "bytes": len(payload),
               "sha256": hashlib.sha256(payload).hexdigest()}
        self.data["files"].append(row)
        return row

    def case(self, name, export, snapshot):
        return {"id": name, "parent": None, "purpose": "d-control-mapping", "inputAbr": None,
            "inputProvenance": None, "abr": export, "expectedInputHeader": None,
            "expectedExportHeader": {"major": 6, "subversion": 2, "legacyRecordCount": None},
            "interpretation": {"status": "loaded", "uiEvidence": ["ui"], "renderAssociation": None, "rejectionReason": None},
            "exportOrder": ["preset"], "resources": [{"id": "primary", "asset": "mask",
                "ordinals": {"input": None, "export": None},
                "identities": {"input": None, "export": unresolved("sample")}}],
            "presets": [{"id": "preset", "name": "GZ Original protocol preset",
                "ordinals": {"input": None, "export": None}, "primary": "primary", "texture": None,
                "secondary": None, "uiSnapshot": snapshot,
                "identities": {"input": None, "export": unresolved("preset")}}], "change": None}

    def write_manifest(self, data=None):
        (self.root / "manifest.json").write_bytes(contract.canonical(self.data if data is None else data))

    def ingest(self, data=None):
        self.write_manifest(data)
        return contract.validate(self.root)

    def reject(self, mutate):
        data = copy.deepcopy(self.data)
        mutate(data)
        with self.assertRaises(ValueError): self.ingest(data)

    def test_file_header_and_single_setting_receipt_never_qualify(self):
        report = self.ingest()
        self.assertEqual(report["caseCount"], 2)
        self.assertEqual(report["singleSettingPairs"][0]["control"], "options.opacity")
        self.assertTrue(report["singleSettingPairs"][0]["serializedBytesChanged"])
        self.assertEqual(report["containerEvidence"]["6/2"]["exportFiles"], ["base", "half"])
        self.assertEqual(report["containerEvidence"]["1"]["p2711Interpretations"], [])
        self.assertEqual(report["unresolvedSourceIdentities"], 4)
        for field in ("abrBodyDecoded", "abrDescriptorEnumsInterpreted", "pngPixelsDecoded",
                      "sourceSemanticallySupported", "sourceReferenceVerified", "physicalTabletValidated"):
            self.assertIs(report[field], False)

    def test_legacy_interpreted_input_is_distinct_from_modern_export(self):
        original = self.add_file("legacy", "legacy.abr", "abr-input", struct.pack(">HH", 1, 1) + b"original structural input")
        historical = self.case("legacy-input", "base", "full")
        historical.update(inputAbr="legacy", inputProvenance={"kind": "independently-authored-structural",
            "generatorRevision": "2" * 40, "producerLabel": None},
            expectedInputHeader={"major": 1, "subversion": None, "legacyRecordCount": 1})
        for node in historical["resources"] + historical["presets"]:
            kind = "sample" if "asset" in node else "preset"
            node["identities"]["input"] = unresolved(kind)
            node["ordinals"]["input"] = 0
        historical["interpretation"]["renderAssociation"] = {"referenceSet": "original-reference",
            "manifestSha256": "3" * 64, "inputAbrSha256": original["sha256"],
            "producerExecutableSha256": "1" * 64}
        self.data["cases"].append(historical)
        report = self.ingest()
        self.assertEqual(report["containerEvidence"]["1"]["inputFiles"], ["legacy"])
        self.assertEqual(report["containerEvidence"]["1"]["exportFiles"], [])
        self.assertEqual(report["containerEvidence"]["1"]["p2711Interpretations"], ["legacy-input"])
        self.assertEqual(len(report["containerEvidence"]["1"]["renderAssociations"]), 1)
        self.assertIs(report["renderAssociationsAreExternalAndUnvalidated"], True)
        self.reject(lambda d: d["cases"][-1]["resources"][0]["ordinals"].update(input=1))
        self.reject(lambda d: d["cases"][-1]["presets"][0]["ordinals"].update(input=255))

    def test_legacy_export_ordinal_stays_inside_header_count(self):
        self.add_file("legacy-export", "legacy-export.abr", "abr-export", struct.pack(">HH", 2, 1) + b"original protocol fixture")
        historical = self.case("legacy-export", "legacy-export", "full")
        historical["expectedExportHeader"] = {"major": 2, "subversion": None, "legacyRecordCount": 1}
        for node in historical["resources"] + historical["presets"]:
            node["ordinals"]["export"] = 0
        self.data["cases"].append(historical)
        self.assertEqual(self.ingest()["containerEvidence"]["2"]["exportFiles"], ["legacy-export"])
        self.reject(lambda d: d["cases"][-1]["resources"][0]["ordinals"].update(export=1))
        self.reject(lambda d: d["cases"][-1]["presets"][0]["ordinals"].update(export=255))

    def test_pending_input_cannot_claim_ui_or_execution(self):
        self.add_file("legacy", "legacy.abr", "abr-input", struct.pack(">HH", 2, 1) + b"original structural input")
        pending = self.case("pending-original", None, None)
        pending.update(inputAbr="legacy", inputProvenance={"kind": "independently-authored-structural",
            "generatorRevision": "2" * 40, "producerLabel": None},
            expectedInputHeader={"major": 2, "subversion": None, "legacyRecordCount": 1},
            expectedExportHeader=None, interpretation={"status": "pending", "uiEvidence": [], "renderAssociation": None, "rejectionReason": None})
        for node in pending["resources"] + pending["presets"]:
            kind = "sample" if "asset" in node else "preset"
            node["identities"] = {"input": unresolved(kind), "export": None}
        self.data["cases"].append(pending)
        report = self.ingest()
        self.assertEqual(report["containerEvidence"]["2"]["p2711Interpretations"], [])
        self.reject(lambda d: d["cases"][-1]["presets"][0].update(uiSnapshot="full"))

    def test_opaque_identity_reports_bind_exact_case_and_role(self):
        baseline = self.data["cases"][0]
        observations = []
        for node, kind, payload in ((baseline["resources"][0], "sample", "010203"),
                                    (baseline["presets"][0], "preset", None)):
            node["identities"]["export"] = {"status": "observed", "kind": kind,
                "opaqueHex": payload, "locator": "Original structural locator", "evidence": "observed"}
            observations.append({"localId": node["id"], "kind": kind,
                                 "opaqueHex": payload, "locator": "Original structural locator"})
        report = {"schema": contract.OBSERVATIONS, "abrSha256": self.data["files"][2]["sha256"],
            "observer": {"kind": "bounded-structural-reader", "revision": "4" * 40}, "observations": observations}
        self.add_file("observed", "observed.json", "observation-json", contract.canonical(report))
        self.assertEqual(self.ingest()["unresolvedSourceIdentities"], 2)
        self.reject(lambda d: d["cases"][0]["resources"][0]["identities"]["export"].update(opaqueHex="ffff"))
        self.reject(lambda d: d["cases"][0]["resources"][0]["identities"]["export"].update(opaqueHex=None))

    def test_one_authored_choice_retains_newly_visible_default(self):
        before = self.data["snapshots"]["full"]["controls"]
        after = self.data["snapshots"]["half"]["controls"]
        after["options.opacity"]["value"] = 100
        for controls, choice in ((before, "Off"), (after, "Fade")):
            controls["shape.sizeControl"].update(available=True, enabled=True,
                value=choice, choices=["Off", "Fade"])
        after["shape.sizeFadeSteps"].update(available=True, enabled=True, value=17)
        self.data["cases"][1]["change"] = {"preset": "preset", "control": "shape.sizeControl",
            "before": "Off", "after": "Fade", "derivedStateChanges": [
                {"preset": "preset", "control": "shape.sizeFadeSteps", "before": None,
                 "after": 17, "field": "value", "cause": "availability-change"},
                {"preset": "preset", "control": "shape.sizeFadeSteps", "field": "available", "before": False, "after": True},
                {"preset": "preset", "control": "shape.sizeFadeSteps", "field": "enabled", "before": False, "after": True}]}
        report = self.ingest()
        self.assertEqual(report["singleSettingPairs"][0]["control"], "shape.sizeControl")
        self.reject(lambda d: d["cases"][1]["change"].update(derivedStateChanges=[]))

    def test_unknown_missing_forged_and_multiple_setting_changes_reject(self):
        self.reject(lambda d: d.update(sourceReferenceVerified=True))
        self.reject(lambda d: d["producer"].update(version="27.12"))
        self.reject(lambda d: d["producer"].update(reportedVersion="27.110"))
        self.reject(lambda d: d["producer"].update(reportedVersion="27.11.1"))
        self.reject(lambda d: d["producer"].update(executableSha256="0" * 64))
        self.reject(lambda d: d["producer"].update(build="/private/source-path"))
        self.reject(lambda d: d["assets"][0]["rights"].update(operatorOriginal=False))
        self.reject(lambda d: d["assets"][0]["rights"].update(license="Unspecified"))
        self.reject(lambda d: d["snapshots"]["full"]["controls"].pop("tip.angle"))
        self.reject(lambda d: d["snapshots"]["half"]["controls"]["options.opacity"].update(value=True))
        self.reject(lambda d: d["cases"][1].update(parent="future-parent"))
        self.reject(lambda d: d["cases"][0].update(expectedExportHeader={"major": 7, "subversion": 2, "legacyRecordCount": None}))
        def two_changes(d):
            for name in ("full", "half"):
                d["snapshots"][name]["controls"]["shape.sizeJitter"].update(available=True, enabled=True, value=0)
            d["snapshots"]["half"]["controls"]["shape.sizeJitter"]["value"] = 1
        self.reject(two_changes)
        self.reject(lambda d: d["files"].extend(copy.deepcopy(d["files"]) * contract.MAX_FILES))

    def test_paths_hashes_links_and_extra_files_reject(self):
        self.reject(lambda d: d["files"][0].update(path="../ui.png"))
        self.reject(lambda d: d["files"][0].update(sha256="f" * 64))
        self.add_file("unused", "unused.abr", "abr-input", struct.pack(">HH", 2, 1))
        with self.assertRaises(ValueError): self.ingest()
        self.data["files"].pop()
        path = self.root / "mask.png"
        original = path.read_bytes()
        outside = self.root / "unlisted-original.png"
        outside.write_bytes(original)
        path.unlink()
        try:
            os.link(outside, path)
        except OSError:
            path.write_bytes(original)
            self.skipTest("hard-link creation unavailable")
        with self.assertRaises(ValueError): self.ingest()
        path.unlink()
        try:
            path.symlink_to(outside)
        except OSError:
            path.write_bytes(original)
            self.skipTest("symlink creation unavailable")
        with self.assertRaises((ValueError, OSError)): self.ingest()

    def test_png_structure_rejects_crc_private_text_and_truncation(self):
        image = png()
        self.assertEqual(contract.png_structure(image)["width"], 2)
        bad = bytearray(image)
        bad[-1] ^= 1
        with self.assertRaises(ValueError): contract.png_structure(bytes(bad))
        with self.assertRaises(ValueError): contract.png_structure(image[:-1])
        for kind in (b"tEXt", b"vpAg", b"ABCD", b"iCCP", b"eXIf", b"acTL"):
            content = b"private original metadata"
            chunk = struct.pack(">I", len(content)) + kind + content + struct.pack(">I", zlib.crc32(kind + content) & 0xffffffff)
            with self.subTest(kind=kind):
                with self.assertRaises(ValueError): contract.png_structure(image[:-12] + chunk + image[-12:])

    def test_json_header_and_fingerprint_boundaries(self):
        for data in (b'{"schema":1,"schema":2}', b'{"value":NaN}', b'[' * 40 + b'0' + b']' * 40):
            with self.assertRaises(ValueError): contract.parse(data)
        self.assertEqual(contract.abr_header(struct.pack(">HH", 1, 2))["legacyRecordCount"], 2)
        for major in range(6, 11):
            for subversion in (1, 2):
                self.assertEqual(contract.abr_header(struct.pack(">HH", major, subversion))["subversion"], subversion)
        for data in (b'abc', struct.pack(">HH", 6, 0), struct.pack(">HH", 11, 1)):
            with self.assertRaises(ValueError): contract.abr_header(data)
        image = self.root / "Original image with spaces.bin"
        image.write_bytes(b"Original fingerprint fixture, never executed")
        result = contract.fingerprint_executable(image)
        self.assertEqual(result["sha256"], hashlib.sha256(image.read_bytes()).hexdigest())
        self.assertIs(result["executed"], False)
        self.assertIs(result["pathRetained"], False)


if __name__ == "__main__":
    unittest.main()
