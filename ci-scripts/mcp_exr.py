"""Decode the OpenEXR files the worker writes, with the standard library only.

The decoder is an independent reader for acceptance tests: it shares no code with OpenEXR or the
native writer. It reads single-part scanline files with HALF or FLOAT channels stored without
compression (NONE) or with ZIPS or ZIP compression, which is every form the render profiles can
write. Anything else (tiles, deep data, several parts, other compressions, UINT channels) is
refused with a ValueError, never guessed.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

MAGIC = 20000630
TILED_FLAG = 0x200
UNSUPPORTED_FLAGS = 0x200 | 0x800 | 0x1000
PIXEL_TYPES = {1: ("half", 2, "<e"), 2: ("float", 4, "<f")}
COMPRESSIONS = {0: ("none", 1), 2: ("zips", 1), 3: ("zip", 16)}
MAX_DIMENSION = 2048
MAX_HEADER_BYTES = 65536


@dataclass(frozen=True)
class Channel:
    """One channel of the file's channel list.

    Attributes:
        name: The channel name, such as R, G, B or A.
        sample: "half" or "float".
        linear: The channel list's pLinear flag (a hint for lossy compressors).
    """

    name: str
    sample: str
    linear: bool


@dataclass
class ExrImage:
    """A decoded scanline image.

    Attributes:
        width: Pixels per row.
        height: Rows.
        compression: "none", "zips" or "zip".
        channels: The channel list in file order (alphabetical, as OpenEXR stores it).
        attributes: Every header attribute name with its type name.
        samples: Per channel name, row-major values from the top row, as Python floats.
    """

    width: int
    height: int
    compression: str
    channels: list[Channel]
    attributes: dict[str, str]
    samples: dict[str, list[float]] = field(default_factory=dict)

    def pixel(self, x: int, y: int, names: str = "RGBA") -> tuple[float, ...]:
        """Return the named channels of one pixel."""
        index = y * self.width + x
        return tuple(self.samples[name][index] for name in names)


class _Reader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.position = 0

    def take(self, count: int) -> bytes:
        if count < 0 or self.position + count > len(self.data):
            raise ValueError("Truncated EXR file")
        chunk = self.data[self.position : self.position + count]
        self.position += count
        return chunk

    def unpack(self, form: str) -> tuple:
        return struct.unpack(form, self.take(struct.calcsize(form)))

    def text(self) -> str:
        end = self.data.find(b"\0", self.position, self.position + 256)
        if end < 0:
            raise ValueError("EXR name is unterminated or longer than 255 bytes")
        value = self.data[self.position : end].decode("ascii")
        self.position = end + 1
        return value


def _channel_list(value: bytes) -> list[Channel]:
    reader = _Reader(value)
    channels = []
    while True:
        name = reader.text()
        if not name:
            return channels
        pixel_type, linear, _reserved, x_sampling, y_sampling = reader.unpack("<iB3sii")
        if pixel_type not in PIXEL_TYPES or (x_sampling, y_sampling) != (1, 1):
            raise ValueError(f"Unsupported EXR channel {name}: type {pixel_type}")
        channels.append(Channel(name, PIXEL_TYPES[pixel_type][0], bool(linear)))


def _header(reader: _Reader) -> tuple[dict[str, str], dict[str, bytes]]:
    magic, version = reader.unpack("<ii")
    if magic != MAGIC:
        raise ValueError("Not an OpenEXR file")
    if version & 0xFF != 2 or version & UNSUPPORTED_FLAGS:
        raise ValueError(f"Unsupported EXR version field {version:#x}")
    types: dict[str, str] = {}
    values: dict[str, bytes] = {}
    while True:
        if reader.position > MAX_HEADER_BYTES:
            raise ValueError("EXR header exceeds the decoder's bound")
        name = reader.text()
        if not name:
            return types, values
        kind = reader.text()
        (size,) = reader.unpack("<i")
        if name in types:
            raise ValueError(f"Duplicate EXR attribute {name}")
        types[name] = kind
        values[name] = reader.take(size)


def _unzip(chunk: bytes, expected: int) -> bytes:
    if len(chunk) == expected:
        return chunk
    raw = bytearray(zlib.decompress(chunk))
    if len(raw) != expected:
        raise ValueError("EXR block inflates to the wrong size")
    for index in range(1, len(raw)):
        raw[index] = (raw[index - 1] + raw[index] - 128) & 0xFF
    half = (len(raw) + 1) // 2
    result = bytearray(len(raw))
    result[0::2] = raw[:half]
    result[1::2] = raw[half:]
    return bytes(result)


def _geometry(types: dict[str, str], values: dict[str, bytes]) -> tuple[int, int, str, int]:
    for name in ("channels", "compression", "dataWindow", "lineOrder"):
        if name not in types:
            raise ValueError(f"EXR header lacks {name}")
    x0, y0, x1, y1 = struct.unpack("<iiii", values["dataWindow"])
    width, height = x1 - x0 + 1, y1 - y0 + 1
    if not 0 < width <= MAX_DIMENSION or not 0 < height <= MAX_DIMENSION:
        raise ValueError("EXR data window is empty or exceeds the decoder's bound")
    compression = values["compression"][0]
    if compression not in COMPRESSIONS or values["lineOrder"][0] != 0:
        raise ValueError(f"Unsupported EXR compression {compression} or line order")
    name, lines = COMPRESSIONS[compression]
    return width, height, name, lines


def _store_block(image: ExrImage, block: bytes, first_row: int, rows: int) -> None:
    position = 0
    for row in range(first_row, first_row + rows):
        for channel in image.channels:
            _, size, form = PIXEL_TYPES[1 if channel.sample == "half" else 2]
            span = size * image.width
            values = struct.unpack(f"<{image.width}{form[1]}", block[position : position + span])
            start = row * image.width
            image.samples[channel.name][start : start + image.width] = list(values)
            position += span


def decode_exr(data: bytes) -> ExrImage:
    """Decode a single-part scanline OpenEXR file.

    Args:
        data: The whole file.
    Returns:
        The image with every channel's samples.
    Raises:
        ValueError: For any form this decoder does not read, or a malformed file.
    """
    reader = _Reader(data)
    types, values = _header(reader)
    width, height, compression, lines = _geometry(types, values)
    channels = _channel_list(values["channels"])
    image = ExrImage(width, height, compression, channels, types)
    for channel in channels:
        image.samples[channel.name] = [0.0] * (width * height)
    row_bytes = width * sum(PIXEL_TYPES[1 if c.sample == "half" else 2][1] for c in channels)
    blocks = (height + lines - 1) // lines
    offsets = reader.unpack(f"<{blocks}Q")
    y0 = struct.unpack_from("<i", values["dataWindow"], 4)[0]
    for number, offset in enumerate(offsets):
        block_reader = _Reader(data)
        block_reader.position = offset
        y, size = block_reader.unpack("<ii")
        first = y - y0
        if first != number * lines:
            raise ValueError("EXR block offsets are out of order")
        rows = min(lines, height - first)
        chunk = block_reader.take(size)
        expected = rows * row_bytes
        block = chunk if compression == "none" else _unzip(chunk, expected)
        if len(block) != expected:
            raise ValueError("EXR block has the wrong size")
        _store_block(image, block, first, rows)
    return image


def decode_exr_file(path: Path) -> ExrImage:
    """Decode an OpenEXR file from disk."""
    return decode_exr(path.read_bytes())
