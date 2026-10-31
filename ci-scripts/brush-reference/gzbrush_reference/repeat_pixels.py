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


def capture_checkpoint(read, wait, expected_bytes, phase, reset=None):
    """Two fixed observations across a Qt turn, never a convergence retry.

    Return the first capture only. A changed visible result fails instead of
    selecting whichever observation happens to match another stroke.
    """
    import json
    wait()
    first = bytes(read())
    if len(first) != expected_bytes:
        raise RuntimeError("Incomplete source readback at " + phase)
    if reset is not None and first != reset:
        raise RuntimeError("Source reset readback differs at " + phase)
    yield 20
    wait()
    second = bytes(read())
    if len(second) != expected_bytes:
        raise RuntimeError("Incomplete settled source readback at " + phase)
    delta = ({"hiddenRgbOnlyPixels": 0, "visibleOrAlphaChangedPixels": 0}
             if first == second else differences(first, second))
    evidence = {"phase": phase, "qtTurnDelayMs": 20,
                "firstSha256Pixels": hashlib.sha256(first).hexdigest(),
                "settledSha256Pixels": hashlib.sha256(second).hexdigest(),
                **delta}
    if (reset is not None and second != reset) or delta["visibleOrAlphaChangedPixels"]:
        raise RuntimeError("Source readback changed across fixed Qt checkpoint: " +
                           json.dumps(evidence, separators=(",", ":")))
    return first, evidence
