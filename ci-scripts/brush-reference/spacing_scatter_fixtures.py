"""Original auto-spacing/scatter acquisitions; no foreign rendering equations.

K5341 serializer metadata, not renderer implementation:
https://github.com/KDE/krita/blob/bd6e8d0/libs/brush/kis_auto_brush_factory.cpp
https://github.com/KDE/krita/blob/bd6e8d0/plugins/paintops/libpaintop/KisScatterOptionData.cpp
"""
import json
from xml.etree.ElementTree import Element, SubElement, tostring

SCATTER_GROUPS = tuple("scatter-" + axis + "-" + direction
                       for axis in ("x", "y", "xy") for direction in ("east", "north"))
GROUPS = ("autospacing",) + SCATTER_GROUPS
REPLICATES = 100


def brush(diameter, spacing, *, automatic=False, coefficient=1, ratio=1):
    definition = Element("Brush", type="auto_brush", BrushVersion="2",
                         spacing=str(spacing), angle="0", randomness="0", density="1",
                         useAutoSpacing="1" if automatic else "0",
                         autoSpacingCoeff=str(coefficient))
    SubElement(definition, "MaskGenerator", id="default", type="circle",
               diameter=str(diameter), ratio=str(ratio), hfade="1", vfade="1",
               antialiasEdges="0", spikes="2")
    return tostring(definition, encoding="unicode")


def common():
    # Explicitly disable unrelated dynamics. No inferred defaults establish a
    # measured equivalence class, even when the source defaults happen to match.
    return {"PressureSize": "false", "PressureRatio": "false",
            "PressureRotation": "false", "PressureSpacing": "false",
            "PressureScatter": "false", "PressureMirror": "false",
            "PressureOpacity": "false", "PressureFlow": "false",
            "Pressureh": "false", "Pressures": "false", "Pressurev": "false",
            "OpacityUseCurve": "false", "FlowUseCurve": "false",
            "Texture/Pattern/Enabled": "false", "PaintOpAction": "1",
            "Spacing/Isotropic": "false"}


def auto_cases():
    for diameter in (1, 8, 64, 256):
        for ratio in (1, .5):
            for coefficient in (.1, .5, 1, 2):
                for direction in ("east", "north", "diagonal"):
                    start, end = {
                        "east": ([896, 1024], [1152, 1024]),
                        "north": ([1024, 1152], [1024, 896]),
                        "diagonal": ([896, 896], [1152, 1152]),
                    }[direction]
                    for enabled in (False, True):
                        fields = common()
                        fields["brush_definition"] = brush(diameter, 1, automatic=enabled,
                                                            coefficient=coefficient, ratio=ratio)
                        yield {"diameter": diameter, "spacing": 1, "opacity": .5,
                               "flow": 1, "pressure": [1, 1], "start": start, "end": end,
                               "background": "transparent", "sourceEngine": "paintbrush",
                               "delivery": "Node.paintLine", "overrides": fields,
                               "expectUnchanged": False,
                               "autoSpacingProbe": {"enabled": enabled, "coefficient": coefficient,
                                   "ratio": ratio, "direction": direction,
                                   "rect": [704, 704, 640, 640],
                                   "qualification": "original acquisition; automatic spacing equation unverified"}}


def scatter_cases(group):
    if group not in SCATTER_GROUPS:
        raise ValueError("Unknown bounded scatter group")
    _, axes, direction = group.split("-")
    start, end, rect = (([256, 1024], [1249, 1024], [128, 896, 1280, 256])
                        if direction == "east" else
                        ([1024, 1248], [1024, 255], [896, 128, 256, 1280]))
    # The enabled configuration earns statistical evidence from 100 independent
    # strokes. Two disabled controls independently qualify deterministic replay.
    # They are not counted as RNG samples or silently substituted for them.
    for enabled, count in ((True, REPLICATES), (False, 2)):
        for replicate in range(count):
            fields = common()
            fields.update({"brush_definition": brush(8, 4),
                           "PressureScatter": str(enabled).lower(), "ScatterValue": "1",
                           "ScatterUseCurve": "false", "ScatterUseSameCurve": "true",
                           "ScattercurveMode": "0", "ScattercommonCurve": "0,1;1,1;",
                           "ScatterSensor": '<params id="pressure"><curve>0,1;1,1;</curve></params>',
                           "Scattering/AxisX": str("x" in axes).lower(),
                           "Scattering/AxisY": str("y" in axes).lower()})
            yield {"diameter": 8, "spacing": 4, "opacity": 1, "flow": 1,
                   "pressure": [1, 1], "start": start, "end": end,
                   "background": "transparent", "sourceEngine": "paintbrush",
                   "delivery": "Node.paintLine", "overrides": fields,
                   "expectUnchanged": False,
                   "scatterProbe": {"axes": axes, "direction": direction, "amount": 1,
                       "enabled": enabled, "replicate": replicate, "replicates": count,
                       "nominalSlots": 32, "rect": rect, "bands": 3,
                       "qualification": "independent source RNG acquisition; seed/pixel parity not asserted"}}


def prepare(root, resources, preset_writer, group):
    if group not in GROUPS:
        raise ValueError("Unknown spacing/scatter acquisition group")
    cases, names = [], []
    plans = auto_cases() if group == "autospacing" else scatter_cases(group)
    for index, case in enumerate(plans):
        name = "GZ Original dry " + group + " " + str(index).zfill(3)
        case.update(preset=name, variant="baseline", foregroundRGBA8=[173, 91, 47, 255])
        (resources / "paintoppresets" / (name + ".kpp")).write_bytes(
            preset_writer(name, "dry", False, case["overrides"], case["diameter"],
                          case["spacing"], 0))
        cases.append(case)
        names.append(name)
    (root / "output" / "fixtures.json").write_text(json.dumps({
        "version": "5.3.4.1", "group": group, "presets": names, "cases": cases,
        "license": "CC0-1.0", "width": 2048, "height": 2048,
        "randomizeExecution": group in SCATTER_GROUPS,
        "stochasticSamplePolicy": "100 independent enabled strokes; no random exact-repeat requirement"
            if group in SCATTER_GROUPS else "deterministic independent repeat required",
        "input": "public Node.paintLine; effective controls and delivered endpoints required",
    }, indent=2))
