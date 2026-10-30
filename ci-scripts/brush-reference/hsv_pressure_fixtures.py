"""Original H/V pressure heldouts; no assumed source color composition."""
import json
from option_fixtures import prepare as prepare_options
from sensor_fixtures import trace


def prepare(root, resources, preset_writer):
    plans, pressures = [], []
    for target in ("h", "v"):
        for color in ([211, 37, 149, 255], [9, 173, 231, 255]):
            for pressure in ([.125, .875], [.875, .125]):
                plans.append((target, "pressure", {}, "0,0;1,1;", trace("east"), color))
                pressures.extend([pressure, pressure])
    prepare_options(root, resources, preset_writer, "hsvpressure", plans)
    path=root / "output" / "fixtures.json"
    fixtures=json.loads(path.read_text())
    for case, pressure in zip(fixtures["cases"], pressures):
        case["delivery"]="Node.paintLine"
        case.pop("events")
        case["pressure"]=pressure
        case["sensorProbe"]["captureColorNotices"]=True
        case["sensorProbe"]["qualification"]="Original heldout; exact full-region dynamic H/V gate"
    fixtures["input"]="Node.paintLine opposite pressure ramps; original unseen foreground colors"
    path.write_text(json.dumps(fixtures,indent=2))
