"""CI-only source-envelope generation and native decoder negative controls.

The spacing-eight diagnostics remain original source acquisitions. Their only
admitted candidate outcome is explicit importer rejection, never pixel parity.
"""
import copy
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from xml.etree.ElementTree import fromstring
import zlib


ADMISSION = {
    "version": 1, "outcome": "expected-rejection",
    "reasonCode": "kpp-spacing-outside-rcp1", "field": "spacing",
    "authoredValue": 8, "minimum": .01, "maximum": 5,
}


def authored_spacing(path):
    """Read the original KPP XML to prove the out-of-envelope value survives."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise AssertionError("Generated source preset is not PNG")
    offset = 8
    while offset < len(data):
        size = struct.unpack(">I", data[offset:offset + 4])[0]
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + size]
        if kind == b"zTXt" and payload.startswith(b"preset\0\0"):
            fields = fromstring(zlib.decompress(payload[8:]))
            brush = fields.find("param[@name='brush_definition']")
            return float(fromstring(brush.text).attrib["spacing"])
        offset += 12 + size
    raise AssertionError("Generated source preset lost its authored brush XML")


def prepare(group, root):
    subprocess.run([sys.executable, str(Path(__file__).with_name("prepare.py"))],
                   env=dict(os.environ, BRUSH_REFERENCE_ROOT=str(root),
                            BRUSH_REFERENCE_GROUP=group),
                   check=True, timeout=30, capture_output=True)
    return json.loads((root / "output/fixtures.json").read_text(encoding="utf-8"))


def main():
    comparator = str(Path(sys.argv[1]).resolve())
    failures = []
    with tempfile.TemporaryDirectory(prefix="gz-source-admission-") as directory:
        root = Path(directory)
        original = prepare("spacingdynamics", root)
        assert len(original["cases"]) == 112
        outside = []
        for index, case in enumerate(original["cases"]):
            spacing = 1 if index < 16 else 4 if index < 64 else 8
            assert case["spacing"] == spacing
            preset = root / "data/krita/paintoppresets" / (case["preset"] + ".kpp")
            assert authored_spacing(preset) == spacing
            if spacing == 8:
                assert case["sourceAdmission"] == ADMISSION
                outside.append(index)
            else:
                assert "sourceAdmission" not in case
        assert outside == list(range(64, 112))
        manifest = root / "output/fixtures.json"

        def validate(document, label, accepted=False):
            manifest.write_text(json.dumps(document), encoding="utf-8")
            result = subprocess.run([comparator, "--validate-fixtures", str(root)],
                                    capture_output=True, text=True, timeout=10)
            valid = result.returncode == 0 if accepted else (
                result.returncode == 1 and "Brush comparison failed" in result.stderr)
            if not valid:
                failures.append(label + ": " + (result.stderr or result.stdout).strip())

        validate(original, "original admission matrix", accepted=True)
        result = subprocess.run([comparator, "--validate-source-admission", str(root)],
                                capture_output=True, text=True, timeout=10)
        if result.returncode != 0:
            failures.append("actual source spacing rejection: " +
                            (result.stderr or result.stdout).strip())
        else:
            rejected = json.loads(result.stdout)["records"]
            assert len(rejected) == 48
            null_fields = (
                "region", "regionPixels", "referenceBounds", "candidateBounds",
                "referenceAlphaBounds", "candidateAlphaBounds", "transparentBase",
                "alphaCoverageIoU", "referenceChangedPixels", "candidateChangedPixels",
                "changedPixelIoU", "visibleChangedPixelIoU", "premultipliedRGBMAE",
                "alphaMAE", "maxRGBError", "maxAlphaError", "alphaP99",
                "deltaE00BlackMean", "deltaE00WhiteMean", "deltaE00BlackP95",
                "deltaE00WhiteP95", "emptyCandidateFailure", "bothEmpty",
                "boundsError", "candidateProfile", "candidateSeed", "renderMilliseconds", "dabs")
            for index, outcome in enumerate(rejected, 64):
                assert outcome["preset"] == original["cases"][index]["preset"]
                assert outcome["comparisonStatus"] == "expected-rejection"
                assert outcome["sourceAdmission"] == ADMISSION
                assert outcome["admissionReason"] == (
                    "KPP spacing is outside the RCP1 supported range 0.01-5")
                assert outcome["candidateImportComplete"] is False
                assert outcome["candidateImportRecords"] == 0
                assert all(outcome[field] is None for field in null_fields)
                assert all(outcome[field] is False for field in (
                    "sourceSemanticallySupported", "sourceReferenceVerified",
                    "sourceParityQualified", "sensorSourceParityQualified",
                    "sensorAcquisitionInputsValidated"))
        # Metadata cannot convert a corrupt parser input, or a successfully
        # admitted lower-spacing preset, into an expected rejection outcome.
        outside_preset = root / "data/krita/paintoppresets" / (
            original["cases"][64]["preset"] + ".kpp")
        admitted_preset = root / "data/krita/paintoppresets" / (
            original["cases"][63]["preset"] + ".kpp")
        outside_bytes = outside_preset.read_bytes()
        try:
            for label, replacement in (
                    ("unrelated malformed preset", b"original corrupt PNG diagnostic"),
                    ("successfully admitted preset", admitted_preset.read_bytes())):
                outside_preset.write_bytes(replacement)
                result = subprocess.run([comparator, "--validate-source-admission", str(root)],
                                        capture_output=True, text=True, timeout=10)
                if (result.returncode != 1 or
                        "Expected exact source-envelope rejection" not in result.stderr):
                    failures.append(label + " was reported as expected rejection: " +
                                    (result.stderr or result.stdout).strip())
        finally:
            outside_preset.write_bytes(outside_bytes)
        mutations = [
            (("cases", 64, "sourceAdmission"), None),
            (("cases", 64, "sourceAdmission"), []),
            (("cases", 64, "sourceAdmission"), "expected-rejection"),
            (("cases", 64, "sourceAdmission", "version"), 2),
            (("cases", 64, "sourceAdmission", "version"), True),
            (("cases", 64, "sourceAdmission", "outcome"), "source-verified"),
            (("cases", 64, "sourceAdmission", "reasonCode"), "invalid-preset"),
            (("cases", 64, "sourceAdmission", "field"), "geometry"),
            (("cases", 64, "sourceAdmission", "authoredValue"), 4),
            (("cases", 64, "sourceAdmission", "authoredValue"), "8"),
            (("cases", 64, "sourceAdmission", "authoredValue"), True),
            (("cases", 64, "sourceAdmission", "minimum"), .001),
            (("cases", 64, "sourceAdmission", "maximum"), 8),
            (("cases", 64, "sourceAdmission", "sourceParityQualified"), True),
            (("cases", 0, "sourceAdmission"), ADMISSION),
            (("cases", 63, "sourceAdmission"), ADMISSION),
            (("cases", 0, "sourceAdmission"), None),
            (("cases", 64, "spacing"), 4),
            (("cases", 64, "sensorProbe", "target"), "Ratio"),
            (("cases", 64, "sensorProbe", "sensor"), "xtilt"),
            (("cases", 64, "sensorProbe", "label"), "sparse 4 east quarter"),
        ]
        for ordinal, (path, value) in enumerate(mutations):
            broken = copy.deepcopy(original)
            node = broken
            for key in path[:-1]:
                node = node[key]
            node[path[-1]] = copy.deepcopy(value)
            validate(broken, "forged source admission " + str(ordinal))
        for index in (64, 111):
            broken = copy.deepcopy(original)
            del broken["cases"][index]["sourceAdmission"]
            validate(broken, "missing outside-envelope admission " + str(index))
        broken = copy.deepcopy(original)
        broken["cases"][64]["preset"] = "forged source spacing diagnostic"
        broken["presets"][64] = broken["cases"][64]["preset"]
        validate(broken, "forged outside-envelope preset identity")

        # Tablet diagnostics share the generator, but all their authored
        # spacing values are admitted. None may inherit a rejection outcome.
        tablet_root = root / "tablet"
        tablet = prepare("tabletsensors", tablet_root)
        assert len(tablet["cases"]) == 188
        assert all(.01 <= case["spacing"] <= 5 and "sourceAdmission" not in case
                   for case in tablet["cases"])
    if failures:
        raise RuntimeError("Source admission decoder failures:\n" + "\n".join(failures))
    print("Source spacing admission metadata and native forgery guards passed")


if __name__ == "__main__":
    main()
