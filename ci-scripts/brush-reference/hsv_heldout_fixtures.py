"""Original heldout colors/controls; no reference renderer code or brush assets."""
from option_fixtures import prepare as prepare_options
from sensor_fixtures import trace


def prepare(root, resources, preset_writer):
    plans = []
    # Unseen chromatic and achromatic inputs, neutral and unseen non-neutral
    # controls. Captures must validate actual UI/input controls before use.
    for target in ("h", "s", "v"):
        for color in ([211, 37, 149, 255], [9, 173, 231, 255], [101, 101, 101, 255]):
            for value in (.125, .5, .875):
                curve = "0," + str(value) + ";1," + str(value) + ";"
                plans.append((target, "pressure", {}, curve, trace("east"), color))
    prepare_options(root, resources, preset_writer, "hsvheldout", plans)
