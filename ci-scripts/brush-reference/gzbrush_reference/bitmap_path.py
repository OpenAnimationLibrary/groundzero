"""Bounded fractional bitmap geometry through the public QPainterPath API."""
import math


def validate(case):
    points = [case["start"], case["end"]]
    if (case.get("bitmapPathProbe") is not True or
            not case.get("resourceProbe", {}).get("kind", "").startswith("filter-") or
            case.get("pressure") != [1, 1] or
            any(not isinstance(p, list) or len(p) != 2 for p in points) or
            any(type(v) not in (int, float) or not math.isfinite(v) or
                not 0 <= v <= 2047 for p in points for v in p) or
            points[1] != [points[0][0] + 1, points[0][1]]):
        raise RuntimeError("Invalid bounded bitmap path discriminator")
    return points
