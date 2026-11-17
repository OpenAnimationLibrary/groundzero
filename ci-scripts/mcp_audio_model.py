"""Write audio fixtures and model the native offline mix independently of the worker."""

from __future__ import annotations

import hashlib
import io
import math
import struct
import wave
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field

Frame = tuple[float, ...]
WIDTHS = {"pcm_u8": 1, "pcm_s8": 1, "pcm_s16": 2, "pcm_s24": 3, "pcm_f32": 4}
RANGES = {
    "pcm_u8": (0, 255),
    "pcm_s8": (-128, 127),
    "pcm_s16": (-32768, 32767),
    "pcm_s24": (-8388608, 8388607),
}


@dataclass(frozen=True)
class Source:
    """Decoded samples of one fixture, one tuple of channel values per sample frame."""

    rate: int
    codec: str
    frames: tuple[Frame, ...]

    @property
    def channels(self) -> int:
        return len(self.frames[0])

    def frame_count(self, fps: float) -> int:
        """Return the native frame count: trunc(duration * fps), at least one."""
        return max(1, int(len(self.frames) / self.rate * fps))


def tone(rate: int, codec: str, count: int, channels: int = 1, step: int = 37) -> Source:
    """Build a deterministic ramp with distinct values per sample and channel."""
    low, high = RANGES.get(codec, (-1, 1))
    frames = []
    for index in range(count):
        frame = []
        for channel in range(channels):
            if codec == "pcm_f32":
                frame.append(float(_f32(((index * step + channel * 101) % 2000 - 1000) / 1250)))
            else:
                span = min(high - low, 60000)
                frame.append(low + (index * step + channel * 101) % span + (high - low - span) // 2)
        frames.append(tuple(frame))
    return Source(rate, codec, tuple(frames))


def constant(rate: int, codec: str, count: int, value: float, channels: int = 1) -> Source:
    """Build a constant (DC) source."""
    return Source(rate, codec, tuple((value,) * channels for _ in range(count)))


def _f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _sample_bytes(codec: str, value: float, big_endian: bool = False) -> bytes:
    order = ">" if big_endian else "<"
    if codec == "pcm_f32":
        return struct.pack(order + "f", value)
    if codec == "pcm_s24":
        data = int(value).to_bytes(3, "little", signed=True)
        return data[::-1] if big_endian else data
    form = {"pcm_u8": "B", "pcm_s8": "b", "pcm_s16": "h"}[codec]
    return struct.pack(order + form, int(value))


def pcm(source: Source, big_endian: bool = False) -> bytes:
    """Interleave samples in the requested byte order."""
    return b"".join(
        _sample_bytes(source.codec, value, big_endian) for frame in source.frames for value in frame
    )


def pcm_sha256(source: Source) -> str:
    return hashlib.sha256(pcm(source)).hexdigest()


def wav_file(source: Source) -> bytes:
    """Write integer PCM with the wave module and IEEE float with struct."""
    if source.codec == "pcm_f32":
        data = pcm(source)
        fmt = struct.pack(
            "<HHIIHH",
            3,
            source.channels,
            source.rate,
            source.rate * 4 * source.channels,
            4 * source.channels,
            32,
        )
        body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"data"
        body += struct.pack("<I", len(data)) + data
        return b"RIFF" + struct.pack("<I", len(body)) + body
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as output:
        output.setnchannels(source.channels)
        output.setsampwidth(WIDTHS[source.codec])
        output.setframerate(source.rate)
        output.writeframes(pcm(source))
    return buffer.getvalue()


def _extended(rate: int) -> bytes:
    exponent = rate.bit_length() - 1
    return struct.pack(">HQ", 16383 + exponent, rate << (63 - exponent))


def aiff_file(source: Source, form: bytes = b"AIFF") -> bytes:
    """Write a big-endian AIFF fixture (8-bit samples are signed)."""
    data = pcm(source, big_endian=True)
    bits = WIDTHS[source.codec] * 8
    comm = struct.pack(">hIh", source.channels, len(source.frames), bits) + _extended(source.rate)
    body = form + b"COMM" + struct.pack(">I", 18) + comm
    body += b"SSND" + struct.pack(">III", 8 + len(data), 0, 0) + data
    if len(body) % 2:
        body += b"\0"
    return b"FORM" + struct.pack(">I", len(body)) + body


@dataclass
class Clip:
    source: Source
    timeline: int
    start: int
    count: int


@dataclass
class Column:
    clips: list[Clip] = field(default_factory=list)
    volume_permille: int = 1000
    audible: bool = True

    def natively_empty(self) -> bool:
        """Native sound columns are 'empty' when their clips span at most one frame."""
        if not self.clips:
            return True
        first = min(clip.timeline for clip in self.clips)
        last = max(clip.timeline + clip.count - 1 for clip in self.clips)
        return first == last


def mix_sample_count(start: int, end_exclusive: int, fps: float, rate: int) -> int:
    """Count samples as the native track builder: ceil with a 1e-8 tolerance."""
    exact = (end_exclusive - start) / fps * rate
    count = int(exact)
    return count + 1 if count < exact - 1e-8 else count


def _convert(source: Source, codec: str, channels: int, rate: int) -> list[Frame]:
    if source.rate != rate or source.codec != codec:
        raise ValueError("The model converts only the channel count of mixed sources")
    if source.channels == channels:
        return list(source.frames)
    if source.channels == 1 and channels == 2:
        return [(frame[0], frame[0]) for frame in source.frames]
    raise ValueError("The model does not average stereo sources into mono")


def _column_track(column: Column, mix: MixFormat, start: int, to: int, count: int) -> list[Frame]:
    silence = (127.0 if mix.codec == "pcm_u8" else 0.0,) * mix.channels
    track = [silence] * count
    per_frame = mix.rate / mix.fps
    for clip in sorted(column.clips, key=lambda item: item.timeline):
        begin, end = clip.timeline, clip.timeline + clip.count
        if begin < start and end < start:
            continue
        if begin > to and end > to:
            break
        first = int((clip.start + max(0, start - begin)) * per_frame)
        last = int((clip.start + clip.count - max(0, end - to)) * per_frame)
        if last <= 0 or last < first:
            continue
        samples = _convert(clip.source, mix.codec, mix.channels, mix.rate)
        top = len(samples) - 1
        target = min(max(int((begin - start) * per_frame), 0), count - 1)
        for index in range(min(max(first, 0), top), min(max(last, 0), top) + 1):
            if target >= count:
                break
            track[target] = samples[index]
            target += 1
    return track


def _variants(previous: float, value: float, volume: float) -> set[float]:
    return {
        previous * 1.0 + value * volume,
        math.fma(value, volume, previous * 1.0),
        math.fma(previous, 1.0, value * volume),
    }


def _single(results: set[float]) -> float:
    if len(results) != 1:
        raise ValueError("Fixture sample is ambiguous under floating-point contraction")
    return results.pop()


def _combine(codec: str, previous: float, value: float, volume: float) -> float:
    if codec == "pcm_f32":
        return _single({_f32(item) for item in _variants(previous, value, volume)})
    if codec == "pcm_u8":
        results = {int(item + 128) for item in _variants(previous - 128, value - 128, volume)}
    else:
        results = {int(item) for item in _variants(previous, value, volume)}
    low, high = RANGES[codec]
    return float(min(max(_single(set(results)), low), high))


@dataclass(frozen=True)
class MixFormat:
    rate: int
    codec: str
    channels: int
    fps: float


@dataclass(frozen=True)
class Mix:
    rate: int
    codec: str
    channels: int
    frames: tuple[Frame, ...]
    included: tuple[int, ...]
    excluded: tuple[tuple[int, str], ...]

    def pcm(self) -> bytes:
        return pcm(Source(self.rate, self.codec, self.frames))

    def sha256(self) -> str:
        return hashlib.sha256(self.pcm()).hexdigest()

    def wav(self) -> bytes:
        """Return the bytes the native WAV writer stores, RIFF size four bytes short."""
        data = self.pcm()
        width = WIDTHS[self.codec]
        tag = 3 if self.codec == "pcm_f32" else 1
        fmt = struct.pack(
            "<HHIIHH",
            tag,
            self.channels,
            self.rate,
            self.rate * width * self.channels,
            width * self.channels,
            width * 8,
        )
        header = b"RIFF" + struct.pack("<I", 32 + len(data)) + b"WAVE"
        return (
            header
            + b"fmt "
            + struct.pack("<I", 16)
            + fmt
            + b"data"
            + struct.pack("<I", len(data))
            + data
        )

    def peak(self) -> float:
        full = 1.0 if self.codec == "pcm_f32" else 1 << (WIDTHS[self.codec] * 8 - 1)
        return max(abs(value) / full for frame in self.frames for value in frame)

    def full_scale_count(self) -> int:
        if self.codec == "pcm_f32":
            return sum(abs(value) >= 1 for frame in self.frames for value in frame)
        low, high = RANGES[self.codec]
        return sum(value in (low, high) for frame in self.frames for value in frame)


def _plan(columns: Sequence[Column]) -> tuple[list[int], list[int], list[tuple[int, str]]]:
    native: list[int] = []
    included: list[int] = []
    excluded: list[tuple[int, str]] = []
    for index, column in enumerate(columns):
        reason = ""
        if column.natively_empty():
            reason = "empty"
        elif not column.audible:
            reason = "muted"
        elif native and column.volume_permille == 0:
            reason = "zero_volume"
        if reason in ("", "zero_volume"):
            native.append(index)
        if reason:
            excluded.append((index, reason))
        else:
            included.append(index)
    return native, included, excluded


def _promote(codec: str) -> Callable[[float], float]:
    if codec == "pcm_u8":
        return lambda value: (value - 128) * 256
    if codec == "pcm_s8":
        return lambda value: value * 256
    return lambda value: value


def offline_mix(columns: Sequence[Column], start: int, end: int, fps: float) -> Mix:
    """Model TXshSoundColumn::mixingTogetherOffline over frames [start, end]."""
    native, included, excluded = _plan(columns)
    if not native:
        raise ValueError("No audible column")
    first = min(columns[native[0]].clips, key=lambda clip: clip.timeline).source
    mix = MixFormat(first.rate, first.codec, first.channels, fps)
    count = mix_sample_count(start, end + 1, fps, mix.rate)
    initial = 127.0 if mix.codec == "pcm_u8" else 0.0
    result = [(initial,) * mix.channels] * count
    for index in native:
        if index in dict(excluded):
            continue
        volume = columns[index].volume_permille / 1000
        track = _column_track(columns[index], mix, start, end + 1, count)
        result = [
            tuple(_combine(mix.codec, a, b, volume) for a, b in zip(old, new, strict=True))
            for old, new in zip(result, track, strict=True)
        ]
    promote = _promote(mix.codec)
    codec = "pcm_s16" if WIDTHS[mix.codec] == 1 else mix.codec
    frames = tuple(tuple(promote(value) for value in frame) for frame in result)
    return Mix(mix.rate, codec, mix.channels, frames, tuple(included), tuple(excluded))
