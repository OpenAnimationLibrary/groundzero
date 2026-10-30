#!/usr/bin/env python3
"""Exercise the deployed, signed helper, including its mandatory denial probes."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def main():
    helper = Path(sys.argv[1]).resolve(strict=True)
    # Original one-pixel GBR v2 fixture, independent of any distributed pack.
    fixture = struct.pack(">7I", 30, 2, 1, 1, 1, 0x47494D50, 25) + b"x\0\xff"
    with tempfile.TemporaryDirectory(prefix="brush-boundary-") as temporary:
        sentinel = Path(temporary) / "private"
        sentinel.write_bytes(b"private")
        response = Path(temporary) / "response"
        with response.open("wb") as output:
            result = subprocess.run(
                [str(helper), "--raster-brush-import-stream", "probe.gbr", str(sentinel)],
                input=fixture, stdout=output, stderr=subprocess.PIPE,
                env={"MallocNanoZone": "0"}, cwd="/", timeout=30, check=False,
            )
        if result.returncode:
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
