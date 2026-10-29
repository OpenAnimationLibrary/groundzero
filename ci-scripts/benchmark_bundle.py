#!/usr/bin/env python3
"""Package already-built scene benchmarks; never configure, build or run them."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def build_details(build, configuration, qt_version):
    cache = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        match = re.match(r"([^:#]+):[^=]+=(.*)", line)
        if match:
            cache[match[1]] = match[2]
    compiler_files = list((build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    if len(compiler_files) != 1:
        raise ValueError("Expected one configured C++ compiler description")
    compiler = dict(re.findall(r'set\((CMAKE_CXX_COMPILER_(?:ID|VERSION)) "([^"]+)"\)',
                               compiler_files[0].read_text(encoding="utf-8")))
    if len(compiler) != 2:
        raise ValueError("Missing compiler identity/version")
    # Keep flags, without putting the checkout's absolute directory in reports.
    source = cache.get("CMAKE_HOME_DIRECTORY", "")
    flags = {key: value.replace(source, "<source>") if source else value
             for key, value in cache.items()
             if re.fullmatch(r"CMAKE_(?:CXX|EXE_LINKER)_FLAGS(?:_[A-Z]+)?", key)}
    return {
        "configuration": configuration or cache.get("CMAKE_BUILD_TYPE") or "unspecified",
        "generator": cache.get("CMAKE_GENERATOR"),
        "compiler": compiler,
        "fixture_source_sha256": file_hash(Path(source) / "toonzlib/tests/scenebench_test.cpp"),
        # Flags can contain private SDK/user paths. The normalized fingerprint
        # still rejects unlike compiler settings without publishing those paths.
        "flags_sha256": hashlib.sha256(canonical(flags)).hexdigest(),
        "qt_version": qt_version,
        "architecture": platform.machine(),
    }


def package(args):
    build = args.build.resolve(strict=True)
    suite = json.loads(args.suite.read_text(encoding="utf-8"))
    if suite.get("schema") != 1 or not suite.get("cases"):
        raise ValueError("Unsupported/empty benchmark suite")
    if not re.fullmatch(r"[0-9a-f]{40}", args.revision):
        raise ValueError("A full source commit SHA is required")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="benchmark-bundle-") as temporary:
        root = Path(temporary) / "bundle"
        root.mkdir()
        if args.platform == "windows":
            source = build / "Opentoonz"
            for library in source.glob("*.dll"):
                shutil.copy2(library, root / library.name)
            for directory in ("platforms", "imageformats"):
                if (source / directory).is_dir():
                    shutil.copytree(source / directory, root / directory)
            for name in ("scenebench_test.exe", "lzocompress.exe", "lzodecompress.exe"):
                shutil.copy2(build / args.configuration / name, root / name)
            executable = root / "scenebench_test.exe"
            if not (root / "Qt5Core.dll").is_file():
                raise ValueError("Windows package lacks its deployed Qt runtime")
        else:
            if not args.macdeployqt:
                raise ValueError("macOS packaging requires --macdeployqt")
            app = root / "OpenToonz.app"
            shutil.copytree(build / "toonz/OpenToonz.app/Contents", app / "Contents",
                            symlinks=True)
            executable = app / "Contents/MacOS/scenebench_test"
            shutil.copy2(build / "toonzlib/scenebench_test", executable)
            # Deploy into this benchmark-only copy. The distributable app and
            # its test configuration are left alone.
            subprocess.run([args.macdeployqt, str(app), "-verbose=0",
                            "-executable=" + str(executable)], check=True)
        metadata = {
            "schema": 1, "platform": args.platform,
            "revision": args.revision, "run_id": args.run_id,
            "run_attempt": args.run_attempt,
            "suite": suite, "suite_sha256": hashlib.sha256(canonical(suite)).hexdigest(),
            "executable": executable.relative_to(root).as_posix(),
            "executable_sha256": file_hash(executable),
            "build": build_details(build, args.configuration, args.qt_version),
            "build_image": {key: os.environ.get(key) for key in ("ImageOS", "ImageVersion")},
        }
        (root / "benchmark.json").write_text(json.dumps(metadata, indent=2) + "\n",
                                             encoding="utf-8")
        with tarfile.open(args.output, "w:gz") as archive:
            archive.add(root, arcname="bundle")
    print(f"Prepared {args.platform} benchmark bundle for {args.revision}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--platform", choices=("windows", "macos"), required=True)
    parser.add_argument("--configuration", default="")
    parser.add_argument("--qt-version", required=True)
    parser.add_argument("--macdeployqt")
    parser.add_argument("--revision", required=True)
    parser.add_argument("--run-id", type=int, required=True)
    parser.add_argument("--run-attempt", type=int, required=True)
    parser.add_argument("--suite", type=Path,
                        default=Path(__file__).parent / "benchmarks/scene_io_v2.json")
    parser.add_argument("--output", type=Path, required=True)
    package(parser.parse_args())


if __name__ == "__main__":
    main()
