"""Prepare original focused scatter holdouts; no source admission is inferred."""
import json
from scatter_holdout_contract import GROUPS, SAMPLE_POLICY, cases, validate_authorship, status, candidate_expected


def prepare(root, resources, preset_writer, group):
    authored = list(cases(group))
    validate_authorship(group, authored)
    names = []
    for index, case in enumerate(authored):
        name = "GZ Original dry " + group + " " + str(index).zfill(3)
        case["preset"] = name
        names.append(name)
        (resources / "paintoppresets" / (name + ".kpp")).write_bytes(
            preset_writer(name, "dry", False, case["overrides"], case["diameter"], case["spacing"], 0))
    (root / "output/fixtures.json").write_text(json.dumps({
        "version": "5.3.4.1", "group": group, "width": 2048, "height": 2048, "license": "CC0-1.0",
        "presets": names, "cases": authored, "randomizeExecution": True,
        "stochasticSamplePolicy": SAMPLE_POLICY,
        "developmentAcquisition": True, "implementationStatus": status(group),
        "candidateExpected": candidate_expected(group),
        "sourceProfileVerified": False, "pixelParityClaimed": False,
        "integrationStatus": "Focus-only development acquisition; source scope and qualification remain explicit",
        "input": "Public Node.paintLine static holdouts; actual Qt tablet events for pressure holdouts; never claim a physical tablet"}, indent=2))
