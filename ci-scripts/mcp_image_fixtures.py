"""Write small raster files in the formats intake admits, with a known pixel pattern.

Every encoder here is written from the format specification with the standard library, so
the pixels a worker reports back can be compared with an oracle that shares no code with
the native readers. JPEG has no standard-library encoder; it is made with ffmpeg.
"""

from __future__ import annotations

import struct
import subprocess
import zlib
from pathlib import Path

from mcp_png import SIGNATURE

Pixel = tuple[int, int, int, int]
WIDTH, HEIGHT = 4, 3


def pattern(x: int, y: int, *, alpha: bool = True) -> Pixel:
    """Return the 8-bit straight RGBA colour of one pixel; row 0 is the top."""
    return ((60 * x + 10) % 256, (90 * y + 20) % 256, 200, 128 if alpha and x == 3 else 255)


def _rows(alpha: bool) -> list[list[Pixel]]:
    return [[pattern(x, y, alpha=alpha) for x in range(WIDTH)] for y in range(HEIGHT)]


def _chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png16(*, alpha: bool = True) -> bytes:
    """A 16-bit RGB(A) PNG whose samples are the 8-bit pattern times 257 plus 0x40."""
    channels = 4 if alpha else 3
    raw = bytearray()
    for row in _rows(alpha):
        raw.append(0)
        for pixel in row:
            for value in pixel[:channels]:
                raw.extend(
                    struct.pack(">H", value * 257 if value in (0, 255) else value * 257 + 0x40)
                )
    ihdr = struct.pack(">IIBBBBB", WIDTH, HEIGHT, 16, 6 if alpha else 2, 0, 0, 0)
    body = _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", zlib.compress(bytes(raw)))
    return SIGNATURE + body + _chunk(b"IEND", b"")


def _ifd_entry(tag: int, kind: int, count: int, value: int) -> bytes:
    if kind == 3 and count == 1:
        return struct.pack("<HHIHH", tag, kind, count, value, 0)
    return struct.pack("<HHII", tag, kind, count, value)


def tiff(*, depth: int = 8, alpha: bool = True, dpi: int = 300, pages: int = 1) -> bytes:
    """An uncompressed little-endian RGB(A) TIFF, chunky, with a resolution in inches."""
    channels = 4 if alpha else 3
    pixels = bytearray()
    for row in _rows(alpha):
        for pixel in row:
            for value in pixel[:channels]:
                pixels.extend(struct.pack("<H", value * 257) if depth == 16 else bytes([value]))
    tags = 11 if alpha else 10
    ifd_size = 2 + 12 * tags + 4
    bits_at = 8 + len(pixels)
    resolution_at = bits_at + 2 * channels
    ifd_at = resolution_at + 16
    extras = struct.pack("<" + "H" * channels, *([depth] * channels))
    extras += struct.pack("<IIII", dpi, 1, dpi, 1)
    entries = [
        _ifd_entry(256, 3, 1, WIDTH),
        _ifd_entry(257, 3, 1, HEIGHT),
        _ifd_entry(258, 3, channels, bits_at),
        _ifd_entry(259, 3, 1, 1),
        _ifd_entry(262, 3, 1, 2),
        _ifd_entry(273, 4, 1, 8),
        _ifd_entry(277, 3, 1, channels),
        _ifd_entry(279, 4, 1, len(pixels)),
        _ifd_entry(282, 5, 1, resolution_at),
        _ifd_entry(283, 5, 1, resolution_at + 8),
    ]
    if alpha:
        entries.append(_ifd_entry(338, 3, 1, 2))
    next_ifd = ifd_at + ifd_size if pages > 1 else 0
    ifd = struct.pack("<H", tags) + b"".join(entries) + struct.pack("<I", next_ifd)
    data = b"II*\x00" + struct.pack("<I", ifd_at) + pixels + extras + ifd
    if pages > 1:
        data += struct.pack("<H", tags) + b"".join(entries) + struct.pack("<I", 0)
    return data


def bmp(*, bits: int = 24) -> bytes:
    """A bottom-up BITMAPINFOHEADER BMP, 24-bit BGR or 32-bit BGRX, 2835 pixels per metre."""
    stride = ((WIDTH * bits // 8) + 3) & ~3
    pixels = bytearray()
    for row in reversed(_rows(False)):
        line = bytearray()
        for red, green, blue, _alpha in row:
            line.extend(bytes([blue, green, red]) + (b"\xff" if bits == 32 else b""))
        pixels.extend(line + bytes(stride - len(line)))
    header = struct.pack(
        "<IiiHHIIiiII", 40, WIDTH, HEIGHT, 1, bits, 0, len(pixels), 2835, 2835, 0, 0
    )
    size = 14 + len(header) + len(pixels)
    return b"BM" + struct.pack("<IHHI", size, 0, 0, 54) + header + pixels


def tga(*, alpha: bool = True) -> bytes:
    """An uncompressed true-colour TGA, bottom-up, BGR(A)."""
    depth = 32 if alpha else 24
    header = struct.pack(
        "<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, WIDTH, HEIGHT, depth, 8 if alpha else 0
    )
    pixels = bytearray()
    for row in reversed(_rows(alpha)):
        for red, green, blue, opacity in row:
            pixels.extend(bytes([blue, green, red]) + (bytes([opacity]) if alpha else b""))
    return header + pixels


def jpeg(ffmpeg: Path, directory: Path, size: int = 16) -> bytes:
    """Encode a flat-coloured opaque JPEG with ffmpeg at the highest quality."""
    target = directory / "fixture.jpg"
    command = [
        str(ffmpeg), "-v", "error", "-y", "-f", "lavfi",
        "-i", f"color=c=0x3C5AC8:s={size}x{size}", "-frames:v", "1", "-q:v", "1",
        "-pix_fmt", "yuvj444p", str(target),
    ]  # fmt: skip
    subprocess.run(command, check=True, capture_output=True, timeout=60)
    return target.read_bytes()
