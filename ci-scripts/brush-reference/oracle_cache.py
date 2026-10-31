"""Content-addressed original source captures, never candidate qualification.

Only repository-generated CC0 cases and their source render/effective settings
are cached. No downloaded application, bundled brushes, logs, local paths or
credentials are persisted. Cache integrity assumes GitHub's repository/branch
cache access boundary; hashes detect damage, not a malicious authorized writer.
"""
import argparse
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import struct
import subprocess
import zipfile

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
    "hsv_joint_test.py", "kernel_precision_probe_test.py",
    "wet_drift_contract_test.py", "gih_cadence_test.py", "test_stochastic_gate.py",
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
    lines[index] = b"        fixture_group: <selected-group-in-identity>\n"
    return digest(b"".join(lines))


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
            re.fullmatch(r"[A-Za-z0-9_.-]+", value) and value.endswith(suffix),
            "invalid capture name")
    return value


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
        repeated = row["repeatSha256Pixels"]
        repetitions = 3 if fixtures["group"] == "geometry" else (
            2 if case.get("variant") == "baseline" or fixtures["group"] == "wetboundary" else 1)
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
        # Resource fixtures expose a measured source UI roundtrip of
        # 90.0002104591497 degrees for a requested 90. Preserve that readback;
        # match the bounded resource-only acquisition guard, not pixel parity.
        rotation_tolerance = .001 if "resourceProbe" in case else 1e-5
        pressure_flag = case.get("sourcePressureFlag", False)
        require(type(pressure_flag) is bool and view["disablePressure"] is pressure_flag,
                "source pressure mode mismatch")
        require(abs(view["rotation"] - case.get("brushRotationDegrees", 0)) <= rotation_tolerance
                and view["blend"] == case.get("overrides", {}).get("CompositeOp", "normal")
                and all(view[k] is False for k in ("eraser", "alphaLock")),
                "source modifier mismatch")
        require(row["profile"] == "sRGB-elle-V2-srgbtrc.icc", "source color profile mismatch")
        if case.get("expectUnchanged", False):
            require(row["changed"] is False, "source no-op mismatch")
        elif "changeExpectation" not in case:
            require(row["changed"] is True, "source stroke missing")
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
        filename(name, ".json" if name in ("oracle-status.json", "kernel-precision.json") else
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
