"""Validate exact authored fractional source paths; no generator equation."""
import math


def validate(case):
    spec = case.get("generatedCurveDiscriminator", {})
    origin = spec.get("originDelta")
    shifts = ((-.125, 0), (.125, 0), (0, -.125), (0, .125), (.5, 0), (0, .5))
    if (case.get("generatedCurvePath") is not True or
            case.get("delivery") != "Node.paintPath" or
            type(origin) is not list or len(origin) != 2 or
            any(type(v) not in (int, float) or not math.isfinite(v) for v in origin) or
            tuple(origin) not in shifts or spec.get("generator") != "soft" or
            spec.get("shape") not in ("circle", "rect") or
            spec.get("diameter") not in (16, 64) or
            spec.get("curve") not in ("0,1;1,1;", "0,0;1,1;", "0,1;1,0;")):
        raise ValueError("Generated curve path changed its bounded source contract")
    expected_mask = {"id": "soft", "type": spec["shape"],
                     "diameter": str(spec["diameter"]), "ratio": "1",
                     "hfade": "0.5", "vfade": "0.5", "antialiasEdges": "1",
                     "spikes": "2", "softness_curve": spec["curve"]}
    if case.get("generatedTipProbe") != expected_mask:
        raise ValueError("Generated curve path label differs from captured mask controls")
    points = [[1024 + origin[0], 1024 + origin[1]],
              [1025 + origin[0], 1024 + origin[1]]]
    if (case.get("sourcePath") != points or case.get("start") != points[0] or
            case.get("end") != points[-1] or case.get("diameter") != spec["diameter"] or
            case.get("spacing") != 1 or case.get("pressure") != [1, 1] or
            case.get("opacity") != 1 or case.get("flow") != 1 or
            case.get("background") != "transparent" or case.get("sourceEngine") != "paintbrush"):
        raise ValueError("Generated curve delivery/geometry differs from authored controls")
    return points
