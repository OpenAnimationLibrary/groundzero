"""Exact visible-RGBA repeat contract; original source bytes stay untouched."""
import hashlib

SEMANTICS = "transparent-rgb-zero-v1"


def canonical_hash(raw):
    if len(raw) % 4:
        raise ValueError("Incomplete BGRA pixel")
    canonical = bytearray(raw)
    # Each byte lane is independent: zero RGB only where the corresponding
    # alpha byte is zero. Integer AND executes this exact byte mask in C,
    # avoiding a Python iteration for every pixel of a full source canvas.
    # Explicit little-endian conversion makes this independent of host order.
    pixels = len(raw) // 4
    alpha_mask = raw[3::4].translate(bytes([0]) + bytes([255]) * 255)
    mask = int.from_bytes(alpha_mask, "little")
    for channel in range(3):
        values = int.from_bytes(raw[channel::4], "little")
        canonical[channel::4] = (values & mask).to_bytes(pixels, "little")
    return hashlib.sha256(canonical).hexdigest()


def differences(first, repeated):
    if len(first) != len(repeated) or len(first) % 4:
        raise ValueError("Repeat raster geometry differs")
    if first == repeated:
        return {"hiddenRgbOnlyPixels": 0, "visibleOrAlphaChangedPixels": 0}
    hidden = visible = 0
    nonzero = bytes([0]) + bytes([1]) * 255
    # Bound temporary byte lanes/integers to 64K pixels (~256 KiB input).
    # Every flag occupies a separate byte, so OR/AND cannot cross lanes.
    for offset in range(0, len(first), 4 * 65536):
        a, b = first[offset:offset + 4 * 65536], repeated[offset:offset + 4 * 65536]
        if a == b:
            continue
        count = len(a) // 4
        changed = 0
        for channel in range(4):
            xor = int.from_bytes(a[channel::4], "little") ^ int.from_bytes(b[channel::4], "little")
            changed |= int.from_bytes(xor.to_bytes(count, "little").translate(nonzero), "little")
        alpha_present = (int.from_bytes(a[3::4].translate(nonzero), "little") |
                         int.from_bytes(b[3::4].translate(nonzero), "little"))
        visible_count = (changed & alpha_present).to_bytes(count, "little").count(1)
        visible += visible_count
        hidden += changed.to_bytes(count, "little").count(1) - visible_count
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
