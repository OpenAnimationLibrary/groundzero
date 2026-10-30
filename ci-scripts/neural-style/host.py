"""CI-only application transport qualification, after the native app build."""
import os
import platform
from pathlib import Path
import subprocess
import sys

try:
    arguments = [sys.executable, str(Path(__file__).with_name("evaluate.py"))]
    if sys.platform == "darwin" and platform.machine() == "x86_64":
        arguments += ["--native-macos", "neural-golden/golden.npz"]
    subprocess.run(arguments, check=True)
    name = "neuralstyle_host_test" + (".exe" if os.name == "nt" else "")
    candidates = [p for p in Path("toonz/build").rglob(name)
                  if p.is_file() and "opentoonz" not in {part.lower() for part in p.parts}]
    if len(candidates) != 1:
        raise RuntimeError("Expected exactly one built Qt host qualification executable")
    result = subprocess.run([str(candidates[0].resolve()),
                             str(Path("neural-evaluation/report/native-package").resolve()),
                             str(Path("neural-evaluation/report/host-fixtures").resolve())],
                            capture_output=True, text=True, timeout=180)
    print(result.stdout, flush=True)
    if result.returncode:
        raise RuntimeError("Qt host qualification failed: " + result.stderr[-3000:])
except Exception as error:
    print("::error title=Neural Qt host qualification::" + str(error).replace("\n", " | "), flush=True)
    raise
