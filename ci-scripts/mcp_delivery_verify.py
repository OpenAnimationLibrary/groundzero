"""Verify a published render delivery independently of the worker that published it.

Reads delivery.json (at most 4 MiB, duplicate keys refused), checks its format and version
and, when given, its SHA-256; walks the directory without following links and requires
every listed file present, regular, single-link and equal in size and SHA-256, and nothing
unlisted; decodes every PNG frame and reads every EXR header against the folder's size and
sample type; with --ffprobe, probes every movie's video stream. Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import struct
import subprocess
import sys
from pathlib import Path
from typing import Any

from mcp_png import decode_png

MANIFEST = "delivery.json"
MANIFEST_LIMIT = 4 * 1024 * 1024
EXR_MAGIC = b"\x76\x2f\x31\x01"
EXR_SAMPLES = {"rgba-half-exr": 1, "rgba-float-exr": 2}


class DeliveryError(Exception):
    """The delivery differs from its manifest or its manifest is malformed."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise DeliveryError(message)


def _unique(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    keys = [key for key, _value in pairs]
    require(len(keys) == len(set(keys)), f"duplicate keys in {MANIFEST}: {keys}")
    return dict(pairs)


def read_manifest(directory: Path, expected_sha256: str | None) -> tuple[dict[str, Any], str]:
    """Read and parse delivery.json without following a link; return it and its digest."""
    path = directory / MANIFEST
    status = path.lstat()
    require(stat.S_ISREG(status.st_mode), f"{MANIFEST} is not a regular file")
    require(status.st_size <= MANIFEST_LIMIT, f"{MANIFEST} exceeds 4 MiB")
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if expected_sha256 is not None:
        require(digest == expected_sha256, f"{MANIFEST} digest {digest} != {expected_sha256}")
    manifest = json.loads(data, object_pairs_hook=_unique)
    require(manifest.get("format") == "groundzero-delivery", "not a groundzero delivery")
    require(manifest.get("version") == 1, f"unsupported version {manifest.get('version')}")
    return manifest, digest


def walk(directory: Path) -> dict[str, os.stat_result]:
    """Every entry below directory by relative path, links reported and never followed."""
    found: dict[str, os.stat_result] = {}
    pending = [Path()]
    while pending:
        relative = pending.pop()
        with os.scandir(directory / relative) as entries:
            for entry in entries:
                name = (relative / entry.name).as_posix()
                found[name] = entry.stat(follow_symlinks=False)
                if entry.is_dir(follow_symlinks=False):
                    pending.append(relative / entry.name)
    return found


def _check_files(directory: Path, manifest: dict[str, Any]) -> dict[str, bytes]:
    entries = walk(directory)
    listed = {row["path"]: row for row in manifest["files"]}
    folders = {row["folder"] for row in manifest["folders"]}
    expected = set(listed) | {MANIFEST} | folders
    require(set(entries) == expected, f"entries differ: {sorted(set(entries) ^ expected)}")
    contents: dict[str, bytes] = {}
    for path, row in listed.items():
        status = entries[path]
        require(stat.S_ISREG(status.st_mode), f"{path} is not a regular file")
        require(status.st_nlink == 1, f"{path} has another link")
        data = (directory / path).read_bytes()
        require(len(data) == row["size_bytes"], f"{path} size {len(data)} != manifest")
        require(hashlib.sha256(data).hexdigest() == row["sha256"], f"{path} digest differs")
        contents[path] = data
    return contents


def exr_header(data: bytes) -> tuple[int, int, set[int]]:
    """Width, height and channel sample types of a single-part scanline EXR header."""
    require(data[:4] == EXR_MAGIC, "not an OpenEXR file")
    offset, width, height, samples = 8, 0, 0, set()
    while data[offset] != 0:
        name_end = data.index(b"\0", offset)
        kind_end = data.index(b"\0", name_end + 1)
        name = data[offset:name_end].decode()
        (size,) = struct.unpack_from("<i", data, kind_end + 1)
        value = data[kind_end + 5 : kind_end + 5 + size]
        if name == "dataWindow":
            x0, y0, x1, y1 = struct.unpack("<4i", value)
            width, height = x1 - x0 + 1, y1 - y0 + 1
        if name == "channels":
            cursor = 0
            while value[cursor] != 0:
                cursor = value.index(b"\0", cursor) + 1
                samples.add(struct.unpack_from("<i", value, cursor)[0])
                cursor += 16
        offset = kind_end + 5 + size
    return width, height, samples


def _check_media(
    directory: Path, folder: dict[str, Any], rows: list[dict[str, Any]], ffprobe: Path | None
) -> None:
    size = (folder["width"], folder["height"])
    for row in rows:
        path = directory / row["path"]
        if row["media_type"] == "image/png":
            width, height, _pixels = decode_png(path.read_bytes())
            require((width, height) == size, f"{row['path']} decodes at {width}x{height}")
        elif row["media_type"] == "image/x-exr":
            width, height, samples = exr_header(path.read_bytes())
            require((width, height) == size, f"{row['path']} header says {width}x{height}")
            require(samples == {EXR_SAMPLES[folder["profile"]]}, f"{row['path']} samples")
        elif row["media_type"] == "video/mp4" and ffprobe is not None:
            _probe_movie(ffprobe, path, folder)


def _probe_movie(ffprobe: Path, path: Path, folder: dict[str, Any]) -> None:
    output = subprocess.run(
        [str(ffprobe), "-v", "error", "-show_streams", "-of", "json", str(path)],
        check=True,
        capture_output=True,
        timeout=60,
    ).stdout
    streams = json.loads(output)["streams"]
    video = [stream for stream in streams if stream["codec_type"] == "video"]
    require(len(video) == 1, f"{path.name} has {len(video)} video streams")
    require(
        (video[0]["width"], video[0]["height"]) == (folder["width"], folder["height"]),
        f"{path.name} video size differs",
    )


def verify(
    directory: Path, expected_sha256: str | None = None, ffprobe: Path | None = None
) -> dict[str, Any]:
    """Verify one delivery directory; return its manifest digest and what was checked."""
    manifest, digest = read_manifest(directory, expected_sha256)
    contents = _check_files(directory, manifest)
    for folder in manifest["folders"]:
        rows = [row for row in manifest["files"] if row["folder"] == folder["folder"]]
        require(len(rows) == folder["files"], f"folder {folder['folder']} file count differs")
        require(
            sum(row["size_bytes"] for row in rows) == folder["bytes"],
            f"folder {folder['folder']} byte count differs",
        )
        _check_media(directory, folder, rows, ffprobe)
    totals = manifest["totals"]
    require(totals["files"] == len(contents), "totals.files differs")
    require(totals["bytes"] == sum(len(data) for data in contents.values()), "totals.bytes")
    return {
        "manifest_sha256": digest,
        "files": len(contents),
        "bytes": totals["bytes"],
        "folders": [folder["folder"] for folder in manifest["folders"]],
        "sha256": {path: hashlib.sha256(data).hexdigest() for path, data in contents.items()},
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--manifest-sha256")
    parser.add_argument("--ffprobe", type=Path)
    args = parser.parse_args()
    try:
        report = verify(args.directory, args.manifest_sha256, args.ffprobe)
    except (DeliveryError, OSError, ValueError, KeyError) as error:
        print(json.dumps({"verified": False, "error": str(error)}))
        return 1
    report.pop("sha256")
    print(json.dumps({"verified": True, **report}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
