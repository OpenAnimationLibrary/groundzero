"""Exact visible-RGBA repeat contract; original source bytes stay untouched."""
import hashlib

SEMANTICS = "transparent-rgb-zero-v1"


def canonical_hash(raw):
    if len(raw) % 4:
        raise ValueError("Incomplete BGRA pixel")
    canonical = bytearray(raw)
    for offset in range(0, len(raw), 4):
        if raw[offset + 3] == 0 and (raw[offset] or raw[offset + 1] or raw[offset + 2]):
            canonical[offset:offset + 3] = b"\0\0\0"
    return hashlib.sha256(canonical).hexdigest()


def differences(first, repeated):
    if len(first) != len(repeated) or len(first) % 4:
        raise ValueError("Repeat raster geometry differs")
    hidden = visible = 0
    for offset in range(0, len(first), 4):
        if first[offset:offset + 4] == repeated[offset:offset + 4]:
            continue
        if first[offset + 3] == repeated[offset + 3] == 0:
            hidden += 1
        else:
            visible += 1
    return {"hiddenRgbOnlyPixels": hidden, "visibleOrAlphaChangedPixels": visible}
