#!/usr/bin/env python3
"""Retain a failed native run's existing Windows binaries, never rebuild them.

PE/CodeView and PDB identities follow Microsoft's PE format and LLVM's MSF/PDB
Info Stream documentation. Only GUID/age identity is read; no code is executed.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PureWindowsPath
import re
import stat
import struct
import tempfile
import time
import zipfile


MESA_VERSION = "26.2.4"
MESA_ARCHIVE_SHA256 = "351fc8c8b695878ffb3eaa044b3ead08672a48b1a045e3c3e3975811df0f6695"
MESA_FILES = ("opengl32.dll", "libgallium_wgl.dll")
NATIVE_FLAGS = frozenset(("--require-function-workflow", "--require-offline-gl-workflow",
                          "--require-vector-workflows", "--require-combined-workflows",
                          "--windows-crash-debugger"))
FORBIDDEN_NAMES = frozenset((".git", ".ssh", ".aws", ".azure", ".kube", ".codex", ".claude", ".env",
                            ".git-credentials", ".netrc", ".npmrc", ".pypirc", "id_rsa", "id_ed25519",
                            "credentials", "credentials.json", "secrets", "secrets.json",
                            "crashdumps", "diagnosticreports", "native-ui-fixture",
                            "seexprsdkfixture.dll", "seexprinvalidfixture.dll"))
FORBIDDEN_SUFFIXES = (".dmp", ".mdmp", ".core", ".pem", ".key", ".pfx", ".p12", ".exe.local")
BUILD_ONLY_SUFFIXES = (".pdb", ".lib", ".exp", ".obj", ".ilk", ".ipdb", ".iobj")


class BundleError(ValueError):
    pass


def _require(condition, message):
    if not condition:
        raise BundleError(message)


def _plain(path, directory=False):
    info = path.lstat()
    _require(not path.is_symlink() and not (getattr(info, "st_file_attributes", 0) & 0x400),
             "Symbolic links and reparse points cannot enter a native bundle")
    _require(stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode),
             "Native bundle input has an unexpected file type")
    return info


def _read_at(stream, offset, count):
    _require(0 <= offset and 0 <= count <= 16 * 1024 * 1024,
             "Debug identity exceeds its bounded read")
    stream.seek(offset)
    data = stream.read(count)
    _require(len(data) == count, "Truncated debug identity")
    return data


def pe_identity(path):
    """Return the RSDS GUID, age and basename, or None without CodeView data."""
    with path.open("rb") as stream:
        dos = _read_at(stream, 0, 64)
        _require(dos[:2] == b"MZ", "Packaged native binary is not a PE image")
        pe = struct.unpack_from("<I", dos, 60)[0]
        _require(pe <= 16 * 1024 * 1024, "Invalid PE header offset")
        header = _read_at(stream, pe, 24)
        _require(header[:4] == b"PE\0\0", "Invalid PE signature")
        sections = struct.unpack_from("<H", header, 6)[0]
        optional_size = struct.unpack_from("<H", header, 20)[0]
        _require(1 <= sections <= 96 and 96 <= optional_size <= 4096,
                 "Invalid PE section or optional-header count")
        optional = _read_at(stream, pe + 24, optional_size)
        magic = struct.unpack_from("<H", optional)[0]
        _require(magic in (0x10B, 0x20B), "Unsupported PE optional header")
        directory_offset = 112 if magic == 0x20B else 96
        _require(len(optional) >= directory_offset, "Truncated PE data directories")
        directory_count = struct.unpack_from("<I", optional, directory_offset - 4)[0]
        if directory_count <= 6:
            return None
        _require(len(optional) >= directory_offset + 56, "Missing PE debug directory")
        debug_rva, debug_size = struct.unpack_from("<II", optional, directory_offset + 48)
        if not debug_rva and not debug_size:
            return None
        _require(0 < debug_size <= 28 * 128 and debug_size % 28 == 0,
                 "Invalid PE debug-directory size")
        table = _read_at(stream, pe + 24 + optional_size, sections * 40)
        debug_offset = None
        for i in range(sections):
            virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", table, i * 40 + 8)
            if virtual_address <= debug_rva and debug_rva - virtual_address + debug_size <= raw_size:
                _require(debug_offset is None, "Ambiguous PE debug-directory mapping")
                debug_offset = raw_offset + debug_rva - virtual_address
        _require(debug_offset is not None, "Unmapped PE debug directory")
        debug = _read_at(stream, debug_offset, debug_size)
        identities = []
        for i in range(0, debug_size, 28):
            kind, length, _, offset = struct.unpack_from("<IIII", debug, i + 12)
            if kind != 2:
                continue
            _require(25 <= length <= 4096, "Invalid CodeView record size")
            record = _read_at(stream, offset, length)
            _require(record[:4] == b"RSDS", "Unsupported CodeView record")
            _require(b"\0" in record[24:], "Unterminated CodeView PDB name")
            name = PureWindowsPath(record[24:].split(b"\0", 1)[0].decode("utf-8", "strict")).name
            _require(name.lower().endswith(".pdb") and name not in ("", ".", ".."),
                     "Invalid CodeView PDB basename")
            identities.append((record[4:20].hex(), struct.unpack_from("<I", record, 20)[0], name))
        _require(len(identities) <= 1, "Ambiguous CodeView PDB identity")
        return identities[0] if identities else None


def pdb_identity(path):
    """Read only the MSF7 stream directory and PDB Info Stream header."""
    with path.open("rb") as stream:
        header = _read_at(stream, 0, 56)
        _require(header[:32] == b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0",
                 "Unsupported PDB container")
        block, _, blocks, directory_size, _, block_map = struct.unpack_from("<6I", header, 32)
        _require(block in (512, 1024, 2048, 4096) and blocks * block == path.stat().st_size,
                 "Invalid PDB block layout")
        count = (directory_size + block - 1) // block
        _require(12 <= directory_size <= 16 * 1024 * 1024 and count * 4 <= block
                 and block_map < blocks, "PDB directory exceeds bounded layout")
        indexes = struct.unpack("<" + "I" * count, _read_at(stream, block_map * block, count * 4))
        _require(all(index < blocks for index in indexes), "PDB directory block out of bounds")
        directory = b"".join(_read_at(stream, index * block, block) for index in indexes)[:directory_size]
        streams = struct.unpack_from("<I", directory)[0]
        _require(2 <= streams <= (len(directory) - 4) // 4, "Invalid PDB stream count")
        sizes = struct.unpack_from("<" + "I" * streams, directory, 4)
        _require(28 <= sizes[1] < 0xFFFFFFFF, "Missing PDB Info Stream")
        skip = 0 if sizes[0] == 0xFFFFFFFF else (sizes[0] + block - 1) // block
        pointer = 4 + streams * 4 + skip * 4
        _require(pointer + 4 <= len(directory), "Truncated PDB Info Stream mapping")
        info_block = struct.unpack_from("<I", directory, pointer)[0]
        _require(info_block < blocks, "PDB Info Stream block out of bounds")
        info = _read_at(stream, info_block * block, 28)
        return info[12:28].hex(), struct.unpack_from("<I", info, 8)[0]


def _release(path, source_sha, run_id, run_attempt, step_start, native_outcome):
    info = _plain(path)
    _require(info.st_size <= 64 * 1024, "Release report exceeds size limit")
    data = json.loads(path.read_text(encoding="utf-8"))
    _require(isinstance(data, dict) and data.get("schema") == 1, "Missing release report schema")
    _require(native_outcome == "failure", "Only a failed native step may retain binaries")
    _require(data.get("source_sha") == source_sha and data.get("run_id") == str(run_id)
             and data.get("run_attempt") == str(run_attempt), "Release report belongs to a different build/run")
    _require(data.get("safe_to_release") is True, "Native process cleanup is not confirmed")
    _require(type(data.get("original_pid")) is int and data["original_pid"] > 0,
             "Release report has no actual original process")
    for name in ("started_at", "finished_at"):
        value = data.get(name)
        _require(type(value) in (float, int) and math.isfinite(value), "Release report has no actual process time")
    _require(type(step_start) in (float, int) and math.isfinite(step_start) and step_start > 0
             and step_start <= data["started_at"] <= data["finished_at"]
             <= time.time() + 5 and info.st_mtime >= step_start, "Stale or future release report")
    _require(type(data.get("timed_out")) is bool, "Release timeout state is missing")
    status = data.get("original_status")
    _require(status is None if data["timed_out"] else type(status) is int,
             "Release report has inconsistent original status")
    return {key: data[key] for key in ("schema", "source_sha", "run_id", "run_attempt", "original_pid",
                                      "timed_out", "original_status", "safe_to_release", "started_at", "finished_at")}


def prepare_bundle(package, build, mesa, release_report, output, *, source_sha, run_id, run_attempt,
                   native_step_started_at, native_outcome="failure", flags=(), recorder_build_id="",
                   max_bytes=8 * 1024 ** 3, max_files=50000, timeout=240):
    package, build, mesa, release_report, output = map(Path, (package, build, mesa, release_report, output))
    _require(re.fullmatch(r"[0-9a-fA-F]{40}", source_sha) is not None, "Invalid source revision")
    _require(str(run_id).isdigit() and str(run_attempt).isdigit(), "Invalid run identity")
    _require(all(flag in NATIVE_FLAGS for flag in flags), "Unknown native fixture flag")
    _require(not recorder_build_id or re.fullmatch(r"[0-9a-fA-F]{40}-[0-9]+-[0-9]+", recorder_build_id),
             "Invalid recorder build identity")
    _require(not recorder_build_id or recorder_build_id == f"{source_sha}-{run_id}-{run_attempt}",
             "Recorder identity does not match the retained build")
    release = _release(release_report, source_sha, run_id, run_attempt, native_step_started_at, native_outcome)
    for root in (package, build, mesa):
        _plain(root, directory=True)
    _require(not output.exists() and not output.is_symlink(), "Refusing to replace an existing bundle")
    _plain(output.parent, directory=True)
    _require(not any(root.resolve() in output.resolve().parents
                     for root in (package, build, mesa)), "Bundle output must be outside its input trees")
    deadline = time.monotonic() + timeout

    def check_time():
        _require(time.monotonic() <= deadline, "Native bundle preparation exceeded its deadline")

    def digest(path):
        check_time()
        _plain(path)
        value = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                check_time()
                value.update(chunk)
        return value.hexdigest()

    for root in (package, build):
        for child in root.iterdir():
            name = child.name.lower()
            _require(name not in MESA_FILES and not name.endswith(".exe.local")
                     and name not in ("seexprsdkfixture.dll", "seexprinvalidfixture.dll"),
                     "Temporary native renderer or SDK fixture remains in the runtime")
    _plain(package / "portablestuff", directory=True)
    build_files = {}
    for path in build.iterdir():
        name = path.name.casefold()
        _require(name not in build_files, "Ambiguous build filenames")
        build_files[name] = path
    files, symbols, binaries = [], {}, []
    total = 0

    def add(path, name):
        nonlocal total
        check_time()
        info = _plain(path)
        total += info.st_size
        _require(total <= max_bytes and len(files) < max_files, "Native bundle exceeds its file/byte budget")
        files.append((path, name, info.st_size, digest(path)))

    def traversal_error(error):
        raise error  # Never publish an apparently complete but partial runtime.

    for directory, dirs, names in os.walk(package, followlinks=False, onerror=traversal_error):
        check_time()
        for name in sorted(dirs + names):
            path = Path(directory) / name
            relative = path.relative_to(package)
            parts = tuple(part.lower() for part in relative.parts)
            _require(not any(":" in part or part in FORBIDDEN_NAMES or part.startswith((".env.", "groundzero-native-"))
                             for part in parts) and not name.lower().endswith(FORBIDDEN_SUFFIXES),
                     "Dump, credential, symbol or disposable fixture found in packaged input")
            _plain(path, directory=name in dirs)
        dirs.sort()
        for name in sorted(names):
            if name.lower().endswith(BUILD_ONLY_SUFFIXES):
                continue  # Only separately verified matching PDBs are retained.
            path = Path(directory) / name
            relative = path.relative_to(package)
            add(path, "package/" + relative.as_posix())
            if len(relative.parts) != 1 or path.suffix.lower() not in (".exe", ".dll"):
                continue
            counterpart = build_files.get(name.casefold())
            if counterpart is None:
                continue  # Deployed third-party dependencies have no build PDB claim.
            _require(digest(counterpart) == files[-1][3], "Packaged executable/DLL differs from its build counterpart")
            identity = pe_identity(path)
            matched = None
            if identity:
                pdb = build_files.get(identity[2].casefold())
                if pdb is not None:
                    _plain(pdb)
                    _require(pdb_identity(pdb) == identity[:2], "PDB GUID/age does not match its packaged binary")
                    matched = "symbols/" + pdb.name
                    symbols[pdb.name.casefold()] = (pdb, matched)
            binaries.append({"path": "package/" + name, "sha256": files[-1][3],
                             "build_counterpart_equal": True, "symbols": matched,
                             "codeview": {"guid": identity[0], "age": identity[1]} if identity else None})
    _require(any(item["path"].casefold() == "package/opentoonz.exe" and item["symbols"] for item in binaries),
             "The packaged OpenToonz executable and matching PDB are required")
    for pdb, name in sorted(symbols.values(), key=lambda item: item[1]):
        add(pdb, name)
    for name in MESA_FILES:
        add(mesa / name, "mesa/" + name)
    manifest = {"schema": 1, "purpose": "failure-only native diagnostics; not a release package",
                "source_sha": source_sha, "run_id": str(run_id), "run_attempt": str(run_attempt),
                "native_outcome": native_outcome, "release": release,
                "configuration": "RelWithDebInfo", "qt": "5.15.2-with-WinTab",
                "native_flags": list(flags), "recorder_build_id": recorder_build_id,
                "mesa": {"version": MESA_VERSION, "archive_sha256": MESA_ARCHIVE_SHA256,
                         "placement": "separate from package; use the native launcher"},
                "binaries": binaries, "files": [{"path": name, "bytes": size, "sha256": checksum}
                                                  for _, name, size, checksum in files]}
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix="native-bundle-", suffix=".zip", dir=output.parent, delete=False) as stream:
            temporary = Path(stream.name)
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as archive:
            for path, name, expected_size, expected_hash in files:
                check_time()
                _plain(path)
                value = hashlib.sha256()
                copied = 0
                with path.open("rb") as source, archive.open(name, "w", force_zip64=True) as target:
                    for chunk in iter(lambda: source.read(1024 * 1024), b""):
                        check_time()
                        copied += len(chunk)
                        _require(copied <= expected_size, "Bundle input changed during retention")
                        value.update(chunk)
                        target.write(chunk)
                _require(copied == expected_size and value.hexdigest() == expected_hash,
                         "Bundle input changed during retention")
            archive.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        check_time()
        _require(not output.exists(), "Refusing to replace an existing bundle")
        # Windows rename does not replace an existing destination. Portable tests
        # use the same link-based exclusive publication without a clobber window.
        os.link(temporary, output)
        temporary.unlink()
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("package", "build", "mesa", "release-report", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("source-sha", "run-id", "run-attempt", "native-outcome", "recorder-build-id"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--native-step-started-at", type=float, required=True)
    parser.add_argument("--flag", action="append", default=[])
    args = parser.parse_args(argv)
    try:
        manifest = prepare_bundle(args.package, args.build, args.mesa, args.release_report, args.output,
                                  source_sha=args.source_sha, run_id=args.run_id, run_attempt=args.run_attempt,
                                  native_step_started_at=args.native_step_started_at, native_outcome=args.native_outcome,
                                  flags=args.flag, recorder_build_id=args.recorder_build_id)
    except (BundleError, OSError, UnicodeError, json.JSONDecodeError, struct.error) as error:
        # Do not print input paths, registry data, sidecar contents or credentials.
        detail = str(error) if isinstance(error, BundleError) else type(error).__name__
        print("::warning title=Native binary retention::Bundle not retained: " + detail)
        return 1
    print("Native binary retention: verified " + str(len(manifest["files"])) + " files.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
