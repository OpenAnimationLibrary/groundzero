"""Content-addressed original source captures, never candidate qualification.

Only repository-generated CC0 cases and their source render/effective settings
are cached. No downloaded application, bundled brushes, logs, local paths or
credentials are persisted. Cache integrity assumes GitHub's repository/branch
cache access boundary; hashes detect damage, not a malicious authorized writer.
"""
import argparse
import base64
import hashlib
import io
import json
import math
import os
from pathlib import Path
from runpy import run_path
import platform
import re
import shutil
import stat
import struct
import subprocess
import zipfile
import zlib
from mask_repeat_contract import contract as mask_repeat_contract
import mask_repeat_lifecycle_contract

# Load only the pure helper file. Importing the package initializes Krita.
rotation_matches = run_path(str(Path(__file__).parent /
    "gzbrush_reference/view_control_contract.py"))["rotation_matches"]

VERSION = 2
FILE_LIMIT = 64 * 1024 * 1024
TOTAL_LIMIT = 512 * 1024 * 1024
COUNT_LIMIT = 4096
HEX = re.compile(r"[0-9a-f]{64}\Z")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read(path, limit=FILE_LIMIT):
    # All cache payloads are flat, with validated nonsymlink parent directories.
    require(not path.parent.is_symlink(), "symlink parent")
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        before = os.fstat(stream.fileno())
        require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1 and
                0 <= before.st_size <= limit, "nonregular or oversized file")
        data = stream.read(limit + 1)
        after = os.fstat(stream.fileno())
        require(len(data) == before.st_size == after.st_size and
                before.st_mtime_ns == after.st_mtime_ns, "file changed during read")
        return data


def digest(data):
    return hashlib.sha256(data).hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON field")
        result[key] = value
    return result


def parse(data):
    return json.loads(data, object_pairs_hook=unique_object,
                      parse_constant=lambda value: require(False, "nonfinite JSON"))


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      allow_nan=False).encode()


def inventory(root, trusted_links=False):
    require(root.is_dir() and not root.is_symlink(), "invalid inventory root")
    result, total = {}, 0
    for path in sorted(root.rglob("*")):
        require(trusted_links or not path.is_symlink(), "symlink input")
        if path.is_dir():
            continue
        data = read(path.resolve() if trusted_links else path)
        total += len(data)
        require(len(result) < COUNT_LIMIT and total <= TOTAL_LIMIT,
                "input inventory budget")
        result[path.relative_to(root).as_posix()] = [len(data), digest(data)]
    return result


def normalize_generated_packages(root):
    # Only fresh, repository-authored fixture packages. ZIP creation timestamps
    # otherwise change identical fixture bytes on every acquisition attempt.
    directory = root / "output/resource-packages"
    if not directory.exists():
        return
    for path in sorted(directory.glob("*.bundle")):
        buffer = io.BytesIO()
        with zipfile.ZipFile(io.BytesIO(read(path))) as source:
            require(len(source.infolist()) <= 512 and
                    sum(i.file_size for i in source.infolist()) <= FILE_LIMIT,
                    "generated package budget")
            with zipfile.ZipFile(buffer, "w") as target:
                for info in source.infolist():
                    data = source.read(info)
                    info.date_time = (1980, 1, 1, 0, 0, 0)
                    target.writestr(info, data)
        path.write_bytes(buffer.getvalue())


def cpu_dispatch_identity(text):
    # Linux x86 runner only: retain dispatch-relevant public CPU properties,
    # never processor/APIC IDs, serials, hostnames or changing clock readings.
    allowed = {"vendor_id", "cpu family", "model", "stepping", "microcode", "flags"}
    records = []
    for section in text.strip().split("\n\n"):
        fields = {}
        for line in section.splitlines():
            key, separator, value = line.partition(":")
            key = key.strip()
            if separator and key in allowed:
                require(key not in fields, "duplicate CPU dispatch field")
                fields[key] = value.strip()
        require(all(fields.get(key) for key in ("vendor_id", "cpu family", "model", "flags")),
                "CPU dispatch identity unavailable")
        fields["flags"] = sorted(set(fields["flags"].split()))
        records.append(fields)
    require(0 < len(records) <= 4096, "CPU identity processor count")
    distinct = sorted({canonical(record) for record in records})
    return {"logicalProcessors": len(records),
            "dispatchClasses": [parse(record) for record in distinct]}


# These modules only produce fresh fixture bytes before identity is computed.
# Keep this explicit: an unknown module (even one named *_fixtures.py) hashes.
GENERATORS = frozenset({
    "gih_cadence_fixtures.py", "masked_fixtures.py", "option_fixtures.py",
    "hsv_joint_fixtures.py", "wet_drift_fixtures.py", "hsv_heldout_fixtures.py",
    "resource_fixtures.py", "hsv_pressure_fixtures.py", "sensor_fixtures.py",
    "mirror_fixtures.py", "texture_pickup_fixtures.py", "wet_hsv_fixtures.py",
    "timed_fixtures.py",
})
PURE_TESTS = frozenset({
    "application_test.py", "package_test.py", "oracle_cache_test.py",
    "hsv_joint_test.py", "kernel_precision_probe_test.py", "kernel_single_mark_probe_test.py",
    "wet_drift_contract_test.py", "gih_cadence_test.py", "test_stochastic_gate.py",
    "managed_lab_probe_test.py", "generated_tip_test.py", "bitmap_path_test.py",
    "paced_policy_test.py", "wet_hsv_combo_fixtures_test.py",
    "mask_repeat_contract_test.py", "mask_repeat_lifecycle_test.py", "view_control_contract_test.py",
})


def acquisition_scripts(source):
    # Runtime plugin modules also occur in the freshly copied data inventory.
    # Hash them here too, along with validators, prepare.py and unknown modules.
    return {p.relative_to(source).as_posix(): digest(read(p))
            for p in sorted(source.rglob("*.py"))
            if p.relative_to(source).as_posix() not in GENERATORS | PURE_TESTS}


def acquisition_workflow(data):
    # The selected group is separately keyed, as are its complete generated
    # inputs. Other groups' scheduling cannot alter this group's acquisition.
    # Normalize exactly one single-line selector; retain every other byte,
    # including job environment, runner, runtime installation and render gates.
    lines = data.splitlines(keepends=True)
    selectors = [i for i, line in enumerate(lines)
                 if line.startswith(b"        fixture_group: ")]
    require(len(selectors) == 1, "ambiguous source fixture matrix")
    index = selectors[0]
    require(lines[index].rstrip().endswith(b"}}"),
            "source fixture matrix must remain a single expression")
    # Captures are sealed before reporting and candidate compilation. Later
    # metadata-only changes must not force the source application to repaint.
    boundaries = [i for i, line in enumerate(lines)
                  if line == b"      - name: Report source parameter probes\n"]
    seals = [i for i, line in enumerate(lines)
             if line == b"      - name: Seal complete source acquisition\n"]
    require(len(boundaries) == len(seals) == 1 and
            index < seals[0] < boundaries[0], "ambiguous source seal boundary")
    lines[index] = b"        fixture_group: <selected-group-in-identity>\n"
    return digest(b"".join(lines[:boundaries[0]]))


def identity(root, repository, appimage):
    normalize_generated_packages(root)
    # The binary itself is too large for the response-file reader. Hash it in
    # fixed chunks, before extraction/capture, rather than trust its URL/version.
    h = hashlib.sha256()
    with appimage.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    source = repository / "ci-scripts/brush-reference"
    scripts = acquisition_scripts(source)
    # procfs reports zero st_size, so use an explicit read bound rather than
    # the regular on-disk cache-payload reader.
    with open("/proc/cpuinfo", "rb") as cpu:
        cpu_bytes = cpu.read(8 * 1024 * 1024 + 1)
    require(len(cpu_bytes) <= 8 * 1024 * 1024, "CPU identity byte budget")
    runtime = {
        "architecture": platform.machine(), "kernel": platform.release(),
        "cpu": cpu_dispatch_identity(cpu_bytes.decode("ascii")),
        "os": digest(read(Path("/etc/os-release").resolve())),
        "packages": digest(subprocess.check_output(
            ["dpkg-query", "-W", "-f=${Package}=${Version}\\n"])),
        "fonts": inventory(Path("/usr/share/fonts"), trusted_links=True),
        "fontconfig": inventory(Path("/etc/fonts"), trusted_links=True),
        "locale": {k: os.environ.get(k, "") for k in ("LANG", "LC_ALL", "TZ")},
        "runnerImage": os.environ.get("ImageVersion", ""),
        "python": platform.python_version(),
    }
    return {"schema": VERSION, "applicationSha256": h.hexdigest(),
            "group": os.environ["BRUSH_REFERENCE_GROUP"], "scripts": scripts,
            # Includes Xvfb command, environment and acquisition success gates.
            # Engine source/build outputs are deliberately absent from identity.
            "workflow": acquisition_workflow(read(repository / ".github/workflows/raster_brush_reference.yml")),
            "runtime": runtime, "data": inventory(root / "data"),
            "config": inventory(root / "config"), "inputs": inventory(root / "output")}


def filename(value, suffix):
    require(isinstance(value, str) and len(value) <= 240 and
            re.fullmatch(r"[A-Za-z0-9_.+-]+", value) and value.endswith(suffix),
            "invalid capture name")
    return value



def validate_spacing_scatter_acquisition(fixtures, status):
    # Only these complete original source matrices may replace deterministic
    # repeat equality with independent scatter sampling. Merely adding an
    # enabled flag to another fixture must never reduce its repeat gate.
    from runpy import run_path
    helper = run_path(str(Path(__file__).parent /
        "gzbrush_reference/spacing_scatter_probe.py"))
    try:
        helper["validate_manifest"](fixtures)
    except (RuntimeError, ValueError, KeyError, TypeError, AttributeError) as error:
        raise ValueError("invalid complete spacing/scatter manifest") from error
    expected = fixtures["presets"]
    planned, actual = status.get("plannedExecutionOrder"), status.get("actualExecutionOrder")
    require(status.get("intendedPresetOrder") == expected and type(planned) is list and
            len(planned) == len(expected) and all(type(name) is str for name in planned) and
            len(set(planned)) == len(expected) and set(planned) == set(expected) and
            actual == planned, "missing spacing/scatter execution order evidence")
    cases = fixtures["cases"]
    scatter = fixtures["group"] != "autospacing"
    if scatter:
        enabled = [case["preset"] for case in cases if case["scatterProbe"]["enabled"]]
        active_plan = [name for name in planned if name in set(enabled)]
        require(len(enabled) == len(active_plan) == 100 and active_plan != enabled,
                "source scatter execution was not randomized")
    else:
        require(planned == expected, "unexpected automatic-spacing execution order")
    order = {name: index for index, name in enumerate(planned)}
    for index, (case, row) in enumerate(zip(cases, status["records"])):
        require(type(row.get("intendedOrdinal")) is int and row["intendedOrdinal"] == index and
                type(row.get("executionOrdinal")) is int and row["executionOrdinal"] == order[case["preset"]],
                "source spacing/scatter execution ordinal mismatch")
        field = "scatterEvidence" if scatter else "autoSpacingEvidence"
        evidence = row.get(field, {})
        require(isinstance(evidence, dict), "missing spacing/scatter source evidence")
        fields = dict(evidence.get("effectiveControls", {}))
        fields["brush_definition"] = evidence.get("effectiveBrushDefinition", "")
        try:
            validated = helper["validate_controls"](case, fields, row.get("deliveryReport", {}))
        except (RuntimeError, ValueError, KeyError, TypeError, AttributeError) as error:
            raise ValueError("invalid effective spacing/scatter controls or delivery") from error
        require(all(evidence.get(key) == value for key, value in validated.items()) and
                evidence.get("encoding") == "zlib-base64-bgra8-row-major",
                "spacing/scatter source evidence differs from readback")
        rect = validated["rect"]
        size = rect[2] * rect[3] * 4
        data = evidence.get("data")
        require(type(data) is str and len(data) <= 2 * size + 1024,
                "oversized spacing/scatter source pixels")
        stream = zlib.decompressobj()
        try:
            pixels = stream.decompress(base64.b64decode(data, validate=True), size + 1)
        except (ValueError, zlib.error) as error:
            raise ValueError("invalid encoded spacing/scatter source pixels") from error
        require(len(pixels) == size and stream.eof and not stream.unused_data and
                not stream.unconsumed_tail and digest(pixels) == evidence.get("sha256Pixels") and
                type(evidence.get("nonzeroAlphaPixels")) is int and
                evidence["nonzeroAlphaPixels"] == sum(alpha != 0 for alpha in pixels[3::4]) and
                evidence["nonzeroAlphaPixels"] > 0,
                "invalid spacing/scatter source pixel evidence")
    return scatter


def acquisition(output, fixtures):
    require(fixtures.get("license") == "CC0-1.0", "non-original fixture license")
    status_bytes = read(output / "oracle-status.json")
    status = parse(status_bytes)
    expected = fixtures["presets"]
    cases = fixtures["cases"]
    rows = status["records"]
    require(status["status"] == "complete" and status["version"] == fixtures["version"]
            and 0 < len(expected) <= 512 and len(set(expected)) == len(expected)
            and len(cases) == len(expected) and
            [r["preset"] for r in rows] == expected, "incomplete acquisition")
    spacing_scatter_group = fixtures.get("group") == "autospacing" or fixtures.get("group", "").startswith("scatter-")
    validated_scatter = validate_spacing_scatter_acquisition(fixtures, status) if spacing_scatter_group else False
    lifecycle = mask_repeat_lifecycle_contract.contract(fixtures["group"])
    if lifecycle:
        mask_repeat_lifecycle_contract.validate_manifest(fixtures)
    diagnostic = lifecycle or mask_repeat_contract(fixtures["group"])
    if diagnostic:
        observed = status.get("repeatDiagnostic", {})
        require(all(observed.get(key) == value for key, value in diagnostic.items()) and
                observed.get("configurationReadback") == ("1" if diagnostic["configuredMaxThreads"] == 1 else "<default>") and
                observed.get("configurationKey") == "maxNumberOfThreads" and
                observed.get("configurationGroup") == "", "source worker diagnostic readback missing")
    files = {"oracle-status.json": status_bytes}
    for case, row in zip(cases, rows):
        require(all(row.get(k) == v for k, v in case.items()), "stale case inputs")
        if "driftProbe" in case:
            evidence = row.get("driftEvidence", {})
            spec = case["driftProbe"]
            require(evidence.get("nominalDabs") == spec["nominalDabs"] and
                    evidence.get("observedDepositCells") == spec["nominalDabs"] - (0 if spec["calibration"] else 1) and
                    evidence.get("calibration") == spec["calibration"] and
                    evidence.get("singleContinuousStroke") is True and
                    evidence.get("firstCellUnchanged") == (not spec["calibration"]) and
                    type(evidence.get("maximumCellOffset")) is int and 0 <= evidence["maximumCellOffset"] <= 5 and
                    (not spec["calibration"] or evidence.get("calibrationAlpha") in (127, 128)),
                    "missing source drift count evidence")
        if lifecycle:
            lifecycle_evidence = [row.get("maskLifecycleEvidence")] + row.get("maskLifecycleRepeatEvidence", [])
            require(len(lifecycle_evidence) == mask_repeat_lifecycle_contract.repeat_count(case),
                    "lifecycle capture evidence missing")
            for evidence in lifecycle_evidence:
                require(isinstance(evidence, dict) and evidence.get("inputApi") == case["delivery"] and
                        evidence.get("deliveryValuesMatchAuthored") is True and
                        evidence.get("sourceParityQualified") is False and
                        evidence.get("repeatDiagnostic") == lifecycle and
                        evidence.get("rect") == case["maskRepeatProbe"]["maskProbe"]["rect"] and
                        HEX.fullmatch(evidence.get("sha256Pixels", "")),
                        "lifecycle source controls or raster evidence missing")
                # Revalidate retained contacts against the same fixed public
                # Qt enum/sequence/value contract as source acquisition.
                contacts = evidence.get("deliveredContacts", [])
                mask_repeat_lifecycle_contract.validate_delivery(
                    case, {"mode": evidence["inputApi"], "events": contacts}, contacts_only=True)
                fields = dict(evidence.get("effectiveControls", {}),
                              brush_definition=evidence.get("effectivePrimaryBrush", ""))
                mask_repeat_lifecycle_contract.controls(case, fields)
        repeated = row["repeatSha256Pixels"]
        repetitions = 3 if fixtures["group"] == "geometry" else (
            2 if case.get("variant") == "baseline" or fixtures["group"] == "wetboundary" else 1)
        diagnostic = lifecycle or mask_repeat_contract(fixtures["group"])
        if diagnostic:
            require(fixtures.get("repeatDiagnostic") == diagnostic and row.get("repeatDiagnostic") == diagnostic,
                    "missing mask-repeat diagnostic identity")
            repetitions = (mask_repeat_lifecycle_contract.repeat_count(case)
                           if lifecycle else diagnostic["repetitions"])
        if validated_scatter and case["scatterProbe"]["enabled"]:
            repetitions = 1
        canonical = row["sha256RepeatPixels"]
        require(HEX.fullmatch(row["sha256Pixels"]) and len(repeated) == repetitions
                and repeated[0] == row["sha256Pixels"] and all(HEX.fullmatch(h) for h in repeated)
                and row["repeatSemantics"] == "transparent-rgb-zero-v1"
                and HEX.fullmatch(canonical)
                and row["repeatSha256CanonicalPixels"] == [canonical] * repetitions,
                "repeat gate missing")
        require(len(row["repeatDifferences"]) == repetitions - 1
                and all(type(d["visibleOrAlphaChangedPixels"]) is int
                        and d["visibleOrAlphaChangedPixels"] == 0
                        and type(d["hiddenRgbOnlyPixels"]) is int
                        and 0 <= d["hiddenRgbOnlyPixels"] <= fixtures["width"] * fixtures["height"]
                        for d in row["repeatDifferences"]), "repeat classification missing")
        require(all((repeated[index] == repeated[0]) == (difference["hiddenRgbOnlyPixels"] == 0)
                    for index, difference in enumerate(row["repeatDifferences"], 1)),
                "raw repeat hashes disagree with classification")
        require(len(row["repeatImages"]) == repetitions - 1, "raw repeat captures missing")
        for index, repeat_image in enumerate(row["repeatImages"], 1):
            name = filename(repeat_image["image"], ".png")
            require(name == row["preset"].replace(" ", "_") + ".repeat-" + str(index) + ".png"
                    and name not in files, "invalid raw repeat capture name")
            data = read(output / name)
            require(digest(data) == repeat_image["sha256File"] and data[:8] == b"\x89PNG\r\n\x1a\n"
                    and data[12:16] == b"IHDR" and len(data) >= 33
                    and struct.unpack(">II", data[16:24]) == (fixtures["width"], fixtures["height"]),
                    "raw repeat capture mismatch")
            files[name] = data
        view = row["effectiveView"]
        for key, tolerance in (("diameter", 1e-5), ("opacity", 1 / 255 + 1e-9),
                               ("flow", 1 / 255 + 1e-9)):
            require(isinstance(view[key], (int, float)) and math.isfinite(view[key]) and
                    abs(view[key] - case[key]) <= tolerance, "source control mismatch")
        # Preserve the measured resource-only guard and signed/wrapped source
        # orientations; cache admission uses the same pure guard as acquisition.
        pressure_flag = case.get("sourcePressureFlag", False)
        require(type(pressure_flag) is bool and view["disablePressure"] is pressure_flag,
                "source pressure mode mismatch")
        require(rotation_matches(view["rotation"], case.get("brushRotationDegrees", 0),
                                 resource="resourceProbe" in case)
                and view["blend"] == case.get("overrides", {}).get("CompositeOp", "normal")
                and all(view[k] is False for k in ("eraser", "alphaLock")),
                "source modifier mismatch")
        require(row["profile"] == "sRGB-elle-V2-srgbtrc.icc", "source color profile mismatch")
        if case.get("expectUnchanged", False):
            require(row["changed"] is False, "source no-op mismatch")
        elif "changeExpectation" not in case:
            require(row["changed"] is True, "source stroke missing")
        if case.get("generatedTipProbe"):
            from runpy import run_path
            evidence = row.get("generatedTipEvidence", {})
            validate_tip = run_path(str(Path(__file__).parent / "gzbrush_reference/generated_tip_probe.py"))["controls"]
            require(validate_tip(case, evidence.get("brushDefinition", "")) == evidence,
                    "generated tip readback mismatch")
        for field, suffix in (("image", ".png"), ("effectivePresetXml", ".xml")):
            name = filename(row[field], suffix)
            expected_name = row["preset"].replace(" ", "_") + (
                ".png" if field == "image" else ".effective.xml")
            require(name == expected_name, "capture does not belong to original preset")
            require(name not in files, "duplicate capture filename")
            data = read(output / name)
            if field == "image":
                require(data[:8] == b"\x89PNG\r\n\x1a\n" and data[12:16] == b"IHDR"
                        and len(data) >= 33 and struct.unpack(">II", data[16:24]) ==
                        (fixtures["width"], fixtures["height"]), "source image geometry")
            else:
                require(digest(data) == row["effectivePresetSha256"], "effective preset hash")
            files[name] = data
        require(sum(map(len, files.values())) <= TOTAL_LIMIT, "capture byte budget")
    if fixtures.get("group") == "wetkernelcount":
        from runpy import run_path
        data = read(output / "kernel-precision.json", 4 * 1024 * 1024)
        validate_precision = run_path(str(Path(__file__).parent /
            "gzbrush_reference/kernel_precision_probe.py"))["validate"]
        validate_precision(parse(data), fixtures, rows)
        files["kernel-precision.json"] = data
        convergence = read(output / "kernel-convergence.json", 8 * 1024 * 1024)
        convergence_report = parse(convergence)
        validate_convergence = run_path(str(Path(__file__).parent /
            "gzbrush_reference/kernel_convergence_probe.py"))["validate"]
        validate_convergence(convergence_report, fixtures)
        require(convergence_report.get("kernelPrecisionSha256") == digest(data),
                "kernel convergence/precision observation linkage mismatch")
        files["kernel-convergence.json"] = convergence
        single_mark = read(output / "kernel-single-mark.json", 4 * 1024 * 1024)
        validate_single_mark = run_path(str(Path(__file__).parent /
            "gzbrush_reference/kernel_single_mark_probe.py"))["validate"]
        validate_single_mark(parse(single_mark), fixtures, rows)
        files["kernel-single-mark.json"] = single_mark
    if fixtures.get("group") in ("wetkernelcount", "lab-conversion"):
        from runpy import run_path
        lab = read(output / "managed-lab.json", 1024 * 1024)
        run_path(str(Path(__file__).parent / "gzbrush_reference/managed_lab_probe.py"))["validate"](parse(lab))
        files["managed-lab.json"] = lab
        require(sum(map(len, files.values())) <= TOTAL_LIMIT, "capture byte budget")
    require(len(files) <= COUNT_LIMIT, "capture count budget")
    return files


def seal(root, cache, expected):
    files = acquisition(root / "output", parse(read(root / "output/fixtures.json")))
    require(not cache.exists(), "cache destination already exists")
    cache.mkdir()
    for name, data in files.items():
        (cache / name).write_bytes(data)
    manifest = {"schema": VERSION, "identity": expected, "sourceExitCode": 0,
                "files": {k: [len(v), digest(v)] for k, v in files.items()}}
    (cache / "manifest.json").write_bytes(canonical(manifest))


def restore(root, cache, expected):
    require(cache.is_dir() and not cache.is_symlink(), "missing cache")
    manifest = parse(read(cache / "manifest.json", 16 * 1024 * 1024))
    require(set(manifest) == {"schema", "identity", "sourceExitCode", "files"} and
            manifest["schema"] == VERSION and manifest["identity"] == expected and
            manifest["sourceExitCode"] == 0, "stale cache provenance")
    entries = manifest["files"]
    require(isinstance(entries, dict) and 0 < len(entries) <= COUNT_LIMIT and
            {p.name for p in cache.iterdir()} == set(entries) | {"manifest.json"},
            "unexpected or missing cache files")
    total = 0
    for name, spec in entries.items():
        filename(name, ".json" if name in ("oracle-status.json", "kernel-precision.json", "kernel-convergence.json", "kernel-single-mark.json", "managed-lab.json") else
                 ".png" if name.endswith(".png") else ".xml")
        require(isinstance(spec, list) and len(spec) == 2 and type(spec[0]) is int
                and 0 <= spec[0] <= FILE_LIMIT and HEX.fullmatch(spec[1]), "invalid file manifest")
        total += spec[0]
        require(total <= TOTAL_LIMIT, "cache byte budget")
        data = read(cache / name, spec[0])
        require(digest(data) == spec[1], "corrupt cache payload")
    fixtures = parse(read(root / "output/fixtures.json"))
    files = acquisition(cache, fixtures)
    require(set(files) == set(entries), "unreferenced cache payload")
    # Validate everything before touching fresh prepared fixture files.
    for name, data in files.items():
        require(not (root / "output" / name).exists(), "capture overwrites fresh input")
    created = []
    try:
        for name, data in files.items():
            path = root / "output" / name
            # Exclusive creation also closes the gap after the existence check.
            # Track the file before writing: a failed write can leave a prefix.
            with path.open("xb") as stream:
                created.append(path)
                require(stream.write(data) == len(data), "short capture write")
    except BaseException as failure:
        # Roll back only files successfully created by this restore. Never
        # delete an existing fixture if exclusive creation itself failed.
        rollback_failed = False
        for path in reversed(created):
            try:
                path.unlink(missing_ok=True)
            except OSError:
                rollback_failed = True
        if rollback_failed:
            # Not caught by the ordinary cache-miss fallback: fresh capture
            # must not proceed on a directory we could not restore to baseline.
            raise RuntimeError("Cannot roll back source cache output; refusing capture") from failure
        raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("identity", "restore", "seal"))
    args = parser.parse_args()
    root = Path(os.environ["BRUSH_REFERENCE_ROOT"])
    cache = Path(".brush-source-cache")
    identity_path = Path("reference-runtime/oracle-identity.json")
    if args.action == "identity":
        expected = identity(root, Path.cwd(), Path("reference-runtime/krita.AppImage"))
        identity_path.write_bytes(canonical(expected))
        with open(os.environ["GITHUB_OUTPUT"], "a") as out:
            out.write("key=brush-source-v1-" + digest(canonical(expected)) + "\n")
    elif args.action == "restore":
        hit = False
        try:
            restore(root, cache, parse(read(identity_path, 16 * 1024 * 1024)))
            hit = True
            print("Validated complete original source cache; current-head comparison still required")
        except (ValueError, OSError, KeyError, TypeError, AttributeError,
                RecursionError, OverflowError, struct.error) as error:
            print("Source cache unavailable or invalid; fresh acquisition required: " + type(error).__name__)
            if cache.is_symlink() or (cache.exists() and not cache.is_dir()):
                cache.unlink()
            elif cache.exists():
                shutil.rmtree(cache)
        with open(os.environ["GITHUB_OUTPUT"], "a") as out:
            out.write("validated=" + str(hit).lower() + "\n")
    else:
        seal(root, cache, parse(read(identity_path, 16 * 1024 * 1024)))


if __name__ == "__main__":
    main()
