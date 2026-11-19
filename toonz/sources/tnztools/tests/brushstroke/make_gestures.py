#!/usr/bin/env python3
"""Write the pointer gestures replayed by brushstroke_test.

Run from this directory: python3 make_gestures.py

The files in gestures/ are the recorded input of the test and are stored in
the repository; this script only documents how they were made. Each line is
"t_ms x y pressure" with three decimals, so every platform reads the same
doubles. Regenerating them changes the golden data.
"""

import math
from pathlib import Path

OUT = Path(__file__).resolve().parent / "gestures"


def write(name, description, samples):
    lines = ["# " + description, "# t_ms x y pressure"]
    for t_ms, x, y, pressure in samples:
        lines.append(f"{t_ms:.3f} {x:.3f} {y:.3f} {pressure:.3f}")
    (OUT / (name + ".txt")).write_text("\n".join(lines) + "\n")


def curve():
    samples = []
    count = 64
    for i in range(count):
        u = i / (count - 1)
        x = -40 + 80 * u
        y = 15 * math.sin(2 * math.pi * u) + 4 * math.sin(9 * u)
        pressure = 0.2 + 0.8 * math.sin(math.pi * u) ** 2
        samples.append((8 * i, x, y, pressure))
    return samples


def zigzag():
    corners = [(-45, -18), (45, -6), (-45, 6), (45, 18)]
    samples = [(0, corners[0][0], corners[0][1], 0.7)]
    t_ms = 0
    for start, end in zip(corners, corners[1:]):
        for step in range(1, 91):
            u = step / 90
            t_ms += 4
            samples.append((t_ms, start[0] + (end[0] - start[0]) * u,
                            start[1] + (end[1] - start[1]) * u, 0.7))
    return samples


def loop():
    samples = []
    count = 72
    for i in range(count + 1):
        angle = 2 * math.pi * i / count
        x = 26 * math.cos(angle) - 4
        y = 17 * math.sin(angle) + 2
        pressure = 0.55 + 0.3 * math.cos(3 * angle)
        samples.append((10 * i, x, y, pressure))
    # stop one unit short of the first point: snapping closes the rest
    samples[-1] = (10 * count, 21.2, 1.4, 0.6)
    return samples


def line():
    return [(0, -35, -12, 0.8), (40, -10, -3, 0.9), (80, 12, 6, 0.9),
            (120, 36, 14, 0.5)]


def dot():
    return [(0, 5, -3, 0.8), (30, 5, -3, 0.8)]


def speed():
    # Equal spacing in time, unequal in distance: slow, fast, slow.
    samples = []
    x = -42.0
    for i in range(70):
        u = i / 69
        step = 0.3 + 2.4 * math.sin(math.pi * u) ** 4
        x += step
        y = 10 * math.sin(3 * u) - 5
        samples.append((6 * i, x - 4, y, 0.6))
    return samples


def across():
    samples = []
    for i in range(41):
        u = i / 40
        samples.append((7 * i, -44 + 88 * u, 9 * math.sin(4 * u) - 2, 0.8))
    return samples


def main():
    OUT.mkdir(exist_ok=True)
    write("curve", "S curve with a pressure swell", curve())
    write("zigzag", "three long segments joined by sharp reversals", zigzag())
    write("loop", "ellipse that ends close to where it starts", loop())
    write("line", "four samples of a nearly straight drag", line())
    write("dot", "press and release without moving", dot())
    write("speed", "slow, fast, slow drag at a fixed sample rate", speed())
    write("across", "wavy drag across the whole fixture", across())


if __name__ == "__main__":
    main()
