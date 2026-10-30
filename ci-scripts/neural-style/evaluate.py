"""Isolated CI qualification. Never invoked by the application or local builds."""

import hashlib
import json
import os
import plistlib
import re
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
MODEL_PINS = {
    "prediction": (2828838, "af6ad4b2e7aeba0675f32636082ab915ced5375229a3f8aff7e714c6213f5ed2"),
    "transfer": (284398, "7a1550643cf034a4d813c0aa276976cd15da4141b4f1ec3631db1d0d9c8e2cd1"),
}
MAC_ARCHIVE = "https://github.com/google-ai-edge/LiteRT/releases/download/v2.3.0/CLiteRT_mac.xcframework.zip"
MAC_ARCHIVE_SIZE = 12599949
MAC_ARCHIVE_SHA256 = "071b9f4bebb483709ac3d7faf98123da5453a2ebf60ce8f819aa1e30b9535a98"


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
    return result.stdout


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
    native_only = len(sys.argv) == 3 and sys.argv[1] == "--native-macos"
    if len(sys.argv) != 1 and not native_only:
        raise RuntimeError("Unexpected evaluation arguments")
    if native_only and sys.platform != "darwin":
        raise RuntimeError("Universal C API qualification requires macOS")
    golden = Path(sys.argv[2]) if native_only else None
    golden_bytes = golden.stat().st_size if golden else 0
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
        if native_only and package["name"] != "numpy":
            continue
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
    archive_bytes = MAC_ARCHIVE_SIZE if sys.platform == "darwin" else 0
    if downloaded + archive_bytes + golden_bytes + 8 * 1024 * 1024 + 65536 > MAX_DOWNLOAD:
        raise RuntimeError("Runtime and reserved model downloads exceed the 100 MiB budget")
    for wheel in selected:
        if not wheel["url"].startswith("https://files.pythonhosted.org/"):
            raise RuntimeError("Unverified registry source")
        path = WHEELS / wheel["filename"]
        count = download(wheel["url"], path, wheel["bytes"])
        if count != wheel["bytes"] or digest(path) != wheel["sha256"]:
            raise RuntimeError("Registry artifact size/hash mismatch")
    if native_only:
        requirements = ROOT / "native-requirements.txt"
        requirements.write_text("numpy==2.3.5 --hash=sha256:" + selected[0]["sha256"] + "\n")
    wheels = []
    for path in sorted(WHEELS.glob("*.whl")):
        wheels.append({"file": path.name, "bytes": path.stat().st_size,
                       "sha256": digest(path)})
        with zipfile.ZipFile(path) as archive:
            wheels[-1]["native_libraries"] = [
                {"file": info.filename, "bytes": info.file_size}
                for info in archive.infolist()
                if info.filename.endswith((".so", ".dll", ".dylib"))]
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
    if not native_only:
        command([python, "-c", "from ai_edge_litert.interpreter import Interpreter; print('LiteRT interpreter import succeeded')"])
    metadata = {"runtime": "LiteRT C API" if native_only else "ai-edge-litert",
                "version": "2.3.0", "python": "3.12 (test driver)",
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
        if (count, digest(path)) != MODEL_PINS[kind]:
            raise RuntimeError("Official model changed from qualified size/hash: " + kind)
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
    metadata["total_download_bytes"] = downloaded + archive_bytes + golden_bytes + sum(m["bytes"] for m in models) + license_bytes
    if metadata["workspace_bytes"] > MAX_STORAGE or metadata["total_download_bytes"] > MAX_DOWNLOAD:
        raise RuntimeError("Runtime and model artifacts exceed the evaluation budget")
    (REPORT / "provenance.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print("::notice title=Neural model provenance::" + json.dumps(models), flush=True)
    measurements = {key: metadata[key] for key in
                    ("runtime", "version", "wheel_download_bytes", "installed_bytes",
                     "installed_bytes_excludes", "workspace_bytes", "total_download_bytes")}
    measurements["runtime_libraries"] = wheels[0]["native_libraries"] if not native_only else []
    print("::notice title=Neural runtime measurements::" + json.dumps(measurements), flush=True)
    native = REPORT / "native-package"
    native.mkdir()
    library_names = {"win32": "libLiteRt.dll", "darwin": "libLiteRt.dylib"}
    library_name = library_names.get(sys.platform, "libLiteRt.so")
    if sys.platform == "darwin":
        archive_path = ROOT / "CLiteRT_mac.xcframework.zip"
        download(MAC_ARCHIVE, archive_path, MAC_ARCHIVE_SIZE)
        if digest(archive_path) != MAC_ARCHIVE_SHA256:
            raise RuntimeError("Universal macOS publisher artifact hash mismatch")
        with zipfile.ZipFile(archive_path) as archive:
            libraries = [info for info in archive.infolist() if Path(info.filename).name == "libCLiteRT_mac.dylib"]
            manifests = [info for info in archive.infolist() if info.filename.endswith("xcframework/Info.plist")]
            if len(libraries) != 1 or len(manifests) != 1 or libraries[0].file_size > 64 * 1024 * 1024:
                raise RuntimeError("Unexpected universal runtime archive layout")
            manifest = plistlib.loads(archive.read(manifests[0]))
            if not any(set(item.get("SupportedArchitectures", [])) == {"arm64", "x86_64"}
                       and item.get("SupportedPlatform") == "macos"
                       for item in manifest.get("AvailableLibraries", [])):
                raise RuntimeError("Publisher framework does not identify both macOS architectures")
            library_name = "libCLiteRT_mac.dylib"
            (native / library_name).write_bytes(archive.read(libraries[0]))
            for info in archive.infolist():
                if info.is_dir() or not any(word in info.filename.lower() for word in ("license", "notice", "copying")):
                    continue
                relative = Path(info.filename)
                if relative.is_absolute() or ".." in relative.parts or info.file_size > 1024 * 1024:
                    raise RuntimeError("Invalid framework notice entry")
                destination = REPORT / "licenses" / "CLiteRT_mac" / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(archive.read(info))
        architecture = command(["lipo", "-archs", native / library_name]).strip()
        if set(architecture.split()) != {"arm64", "x86_64"}:
            raise RuntimeError("Delivered native runtime is not universal")
        dependencies = command(["otool", "-arch", "all", "-L", native / library_name])
        deployment = command(["otool", "-arch", "all", "-l", native / library_name])
        minimums = re.findall(r"\bminos\s+([0-9.]+)", deployment)
        (REPORT / "macos-native-dependencies.txt").write_text(dependencies, encoding="utf-8")
        (REPORT / "macos-native-deployment.txt").write_text(deployment, encoding="utf-8")
        if not minimums or any(tuple((list(map(int, value.split('.'))) + [0, 0])[:3]) > (12, 0, 0)
                               for value in minimums):
            raise RuntimeError("Delivered native runtime requires macOS newer than 12")
        metadata["native_runtime_archive"] = {"source": MAC_ARCHIVE, "sha256": MAC_ARCHIVE_SHA256,
                                               "bytes": MAC_ARCHIVE_SIZE, "architectures": architecture,
                                               "minimum_os_versions": minimums}
        print("::notice title=Universal macOS runtime::" + json.dumps(metadata["native_runtime_archive"]), flush=True)
    else:
        runtime_wheel = next(p for p in WHEELS.glob("ai_edge_litert-*.whl"))
        with zipfile.ZipFile(runtime_wheel) as archive:
            (native / library_name).write_bytes(archive.read("ai_edge_litert/" + library_name))
    configure = ["cmake", "-S", "toonz/sources/neuralstyle", "-B", ROOT / "build", "-DCMAKE_BUILD_TYPE=Release"]
    if sys.platform == "darwin":
        configure.append("-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0")
    command(configure)
    command(["cmake", "--build", ROOT / "build", "--config", "Release", "--parallel", "2"])
    executable_name = "groundzero-style-worker" + (".exe" if os.name == "nt" else "")
    executable = ROOT / "build" / ("Release" if os.name == "nt" else "") / executable_name
    shutil.copy2(executable, native / executable_name)
    for model in MODELS.glob("*.tflite"):
        shutil.copy2(model, native / model.name)
    (REPORT / "provenance.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    shutil.copy2(REPORT / "provenance.json", native / "provenance.json")
    shutil.copy2(REPORT / "Apache-2.0.txt", native / "Apache-2.0.txt")
    shutil.copytree(REPORT / "licenses", native / "licenses")
    if native_only:
        command([python, Path(__file__).with_name("native_macos.py"), REPORT, native.resolve(), golden])
    else:
        command([python, Path(__file__).with_name("smoke.py"), MODELS, REPORT, native.resolve()])
    metadata["workspace_bytes"] = size(ROOT) + golden_bytes
    metadata["native_package_bytes"] = size(native)
    metadata["measurement_scope"] = "After native build and inference; before wheel cleanup"
    (REPORT / "provenance.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    shutil.copy2(REPORT / "provenance.json", native / "provenance.json")
    print("::notice title=Final neural package measurements::" + json.dumps({
        key: metadata[key] for key in ("workspace_bytes", "native_package_bytes", "measurement_scope")}), flush=True)
    if size(ROOT) + golden_bytes > MAX_STORAGE:
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
