#!/usr/bin/env python3
"""Qualify the packaged private-runtime importer without modifying installation ACLs."""
from pathlib import Path
import shutil
import re
import struct
import subprocess
import sys
import tempfile


def main():
    runtime = Path(sys.argv[1]).resolve(strict=True)
    helper = runtime / "rasterbrushimport-helper.exe"
    # This script qualifies release packages, never a development-only runtime.
    debug_crt = re.compile(r"(?i)(?:(?:msvcp|vcruntime|concrt)[0-9_a-z]*d|ucrtbased)\.dll")
    assert not any(debug_crt.fullmatch(path.name) for path in runtime.rglob("*.dll")), \
        "Release brush runtime contains a non-redistributable Debug CRT"
    with tempfile.TemporaryDirectory(prefix="brush-package-") as temporary:
        temporary = Path(temporary)
        fixture = temporary / "original.gbr"
        fixture.write_bytes(struct.pack(">7I", 30, 2, 1, 1, 1, 0x47494D50, 25) + b"x\0\xff")
        before, after = temporary / "before.acl", temporary / "after.acl"
        def snapshot(destination):
            subprocess.run(["icacls", str(runtime), "/save", str(destination), "/t", "/q"],
                           stdout=subprocess.DEVNULL, check=True, timeout=30)
        snapshot(before)
        subprocess.run([str(helper), "--qualify-brush-import", str(fixture)],
                       check=True, timeout=120)
        snapshot(after)
        assert before.read_bytes() == after.read_bytes(), "Installed runtime ACLs changed"
        altered = temporary / "altered-runtime"
        shutil.copytree(runtime, altered)
        with (altered / "qt.conf").open("ab") as config:
            config.write(b"\n# integrity qualification\n")
        result = subprocess.run(
            [str(altered / helper.name), "--qualify-brush-import", str(fixture)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30, check=False)
        assert result.returncode != 0 and b"changed or is incomplete" in result.stderr, \
            "Changed private-runtime source was not rejected before launch"
    print("Packaged private Windows runtime imported GBR with unchanged installation ACLs")


if __name__ == "__main__":
    main()
