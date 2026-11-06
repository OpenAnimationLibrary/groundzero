"""CI-only integration guard between generated manifests and the C++ comparator.

Every reference workflow group is generated from original metadata with stdlib
helpers. No Krita runtime, downloaded assets, or source rendering is needed.
"""
import copy
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


# Preserve whole-row truncation controls for the original matrix regressions
# and the timed-sensor expansion. Other groups still exercise generic manifest
# cardinality by deleting a case without deleting its declared preset identity.
TRUNCATION_GROUPS = frozenset((
    "wethsv", "gihcadence", "generatedtips", "autospacing", "scatter-x-east",
    "wetgeometry", "timedsensors", "generatedcurves",
))


def workflow_groups(workflow):
    """Read the default matrix and focused-only groups from the owning workflow."""
    text = workflow.read_text(encoding="utf-8")
    expressions = re.findall(r"(?m)^\s*fixture_group:\s*(\$\{\{[^\n]*\}\})\s*$", text)
    if len(expressions) != 1:
        raise RuntimeError("Expected one explicit reference fixture_group expression")
    matrices = [json.loads(value) for value in
                re.findall(r"fromJSON\('([^']+)'\)", expressions[0])]
    if not matrices:
        raise RuntimeError("Reference workflow has no declared fixture matrices")
    for matrix in matrices:
        if (type(matrix) is not list or not matrix or len(matrix) > 128 or
                any(type(group) is not str or
                    re.fullmatch(r"[a-z][a-z0-9-]{0,63}", group) is None
                    for group in matrix) or len(set(matrix)) != len(matrix)):
            raise RuntimeError("Malformed bounded reference fixture matrix")
    # The final fallback is the full PR/default matrix. Validate it first, then
    # focused-only diagnostics such as Lab and repeat investigations.
    groups = list(dict.fromkeys(matrices[-1] + [g for matrix in matrices[:-1] for g in matrix]))
    if len(groups) > 128 or not TRUNCATION_GROUPS.issubset(groups):
        raise RuntimeError("Reference workflow omitted a required fixture regression")
    return groups


def native_result(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=30)


def main():
    comparator = str(Path(sys.argv[1]).resolve())
    prepare = Path(__file__).with_name("prepare.py").resolve()
    workflow = prepare.parents[2] / ".github" / "workflows" / "raster_brush_reference.yml"
    groups = workflow_groups(workflow)
    failures = []
    for group in groups:
        # Keep the retained generated resources bounded to one group at a time.
        with tempfile.TemporaryDirectory(prefix="gz-fixture-matrix-") as folder:
            root = Path(folder)
            environment = dict(os.environ, BRUSH_REFERENCE_ROOT=str(root),
                               BRUSH_REFERENCE_GROUP=group)
            subprocess.run([sys.executable, str(prepare)], env=environment,
                           check=True, timeout=60)
            manifest = root / "output" / "fixtures.json"
            original = json.loads(manifest.read_text(encoding="utf-8"))
            print("Fixture matrix " + group + ": " + str(len(original["presets"])) +
                  " presets", flush=True)
            command = [comparator, "--validate-fixtures", str(root)]
            result = native_result(command)
            if result.returncode != 0:
                failures.append(group + ": generated matrix rejected: " +
                                (result.stderr or result.stdout).strip())
            defects = ["missing-case", "duplicate", "mismatched", "version"]
            if group in TRUNCATION_GROUPS:
                defects.insert(0, "truncated")
            for defect in defects:
                broken = copy.deepcopy(original)
                if defect == "truncated":
                    broken["presets"].pop()
                    broken["cases"].pop()
                elif defect == "missing-case":
                    broken["cases"].pop()
                elif defect == "duplicate":
                    if len(broken["presets"]) > 1:
                        broken["presets"][1] = broken["presets"][0]
                        broken["cases"][1]["preset"] = broken["presets"][0]
                    else:
                        broken["presets"].append(broken["presets"][0])
                        broken["cases"].append(copy.deepcopy(broken["cases"][0]))
                elif defect == "mismatched":
                    broken["cases"][0]["preset"] = "different fixture"
                else:
                    broken["version"] = "different baseline"
                manifest.write_text(json.dumps(broken), encoding="utf-8")
                result = native_result(command)
                if result.returncode != 1 or "Brush comparison failed" not in result.stderr:
                    failures.append(group + ": comparator admitted " + defect +
                                    " or failed without a validation diagnostic: " +
                                    (result.stderr or result.stdout).strip())
    if failures:
        raise RuntimeError("Fixture matrix integration failures:\n" + "\n".join(failures))
    print("All " + str(len(groups)) +
          " workflow fixture matrices and negative controls passed")


if __name__ == "__main__":
    main()
