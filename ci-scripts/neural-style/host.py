"""CI-only application transport qualification, after the native app build."""
import os
import json
import platform
from pathlib import Path
import subprocess
import shutil
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
    name = "neuralstyle_test" + (".exe" if os.name == "nt" else "")
    effects = [p for p in Path("toonz/build").rglob(name)
               if p.is_file() and "opentoonz" not in {part.lower() for part in p.parts}]
    if len(effects) != 1:
        raise RuntimeError("Expected exactly one built neural FX qualification executable")
    package = Path("neural-evaluation/report/native-package")
    shutil.copytree(package, effects[0].parent / "neural-style", dirs_exist_ok=True)
    environment = os.environ.copy()
    if sys.platform == "darwin":
        environment["DYLD_LIBRARY_PATH"] = str(Path("toonz/build/toonz/OpenToonz.app/Contents/MacOS").resolve())
    result = subprocess.run([str(effects[0].resolve()),
                             str(Path("neural-evaluation/report/host-fixtures").resolve())],
                            capture_output=True, text=True, timeout=600, env=environment)
    print(result.stdout, flush=True)
    if result.returncode:
        if result.returncode < 0 and sys.platform == "linux" and shutil.which("gdb"):
            trace = subprocess.run(["gdb", "--batch", "-ex", "run", "-ex", "thread apply all backtrace",
                                    "--args", str(effects[0].resolve()),
                                    str(Path("neural-evaluation/report/host-fixtures").resolve())],
                                   capture_output=True, text=True, timeout=300, env=environment)
            print(trace.stdout[-16000:], trace.stderr[-3000:], flush=True)
        raise RuntimeError(f"Neural FX qualification failed (exit {result.returncode}): " +
                           result.stdout[-2000:] + result.stderr[-3000:])
    metrics = json.loads(Path("neural-evaluation/report/artist-review/metrics.json").read_text())
    for entry in metrics:
        print("::notice title=Neural visual motion measurements::" + json.dumps(entry), flush=True)
    name = "neuralstyle_editor_test" + (".exe" if os.name == "nt" else "")
    editors = [p for p in Path("toonz/build").rglob(name)
               if p.is_file() and "opentoonz" not in {part.lower() for part in p.parts}]
    if len(editors) != 1:
        raise RuntimeError("Expected exactly one built neural editor qualification executable")
    if editors[0].parent != effects[0].parent:
        shutil.copytree(package, editors[0].parent / "neural-style", dirs_exist_ok=True)
    result = subprocess.run([str(editors[0].resolve()), str(Path("neural-evaluation/report/artist-review").resolve())],
                            capture_output=True, text=True, timeout=180, env=environment)
    print(result.stdout, flush=True)
    if result.returncode:
        raise RuntimeError("Neural editor qualification failed: " + result.stderr[-3000:])
    destinations = {"win32": "toonz/build/Opentoonz/neural-style",
                    "darwin": "toonz/build/toonz/OpenToonz.app/Contents/MacOS/neural-style"}
    destination = Path(destinations.get(sys.platform, "toonz/build/bin/neural-style"))
    if destination.resolve() != (effects[0].parent / "neural-style").resolve():
        shutil.copytree(package, destination, dirs_exist_ok=True)
except subprocess.TimeoutExpired as error:
    for captured in (error.stdout, error.stderr):
        if captured:
            text = captured.decode(errors="replace") if isinstance(captured, bytes) else captured
            print(text[-12000:], flush=True)
    print("::error title=Neural qualification deadline::" + str(error).replace("\n", " | "), flush=True)
    raise
except Exception as error:
    print("::error title=Neural Qt host qualification::" + str(error).replace("\n", " | "), flush=True)
    raise
