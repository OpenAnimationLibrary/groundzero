"""Isolated CI qualification. Never invoked by the application or local builds."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import urllib.request
import venv
import zipfile

from pip._vendor.packaging.tags import sys_tags
from pip._vendor.packaging.utils import parse_wheel_filename

ROOT = Path("neural-evaluation")
REPORT = ROOT / "report"
WHEELS = ROOT / "wheels"
MODELS = ROOT / "models"
MAX_DOWNLOAD = 100 * 1024 * 1024
MAX_STORAGE = 500 * 1024 * 1024
MODEL_ROOT = "https://tfhub.dev/google/lite-model/magenta/arbitrary-image-stylization-v1-256/int8/"
PUBLISHER_ROOT = "https://www.kaggle.com/models/google/arbitrary-image-stylization-v1/tfLite/256-int8-"


def size(directory):
    return sum(p.stat().st_size for p in directory.rglob("*") if p.is_file() and not p.is_symlink())


def command(args):
    result = subprocess.run([str(a) for a in args], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=300)
    print(result.stdout, flush=True)
    if result.returncode:
        # Runner log downloads are not available in every review environment.
        # Include the actual dependency/runtime error in check annotations.
        detail = " | ".join(result.stdout.splitlines()[-15:])
        raise RuntimeError("Evaluation command failed: " + detail[-5000:])


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def download(url, target, limit):
    request = urllib.request.Request(url, headers={"User-Agent": "Groundzero model qualification"})
    with urllib.request.urlopen(request, timeout=60) as response:
        if response.headers.get("Content-Length") and int(response.headers["Content-Length"]) > limit:
            raise RuntimeError("Publisher artifact exceeds download bound")
        with target.open("wb") as output:
            count = 0
            while block := response.read(65536):
                count += len(block)
                if count > limit:
                    raise RuntimeError("Publisher artifact exceeds download bound")
                output.write(block)
    return count


def main():
    if ROOT.exists():
        raise RuntimeError("Evaluation directory must be fresh")
    REPORT.mkdir(parents=True)
    WHEELS.mkdir()
    MODELS.mkdir()
    requirements = Path(__file__).with_name("requirements.txt")
    # Download only immutable registry wheels selected for this interpreter.
    # Check declared totals before transfer and enforce byte limits while reading.
    # No source distributions, vendor installers, or uncontrolled pip caches.
    tags = {tag: i for i, tag in enumerate(sys_tags())}
    packages = json.loads(Path(__file__).with_name("wheels.json").read_text())
    selected = []
    for package in packages:
        if package["only_windows"] and os.name != "nt":
            continue
        candidates = []
        for wheel in package["files"]:
            matching = parse_wheel_filename(wheel["filename"])[3] & tags.keys()
            if matching:
                candidates.append((min(tags[t] for t in matching), wheel))
        if not candidates:
            raise RuntimeError("No verified wheel for this platform: " + package["name"])
        selected.append(min(candidates, key=lambda item: item[0])[1])
    downloaded = sum(wheel["bytes"] for wheel in selected)
    if downloaded + 8 * 1024 * 1024 + 65536 > MAX_DOWNLOAD:
        raise RuntimeError("Runtime and reserved model downloads exceed the 100 MiB budget")
    for wheel in selected:
        if not wheel["url"].startswith("https://files.pythonhosted.org/"):
            raise RuntimeError("Unverified registry source")
        path = WHEELS / wheel["filename"]
        count = download(wheel["url"], path, wheel["bytes"])
        if count != wheel["bytes"] or digest(path) != wheel["sha256"]:
            raise RuntimeError("Registry artifact size/hash mismatch")
    wheels = []
    for path in sorted(WHEELS.glob("*.whl")):
        wheels.append({"file": path.name, "bytes": path.stat().st_size,
                       "sha256": digest(path)})
        with zipfile.ZipFile(path) as archive:
            for info in archive.infolist():
                if info.is_dir():
                    continue
                member = info.filename
                if any(word in member.lower() for word in ("license", "notice", "copying")):
                    relative = Path(member)
                    if relative.is_absolute() or ".." in relative.parts:
                        raise RuntimeError("Invalid wheel notice path")
                    destination = REPORT / "licenses" / path.stem / relative
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_bytes(archive.read(member))
    venv.EnvBuilder(with_pip=True).create(ROOT / "venv")
    python = ROOT / "venv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    command([python, "-m", "pip", "install", "--disable-pip-version-check", "--no-cache-dir", "--no-index",
             "--find-links", WHEELS, "--only-binary=:all:", "--require-hashes", "-r", requirements])
    command([python, "-m", "pip", "check"])
    installed = size(ROOT / "venv")
    if size(ROOT) > MAX_STORAGE:
        raise RuntimeError("Installed runtime exceeds the 500 MiB workspace budget")
    command([python, "-c", "from ai_edge_litert.interpreter import Interpreter; print('LiteRT interpreter import succeeded')"])
    metadata = {"runtime": "ai-edge-litert", "version": "2.3.0", "python": "3.12",
                "wheel_download_bytes": downloaded, "installed_bytes": installed,
                "installed_bytes_excludes": "CI-provided Python interpreter; not a standalone product footprint",
                "workspace_bytes": size(ROOT), "wheels": wheels,
                "scope": "Synthetic inference evaluation only; no product or temporal qualification"}
    (REPORT / "runtime.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    models = []
    for kind in ("prediction", "transfer"):
        path = MODELS / (kind + ".tflite")
        url = MODEL_ROOT + kind + "/1?lite-format=tflite"
        count = download(url, path, 4 * 1024 * 1024)
        with path.open("rb") as model:
            header = model.read(8)
        if header[4:8] != b"TFL3":
            raise RuntimeError("Official model endpoint did not return a TFLite file; no archive or login page is executed")
        models.append({"kind": kind, "version": 1, "publisher": "Google",
                       "source": url, "license": "Apache-2.0",
                       "license_evidence": PUBLISHER_ROOT + kind,
                       "bytes": count, "sha256": digest(path)})
    license_bytes = download("https://raw.githubusercontent.com/google-ai-edge/LiteRT/v2.3.0/LICENSE",
                             REPORT / "Apache-2.0.txt", 65536)
    metadata["models"] = models
    metadata["workspace_bytes"] = size(ROOT)
    metadata["total_download_bytes"] = downloaded + sum(m["bytes"] for m in models) + license_bytes
    if metadata["workspace_bytes"] > MAX_STORAGE or metadata["total_download_bytes"] > MAX_DOWNLOAD:
        raise RuntimeError("Runtime and model artifacts exceed the evaluation budget")
    (REPORT / "provenance.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    command([python, Path(__file__).with_name("smoke.py"), MODELS, REPORT])
    if size(ROOT) > MAX_STORAGE:
        raise RuntimeError("Final evaluation workspace exceeds the 500 MiB budget")
    shutil.rmtree(WHEELS)


if __name__ == "__main__":
    started = time.monotonic()
    try:
        main()
    except Exception as error:
        REPORT.mkdir(parents=True, exist_ok=True)
        # Keep actionable failures visible to ci_audit.py without weakening checks.
        message = str(error).replace("\n", " ")
        print("::error title=Neural style evaluation::" + message, flush=True)
        (REPORT / "failure.txt").write_text(message, encoding="utf-8")
        raise
    print("Evaluation elapsed seconds:", round(time.monotonic() - started, 3))
