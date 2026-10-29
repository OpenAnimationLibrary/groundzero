"""Install the pinned optional LLVM development SDK for CI qualification only."""

from pathlib import Path
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request


VERSION = "14.0.6"
RELEASE = "https://github.com/llvm/llvm-project/releases/download/llvmorg-14.0.6/"
SOURCE_ARCHIVE = (
    "llvm-14.0.6.src.tar.xz",
    "050922ecaaca5781fdf6631ea92bc715183f202f9d2f15147226f023414f619a",
)
ARCHIVES = {
    "Darwin": (
        "clang+llvm-14.0.6-x86_64-apple-darwin.tar.xz",
        "e6cc6b8279661fd4452c2847cb8e55ce1e54e1faf4ab497b37c85ffdb6685e7c",
    ),
    "Windows": SOURCE_ARCHIVE,
}


def digest(path):
    checksum = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def download(name, checksum, target):
    if target.is_file() and digest(target) == checksum:
        return
    for attempt in range(3):
        try:
            request = urllib.request.Request(
                RELEASE + name.replace("+", "%2B"),
                headers={"User-Agent": "SeExpr-qualification"},
            )
            with urllib.request.urlopen(request, timeout=120) as response:
                with target.open("wb") as output:
                    shutil.copyfileobj(response, output)
            if digest(target) != checksum:
                raise RuntimeError("LLVM dependency archive checksum mismatch")
            return
        except (OSError, RuntimeError):
            target.unlink(missing_ok=True)
            if attempt == 2:
                raise
            time.sleep(2)


def windows_sdk(source, install, build):
    # A compiler-tools installer does not supply the complete native SDK.
    # Build actual LLVM14 libraries with the same release CRT as the consumer.
    configure = [
        "cmake", "-S", str(source), "-B", str(build),
        "-G", "Visual Studio 17 2022", "-A", "x64", "-T", "host=x64",
        "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={install}",
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL",
        "-DLLVM_USE_CRT_RELEASE=MD", "-DLLVM_TARGETS_TO_BUILD=X86",
        "-DLLVM_ENABLE_EH=ON", "-DLLVM_ENABLE_RTTI=ON",
        "-DLLVM_ENABLE_ASSERTIONS=OFF", "-DLLVM_ENABLE_WERROR=OFF",
        "-DLLVM_INCLUDE_TOOLS=OFF", "-DLLVM_INCLUDE_UTILS=OFF",
        "-DLLVM_INCLUDE_TESTS=OFF", "-DLLVM_INCLUDE_EXAMPLES=OFF",
        "-DLLVM_INCLUDE_BENCHMARKS=OFF", "-DLLVM_INCLUDE_DOCS=OFF",
        "-DLLVM_ENABLE_ZLIB=OFF", "-DLLVM_ENABLE_TERMINFO=OFF",
        "-DLLVM_ENABLE_LIBXML2=OFF", "-DLLVM_ENABLE_LIBEDIT=OFF",
    ]
    subprocess.run(configure, check=True, timeout=600)
    subprocess.run(
        ["cmake", "--build", str(build), "--config", "Release",
         "--target", "install", "--parallel", "4"],
        check=True, timeout=3600,
    )


def license_notice(temporary, destination):
    name, checksum = SOURCE_ARCHIVE
    downloads = temporary / "seexpr-llvm14-downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    archive = downloads / name
    download(name, checksum, archive)
    with tarfile.open(archive, "r:xz") as source:
        notice = source.extractfile("llvm-14.0.6.src/LICENSE.TXT")
        if notice is None:
            raise RuntimeError("LLVM source archive is missing its license")
        text = notice.read()
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(text)


def main():
    if not os.environ.get("GITHUB_ENV") or not os.environ.get("RUNNER_TEMP"):
        raise RuntimeError("LLVM setup requires the CI environment output file")
    temporary = Path(os.environ["RUNNER_TEMP"])
    if sys.argv[1:] == ["--license-only"]:
        notice = temporary / "seexpr-llvm14-notices" / "LICENSE_LLVM.txt"
        license_notice(temporary, notice)
        with Path(os.environ["GITHUB_ENV"]).open("a", encoding="utf-8") as output:
            output.write(f"LLVM_LICENSE_FILE={notice.as_posix()}\n")
        print("Pinned LLVM14 distribution notice is ready.", flush=True)
        return
    if sys.argv[1:]:
        raise RuntimeError("Unknown LLVM dependency setup option")
    system = platform.system()
    if system not in ARCHIVES or platform.machine().lower() not in (
        "amd64", "x86_64",
    ):
        raise RuntimeError("LLVM14 dependency setup requires an Intel CI runner")
    name, checksum = ARCHIVES[system]
    install = temporary / "seexpr-llvm14-sdk"
    manifest = install / ".seexpr-llvm-sdk.json"
    expected = {
        "version": VERSION, "system": system, "sha256": checksum,
        "runtime": "MD" if system == "Windows" else "official-x86_64",
        "layout": 2,
    }
    configuration = install / "lib" / "cmake" / "llvm" / "LLVMConfig.cmake"
    cached = False
    if manifest.is_file() and configuration.is_file():
        cached = json.loads(manifest.read_text(encoding="utf-8")) == expected
    if not cached:
        if install.exists():
            shutil.rmtree(install)
        download_root = temporary / "seexpr-llvm14-downloads"
        download_root.mkdir(parents=True, exist_ok=True)
        archive = download_root / name
        download(name, checksum, archive)
        source_root = temporary / "seexpr-llvm14-source"
        if source_root.exists():
            shutil.rmtree(source_root)
        source_root.mkdir()
        with tarfile.open(archive, "r:xz") as source:
            source.extractall(source_root, filter="data")
        if system == "Darwin":
            shutil.move(str(source_root / name.removesuffix(".tar.xz")), install)
        else:
            windows_sdk(
                source_root / "llvm-14.0.6.src", install,
                temporary / "seexpr-llvm14-build",
            )
        if not configuration.is_file():
            raise RuntimeError("LLVM dependency is missing its native CMake SDK")
        license_notice(temporary, install / "LICENSE_LLVM.txt")
        manifest.write_text(json.dumps(expected, sort_keys=True), encoding="utf-8")
    config_text = configuration.read_text(encoding="utf-8")
    if "set(LLVM_VERSION_MAJOR 14)" not in config_text:
        raise RuntimeError("LLVM dependency reports an unexpected major version")
    if not (install / "include" / "llvm" / "IR" / "IRBuilder.h").is_file():
        raise RuntimeError("LLVM dependency is missing required native headers")
    if not (install / "LICENSE_LLVM.txt").is_file():
        raise RuntimeError("LLVM dependency is missing its distribution notice")
    with Path(os.environ["GITHUB_ENV"]).open("a", encoding="utf-8") as output:
        output.write(f"LLVM_DIR={configuration.parent.as_posix()}\n")
        output.write(f"LLVM_LICENSE_FILE={(install / 'LICENSE_LLVM.txt').as_posix()}\n")
        if system == "Darwin":
            current = os.environ.get("DYLD_LIBRARY_PATH", "")
            libraries = (install / "lib").as_posix()
            output.write(f"DYLD_LIBRARY_PATH={libraries}{':' + current if current else ''}\n")
    print("Pinned LLVM14 development SDK is ready.", flush=True)


if __name__ == "__main__":
    main()
