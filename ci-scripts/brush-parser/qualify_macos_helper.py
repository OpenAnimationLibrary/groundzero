#!/usr/bin/env python3
"""Exercise the deployed, signed helper, including its mandatory denial probes."""
import json
import plistlib
import re
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time


def startup_evidence(helper, pid, started):
    """Bound diagnostics to this helper; never dump unrelated crash reports."""
    commands = [
        ["/usr/bin/file", str(helper)],
        ["/usr/bin/codesign", "--verify", "--strict", "--verbose=4", str(helper)],
        ["/usr/bin/codesign", "--display", "--verbose=4", "--entitlements", "-", str(helper)],
        ["/usr/bin/otool", "-L", str(helper)],
        ["/usr/bin/log", "show", "--last", "2m", "--style", "compact",
         "--predicate", f'processID == {pid} OR (process == "sandboxd" AND eventMessage CONTAINS "{helper.name}")'],
    ]
    for command in commands:
        try:
            # Disk-backed output prevents a verbose system diagnostic from
            # allocating an unbounded Python buffer.
            with tempfile.TemporaryFile() as output:
                result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                        timeout=10, check=False)
                output.seek(0)
                print(f"Startup evidence {command[0]} (exit {result.returncode}):\n"
                      + output.read(32768).decode(errors="replace"), flush=True)
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f"Startup evidence unavailable: {command[0]}: {error}", flush=True)
    directories = [Path.home() / "Library/Logs/DiagnosticReports",
                   Path("/Library/Logs/DiagnosticReports")]
    # ReportCrash may finish after waitpid. Allow ten seconds for its report.
    deadline = time.monotonic() + 10
    while True:
        found = False
        for directory in directories:
            for report in sorted(directory.glob(helper.name + "*"))[-8:]:
                try:
                    if report.stat().st_mtime < started - 1 or not report.is_file():
                        continue
                    with report.open("rb") as stream:
                        data = stream.read(65536).decode(errors="replace")
                    # Recent files are scoped to this executable, including a
                    # crash that happened in libsecinit before entering main.
                    print(f"Helper crash report {report.name}:\n{data}", flush=True)
                    found = True
                except OSError as error:
                    print(f"Helper crash report unavailable: {error}", flush=True)
        if found or time.monotonic() >= deadline:
            if not found:
                print(f"No crash report produced for helper PID {pid}", flush=True)
            break
        time.sleep(0.5)


def main():
    helper = Path(sys.argv[1]).resolve(strict=True)
    # libsecinit requires CFBundleIdentifier in signed metadata even for a
    # standalone command-line executable. Validate every architecture before
    # launching, so packaging cannot silently remove the embedded section.
    identity = "org.openanimationlibrary.groundzero.brush-import"
    subprocess.run(["/usr/bin/codesign", "--verify", "--strict", str(helper)],
                   check=True, timeout=10)
    signature = subprocess.run(
        ["/usr/bin/codesign", "--display", "--verbose=4", str(helper)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True, timeout=10)
    assert f"Identifier={identity}" in signature.stdout.decode(), "Wrong helper signing identifier"
    architectures = subprocess.check_output(
        ["/usr/bin/lipo", "-archs", str(helper)], timeout=10).decode().split()
    assert architectures, "Helper has no Mach-O architecture"
    for architecture in architectures:
        metadata = subprocess.check_output(
            ["/usr/bin/otool", "-arch", architecture, "-P", str(helper)], timeout=10)
        plists = re.findall(rb"<\?xml.*?</plist>", metadata, re.DOTALL)
        assert len(plists) == 1, "Missing or ambiguous embedded helper Info.plist"
        assert plistlib.loads(plists[0])["CFBundleIdentifier"] == identity, \
            "Embedded helper identity does not match code signature"
    print("Signed helper embedded bundle identity verified for " + ", ".join(architectures), flush=True)
    # Original one-pixel GBR v2 fixture, independent of any distributed pack.
    fixture = struct.pack(">7I", 30, 2, 1, 1, 1, 0x47494D50, 25) + b"x\0\xff"
    with tempfile.TemporaryDirectory(prefix="brush-boundary-") as temporary:
        sentinel = Path(temporary) / "private"
        sentinel.write_bytes(b"private")
        response = Path(temporary) / "response"
        with response.open("wb") as output:
            started = time.time()
            with subprocess.Popen(
                [str(helper), "--raster-brush-import-stream", "probe.gbr", str(sentinel)],
                stdin=subprocess.PIPE, stdout=output, stderr=subprocess.PIPE,
                env={"MallocNanoZone": "0"}, cwd="/",
            ) as process:
                try:
                    _, stderr = process.communicate(fixture, timeout=30)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
                    raise
                result = subprocess.CompletedProcess(process.args, process.returncode,
                                                     stderr=stderr)
                pid = process.pid
        if result.returncode:
            startup_evidence(helper, pid, started)
            with response.open("rb") as wire:
                startup_diagnostic = wire.read(8192).decode(errors="replace")
            raise RuntimeError(f"Signed helper/denial probe failed: {result.returncode}: "
                               + result.stderr[:2048].decode(errors="replace")
                               + "\nBounded startup response: " + repr(startup_diagnostic))
        assert sentinel.read_bytes() == b"private", "Helper modified unrelated file"
        data = response.read_bytes()
        assert len(data) < 1024 * 1024 and data[:8] == b"GZBW0001"
        count, = struct.unpack_from(">I", data, 8)
        offset, entries = 12, {}
        assert 1 <= count <= 4
        for _ in range(count):
            name_size, size = struct.unpack_from(">II", data, offset)
            offset += 8
            assert 0 < name_size <= 80 and size <= 65536
            name = data[offset:offset + name_size].decode("ascii")
            offset += name_size
            assert name not in entries
            entries[name] = data[offset:offset + size]
            offset += size
        assert offset == len(data)
        manifest = json.loads(entries["result.json"])
        assert manifest["protocol"] == 2 and manifest["count"] == 1
        assert "0.gzbrush" in entries
    print("Packaged App Sandbox helper imported original GBR; file/network and mmap/Mach allocation denial probes passed")


if __name__ == "__main__":
    main()
