"""Qualify cache-builder release with an isolated Linux LeakSanitizer mutation."""

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

CACHE_SOURCE = Path("toonz/sources/common/tcache/timagecache.cpp")
FIXTURE_SOURCE = Path("toonz/sources/common/tcache/tests/timagecache_test.cpp")
MEMORY_DESTRUCTOR = "CompressedOnMemoryCacheItem::~CompressedOnMemoryCacheItem() {"
DELETE_BUILDER = "\n  delete m_builder;"
LSAN_OPTIONS = "detect_leaks=1:exitcode=23:symbolize=1:allow_addr2line=1:fast_unwind_on_malloc=0"
SUCCESS = "Image cache compression tests passed"


def remove_builder_delete(source: str) -> str:
    """Restore precisely the missing memory-cache delete, rejecting source drift."""
    fixed = MEMORY_DESTRUCTOR + DELETE_BUILDER
    if source.count(MEMORY_DESTRUCTOR) != 1 or source.count(fixed) != 1:
        raise ValueError("Expected exactly one fixed memory-cache builder destructor")
    return source.replace(fixed, MEMORY_DESTRUCTOR, 1)


def configure_options(cache: Path) -> list[str]:
    """Preserve the existing build's compilers, flags and selected feature options."""
    compiler_setting = re.compile(
        r"CMAKE_(?:BUILD_TYPE|(?:C|CXX)_COMPILER(?:_LAUNCHER)?|"
        r"(?:C|CXX|EXE_LINKER|SHARED_LINKER|MODULE_LINKER)_FLAGS(?:_[A-Z]+)?)$"
    )
    options = []
    for line in cache.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([^#/:][^:]*):(BOOL|STRING|FILEPATH|PATH|UNINITIALIZED)=(.*)", line)
        if not match:
            continue
        key, _kind, value = match.groups()
        if (
            compiler_setting.fullmatch(key)
            or key == "Python3_EXECUTABLE"
            or key.startswith(("BUILD_", "WITH_", "OT_"))
        ):
            options.append(f"-D{key}={value}")
    if "-DBUILD_RELIABILITY_TESTS=ON" not in options:
        raise ValueError("The source build must enable BUILD_RELIABILITY_TESTS")
    return options


def copy_sources(root: Path, build: Path, destination: Path) -> list[str]:
    """Copy mutable sources; keep installed SDK inputs outside the disposable build."""
    for name in ("sources", "cmake"):
        shutil.copytree(root / "toonz" / name, destination / "toonz" / name)
    # Existing enabled SeExpr targets reference these sources during generation.
    shutil.copytree(root / "ci-scripts/seexpr", destination / "ci-scripts/seexpr")
    # Linux SDK configuration only reads these inputs; no SDK target is built.
    (destination / "thirdparty").symlink_to(root / "thirdparty", target_is_directory=True)
    (destination / "toonz/install").symlink_to(root / "toonz/install", target_is_directory=True)
    (destination / "stuff/doc").mkdir(parents=True)
    options = ["-DFETCHCONTENT_FULLY_DISCONNECTED=ON"]
    dependencies = sorted((build / "_deps").glob("*-src"))
    if not dependencies:
        raise ValueError("The source build has no populated FetchContent dependencies")
    for dependency in dependencies:
        copied = destination / "dependencies" / dependency.name
        shutil.copytree(dependency, copied, ignore=shutil.ignore_patterns(".git"))
        name = dependency.name.removesuffix("-src").upper()
        options.append(f"-DFETCHCONTENT_SOURCE_DIR_{name}={copied}")
    return options


def run_logged(command: list[str], log: Path, environment: dict[str, str], timeout: int) -> int:
    """Capture a bounded command and reap its process group on timeout."""
    with (
        log.open("wb") as output,
        subprocess.Popen(
            command,
            stdout=output,
            stderr=subprocess.STDOUT,
            env=environment,
            start_new_session=True,
        ) as process,
    ):
        try:
            return process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise RuntimeError(f"{log.name}: command exceeded {timeout} seconds") from None


def require_result(code: int, output: str, *, mutated: bool) -> None:
    """Reject ordinary crashes and unrelated leak reports as mutation evidence."""
    if SUCCESS not in output:
        raise RuntimeError("Cache pixel/removal assertions did not finish")
    if not mutated:
        if code != 0 or "LeakSanitizer:" in output:
            raise RuntimeError(f"Fixed cache fixture failed (exit {code})")
        return
    if code != 23 or "LeakSanitizer: detected memory leaks" not in output:
        raise RuntimeError(f"Mutation did not produce the required LSan exit 23 (exit {code})")
    reports = re.split(r"(?=Direct leak of|Indirect leak of|SUMMARY:)", output)
    builder_stack = re.compile(
        r"#\d+\s+.*(?:CompressedOnMemoryCacheItem::CompressedOnMemoryCacheItem|"
        r"RasterImageBuilder|ToonzImageBuilder)"
    )
    if not any(
        block.startswith("Direct leak of") and builder_stack.search(block) for block in reports
    ):
        raise RuntimeError("Mutation report has no direct cache-builder allocation stack")


def run_fixture(binary: Path, log: Path, environment: dict[str, str], *, mutated: bool) -> int:
    """Run the real fixture and require the expected ownership result."""
    runtime = dict(environment)
    runtime["LD_LIBRARY_PATH"] = str(binary.parent.parent / "lib/opentoonz")
    code = run_logged([str(binary)], log, runtime, 120)
    captured = log.read_text(encoding="utf-8", errors="replace")
    try:
        require_result(code, captured, mutated=mutated)
    except RuntimeError as error:
        raise RuntimeError(f"{log.name}: {error}\n{captured[-6000:]}") from error
    return code


def build_target(build: Path, log: Path, environment: dict[str, str]) -> None:
    """Build only the existing image-cache fixture and its dependencies."""
    code = run_logged(
        [
            "cmake",
            "--build",
            str(build),
            "--target",
            "timagecache_test",
            "--parallel",
            "2",
        ],
        log,
        environment,
        900,
    )
    if code:
        captured = log.read_text(encoding="utf-8", errors="replace")
        raise RuntimeError(f"{log.name}: fixture build failed (exit {code})\n{captured[-6000:]}")


def qualify(root: Path, build: Path, output: Path, report: dict[str, object]) -> None:
    """Compare shipping, leak-restored, and repaired cache fixture executions."""
    source = (root / CACHE_SOURCE).read_bytes()
    fixed = source.decode("utf-8")
    mutated = remove_builder_delete(fixed)
    environment = dict(os.environ, LSAN_OPTIONS=LSAN_OPTIONS)
    report.update(
        {
            "source_sha256": hashlib.sha256(source).hexdigest(),
            "fixture_sha256": hashlib.sha256((root / FIXTURE_SOURCE).read_bytes()).hexdigest(),
            "mutation": "remove only CompressedOnMemoryCacheItem builder delete",
            "limits": "Disk-spill clone destruction and constructor exceptions are not qualified.",
        }
    )
    report["normal_exit"] = run_fixture(
        build / "bin/timagecache_test",
        output / "normal.log",
        environment,
        mutated=False,
    )
    with tempfile.TemporaryDirectory(prefix="image-cache-lsan-") as temporary:
        copied = Path(temporary)
        options = configure_options(build / "CMakeCache.txt")
        options += copy_sources(root, build, copied)
        cache = copied / CACHE_SOURCE
        cache.write_text(mutated, encoding="utf-8")
        isolated = copied / "build"
        command = [
            "cmake",
            "-S",
            str(copied / "toonz/sources"),
            "-B",
            str(isolated),
            "-G",
            "Ninja",
            *options,
        ]
        code = run_logged(command, output / "configure.log", environment, 300)
        if code:
            captured = (output / "configure.log").read_text(encoding="utf-8", errors="replace")
            raise RuntimeError(f"Disposable configuration failed (exit {code})\n{captured[-6000:]}")
        build_target(isolated, output / "mutated-build.log", environment)
        report["mutated_exit"] = run_fixture(
            isolated / "bin/timagecache_test",
            output / "mutated.log",
            environment,
            mutated=True,
        )
        cache.write_bytes(source)
        build_target(isolated, output / "repaired-build.log", environment)
        report["repaired_exit"] = run_fixture(
            isolated / "bin/timagecache_test",
            output / "repaired.log",
            environment,
            mutated=False,
        )
    if (root / CACHE_SOURCE).read_bytes() != source:
        raise RuntimeError("Original cache source changed during qualification")


def main() -> int:
    """Write machine-readable evidence and fail CI on incomplete qualification."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report: dict[str, object] = {"schema": 1, "status": "failed"}
    try:
        if sys.platform != "linux":
            raise RuntimeError("The image-cache LSan qualifier requires Linux")
        qualify(args.root.resolve(), args.build.resolve(), output, report)
        report["status"] = "passed"
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        message = str(error).replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        print(f"::error title=Image cache LSan::{message}", flush=True)
    finally:
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return int(report["status"] != "passed")


if __name__ == "__main__":
    sys.exit(main())
