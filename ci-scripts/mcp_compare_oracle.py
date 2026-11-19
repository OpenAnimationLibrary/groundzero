"""Recompute every image-review number from PNG bytes with the standard library only.

`review.compare.submit` (contract w3-compare §5) promises integer sums, one binary64 division
for each mean and one binary64 square root for each RMSE, nearest-rank percentiles, a fixed grid
and 4-connected components. Python's float division and `math.sqrt` are correctly rounded, so
this oracle must agree with the worker exactly, field by field.
"""

from __future__ import annotations

import hashlib
import math
import struct
from dataclasses import dataclass

from mcp_png import Pixel

CHANNELS = ("r", "g", "b", "a")


@dataclass(frozen=True)
class Request:
    """What one pair is compared under.

    Attributes:
        region: (x, y, width, height) in image pixels from the top-left corner.
        premultiplied: Compare RGB premultiplied by alpha (round half up) instead of stored values.
        alpha: Whether alpha is a compared channel.
        threshold: A pixel is changed when its largest channel difference exceeds this.
        grid: (columns, rows) before clamping to the region.
        components: How many components to list.
    """

    region: tuple[int, int, int, int]
    premultiplied: bool = True
    alpha: bool = True
    threshold: int = 3
    grid: tuple[int, int] = (8, 8)
    components: int = 16


def pixel_sha256(pixels: list[Pixel], width: int, height: int) -> str:
    """SHA-256 of the framed straight RGBA bytes, rows from the top."""
    digest = hashlib.sha256(b"GZRGBA8\n" + struct.pack(">II", width, height))
    digest.update(bytes(value for pixel in pixels for value in pixel))
    return digest.hexdigest()


def _value(pixel: Pixel, channel: int, premultiplied: bool) -> int:
    if not premultiplied or channel == 3:
        return pixel[channel]
    return (pixel[channel] * pixel[3] + 127) // 255


def _cells(size: int, count: int) -> list[int]:
    index = [0] * size
    for cell in range(count):
        for offset in range(cell * size // count, (cell + 1) * size // count):
            index[offset] = cell
    return index


def _differences(
    left: list[Pixel], right: list[Pixel], width: int, request: Request
) -> tuple[list[int], list[list[int]]]:
    x0, y0, region_width, region_height = request.region
    considered = 4 if request.alpha else 3
    largest = []
    per_channel: list[list[int]] = [[] for _ in range(considered)]
    for y in range(y0, y0 + region_height):
        for x in range(x0, x0 + region_width):
            a, b = left[y * width + x], right[y * width + x]
            values = [
                abs(_value(a, c, request.premultiplied) - _value(b, c, request.premultiplied))
                for c in range(considered)
            ]
            for channel, value in enumerate(values):
                per_channel[channel].append(value)
            largest.append(max(values))
    return largest, per_channel


def _percentiles(largest: list[int]) -> dict[str, int]:
    ordered = sorted(largest)
    count = len(ordered)
    return {f"p{p}": ordered[(p * count + 99) // 100 - 1] for p in (50, 95, 99, 100)}


def _components(largest: list[int], request: Request) -> dict[str, object]:
    x0, y0, width, height = request.region
    seen = [False] * len(largest)
    found = []
    for start, value in enumerate(largest):
        if value <= request.threshold or seen[start]:
            continue
        seen[start] = True
        stack, members = [start], []
        while stack:
            at = stack.pop()
            members.append(at)
            x, y = at % width, at // width
            for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                near = ny * width + nx
                inside = 0 <= nx < width and 0 <= ny < height
                if inside and not seen[near] and largest[near] > request.threshold:
                    seen[near] = True
                    stack.append(near)
        xs = [at % width + x0 for at in members]
        ys = [at // width + y0 for at in members]
        found.append(
            {
                "area": len(members),
                "box": _box(min(xs), min(ys), max(xs), max(ys)),
                "max": max(largest[at] for at in members),
                "sum": sum(largest[at] for at in members),
            }
        )
    found.sort(key=lambda row: (-row["area"], row["box"]["y"], row["box"]["x"]))
    return {"total": len(found), "items": found[: request.components]}


def _box(x0: int, y0: int, x1: int, y1: int) -> dict[str, int]:
    return {"x": x0, "y": y0, "width": x1 - x0 + 1, "height": y1 - y0 + 1}


def _grid(largest: list[int], request: Request) -> dict[str, object]:
    _x0, _y0, width, height = request.region
    columns, rows = min(request.grid[0], width), min(request.grid[1], height)
    cell_x, cell_y = _cells(width, columns), _cells(height, rows)
    cells = [[0, 0, 0] for _ in range(columns * rows)]
    for at, value in enumerate(largest):
        cell = cells[cell_y[at // width] * columns + cell_x[at % width]]
        cell[0] += value > request.threshold
        cell[1] = max(cell[1], value)
        cell[2] += value
    return {"columns": columns, "rows": rows, "cells": cells}


def _channel(values: list[int], count: int) -> dict[str, float | int]:
    squares = sum(value * value for value in values)
    return {
        "max_abs": max(values),
        "sum_abs": sum(values),
        "sum_sq": squares,
        "mean_abs": sum(values) / count,
        "rmse": math.sqrt(squares / count),
    }


def _changed(largest: list[int], request: Request) -> tuple[int, dict[str, int] | None]:
    x0, y0, width, _height = request.region
    changed = [at for at, value in enumerate(largest) if value > request.threshold]
    if not changed:
        return 0, None
    xs = [at % width + x0 for at in changed]
    ys = [at // width + y0 for at in changed]
    return len(changed), _box(min(xs), min(ys), max(xs), max(ys))


def compare(left: list[Pixel], right: list[Pixel], width: int, request: Request) -> dict:
    """The metrics and regions the worker reports for one pair of equal-size images."""
    largest, per_channel = _differences(left, right, width, request)
    count = len(largest)
    channels: dict[str, object] = {name: None for name in CHANNELS}
    for index, values in enumerate(per_channel):
        channels[CHANNELS[index]] = _channel(values, count)
    squares = sum(value * value for values in per_channel for value in values)
    histogram = [0] * 256
    for value in largest:
        histogram[value] += 1
    changed, bbox = _changed(largest, request)
    metrics = {
        "identical_straight": left == right,
        "pixels": count,
        "channels": channels,
        "rmse_all": math.sqrt(squares / (count * len(per_channel))),
        "histogram": histogram,
        "percentiles": _percentiles(largest),
        "changed_pixels": changed,
        "changed_ppm": changed * 1_000_000 // count,
        "changed_bbox": bbox,
    }
    regions = {"grid": _grid(largest, request), "components": _components(largest, request)}
    return {"metrics": metrics, "regions": regions, "difference": largest}


def difference_pixels(largest: list[int], threshold: int) -> list[Pixel]:
    """The difference image: gray D where D exceeds the threshold, opaque."""
    return [(d, d, d, 255) if d > threshold else (0, 0, 0, 255) for d in largest]
