"""Create deterministic raster fixtures and independently inspect native PNG output."""

from __future__ import annotations

import struct
import zlib
from collections.abc import Callable, Iterator
from pathlib import Path

SIGNATURE = b"\x89PNG\r\n\x1a\n"
Pixel = tuple[int, int, int, int]


def write_fixture(path: Path, *, size: int = 32, depth: int = 8) -> None:
    """Write four opaque quadrants without an image-library dependency."""

    def chunk(kind: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(kind + data)
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", crc)

    pixels = bytearray()
    for y in range(size):
        pixels.append(0)
        for x in range(size):
            for component in fixture_pixel(x, y, size):
                pixels.extend(bytes([component]) * (depth // 8))
    header = struct.pack(">IIBBBBB", size, size, depth, 6, 0, 0, 0)
    path.write_bytes(
        SIGNATURE
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(pixels))
        + chunk(b"IEND", b"")
    )


def fixture_pixel(x: int, y: int, size: int) -> Pixel:
    """Return the color at one coordinate in the fixture's four quadrants."""
    colors: tuple[Pixel, ...] = (
        (255, 0, 0, 255),
        (0, 255, 0, 255),
        (0, 0, 255, 255),
        (255, 255, 0, 255),
    )
    return colors[int(x >= size // 2) + 2 * int(y >= size // 2)]


def _paeth(left: int, above: int, diagonal: int) -> int:
    prediction = left + above - diagonal
    distances = (abs(prediction - left), abs(prediction - above), abs(prediction - diagonal))
    return (left, above, diagonal)[distances.index(min(distances))]


def _unfilter(data: bytes, width: int, height: int, channels: int) -> bytes:
    stride = width * channels
    if len(data) != height * (stride + 1):
        raise ValueError("PNG decoded byte count does not match its dimensions")
    previous = bytes(stride)
    pixels = bytearray()
    predictors: tuple[Callable[[int, int, int], int], ...] = (
        lambda _left, _above, _diagonal: 0,
        lambda left, _above, _diagonal: left,
        lambda _left, above, _diagonal: above,
        lambda left, above, _diagonal: (left + above) // 2,
        _paeth,
    )
    for y in range(height):
        start = y * (stride + 1)
        filter_type = data[start]
        if filter_type >= len(predictors):
            raise ValueError(f"Invalid PNG filter {filter_type}")
        row = bytearray(data[start + 1 : start + stride + 1])
        for index in range(stride):
            left = row[index - channels] if index >= channels else 0
            diagonal = previous[index - channels] if index >= channels else 0
            row[index] = (
                row[index] + predictors[filter_type](left, previous[index], diagonal)
            ) & 255
        pixels.extend(row)
        previous = bytes(row)
    return bytes(pixels)


def _chunks(data: bytes) -> Iterator[tuple[bytes, bytes]]:
    if not data.startswith(SIGNATURE):
        raise ValueError("Artifact does not have a PNG signature")
    position = len(SIGNATURE)
    while position + 12 <= len(data):
        length = struct.unpack_from(">I", data, position)[0]
        kind = data[position + 4 : position + 8]
        content = data[position + 8 : position + 8 + length]
        if len(content) != length or position + length + 12 > len(data):
            raise ValueError("Truncated PNG chunk")
        crc = struct.unpack_from(">I", data, position + length + 8)[0]
        if zlib.crc32(kind + content) != crc:
            raise ValueError("PNG chunk checksum mismatch")
        yield kind, content
        position += length + 12


def _dimensions(header: bytes) -> tuple[int, int, int]:
    width, height, depth, color, compression, filtering, interlace = struct.unpack(
        ">IIBBBBB", header
    )
    if (depth, color) not in ((8, 2), (8, 6)) or (compression, filtering, interlace) != (0, 0, 0):
        raise ValueError("Acceptance PNG must be non-interlaced eight-bit RGB or RGBA")
    if not 0 < width <= 2048 or not 0 < height <= 2048:
        raise ValueError("PNG dimensions exceed the acceptance-test bound")
    return width, height, 4 if color == 6 else 3


def decode_png(data: bytes) -> tuple[int, int, list[Pixel]]:
    """Validate PNG chunk checksums and decode non-interlaced eight-bit RGB(A)."""
    width, height, channels = 0, 0, 0
    compressed = bytearray()
    ended = False
    for kind, content in _chunks(data):
        if kind == b"IHDR":
            width, height, channels = _dimensions(content)
        elif kind == b"IDAT":
            compressed.extend(content)
        elif kind == b"IEND":
            ended = True
            break
    if not ended or not width or not compressed:
        raise ValueError("PNG is missing its header, image data, or end marker")
    pixels = _unfilter(zlib.decompress(compressed), width, height, channels)
    rgba = []
    for index in range(0, len(pixels), channels):
        rgba.append(
            (
                pixels[index],
                pixels[index + 1],
                pixels[index + 2],
                pixels[index + 3] if channels == 4 else 255,
            )
        )
    return width, height, rgba
