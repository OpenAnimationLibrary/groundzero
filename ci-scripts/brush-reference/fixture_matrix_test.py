"""CI-only integration guard between generated manifests and the C++ comparator."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    comparator = str(Path(sys.argv[1]).resolve())
    prepare = Path(__file__).with_name("prepare.py")
    with tempfile.TemporaryDirectory() as folder:
        for group in ("wethsv", "gihcadence", "generatedtips"):
            root = Path(folder) / group
            environment = dict(os.environ, BRUSH_REFERENCE_ROOT=str(root),
                               BRUSH_REFERENCE_GROUP=group)
            subprocess.run([sys.executable, str(prepare)], env=environment, check=True)
            manifest = root / "output" / "fixtures.json"
            original = json.loads(manifest.read_text())
            command = [comparator, "--validate-fixtures", str(root)]
            subprocess.run(command, check=True)
            # The same native gate must reject truncation, duplicate identities,
            # a mismatched case identity, and a different source baseline.
            for defect in ("truncated", "duplicate", "mismatched", "version"):
                broken = copy.deepcopy(original)
                if defect == "truncated":
                    broken["presets"].pop()
                    broken["cases"].pop()
                elif defect == "duplicate":
                    broken["presets"][1] = broken["presets"][0]
                    broken["cases"][1]["preset"] = broken["presets"][0]
                elif defect == "mismatched":
                    broken["cases"][0]["preset"] = "different fixture"
                else:
                    broken["version"] = "different baseline"
                manifest.write_text(json.dumps(broken))
                result = subprocess.run(command, capture_output=True, text=True)
                if result.returncode != 1 or "Brush comparison failed" not in result.stderr:
                    raise RuntimeError(group + ": comparator admitted " + defect +
                                       " or failed without a validation diagnostic")
            manifest.write_text(json.dumps(original))
    print("Generated fixture matrices and negative controls passed")


if __name__ == "__main__":
    main()
