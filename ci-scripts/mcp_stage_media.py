"""Copy installed media tools into a canonical directory for native MCP CI tests."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path
from typing import Any

from run_harness_tests import annotation


def chocolatey_pair(root: Path) -> dict[str, Path]:
    """Resolve the installed distribution instead of copying Chocolatey shims."""
    package = root / "lib" / "ffmpeg" / "tools"
    candidates = [
        path.resolve(strict=True)
        for path in package.rglob("ffmpeg.exe")
        if path.is_file() and (path.parent / "ffprobe.exe").is_file()
    ]
    if len(candidates) != 1:
        raise ValueError(
            f"Expected one real FFmpeg/ffprobe pair under {package}; got {len(candidates)}"
        )
    return {"ffmpeg": candidates[0], "ffprobe": candidates[0].with_name("ffprobe.exe").resolve()}


def discover_tools(windows: bool) -> dict[str, Path]:
    """Find the installed executable pair and resolve all source symlinks."""
    suffix = ".exe" if windows else ""
    tools = {}
    for name in ("ffmpeg", "ffprobe"):
        command = shutil.which(name + suffix)
        if not command:
            raise ValueError(f"Install the full FFmpeg distribution: {name + suffix} is missing")
        tools[name] = Path(command).resolve(strict=True)
    if windows:
        chocolatey = Path(
            os.environ.get("ChocolateyInstall", "C:/ProgramData/chocolatey")
        ).resolve()
        if any(path.parent == chocolatey / "bin" for path in tools.values()):
            tools = chocolatey_pair(chocolatey)
    if tools["ffmpeg"].parent != tools["ffprobe"].parent:
        raise ValueError(
            "FFmpeg and ffprobe must come from the same installed distribution directory"
        )
    return tools


def copy_file(source: Path, target: Path) -> dict[str, str]:
    """Copy one real binary and fingerprint the resulting regular file."""
    source = source.resolve(strict=True)
    if not source.is_file():
        raise ValueError(f"Media helper source is not a regular file: {source}")
    shutil.copy2(source, target)
    digest = hashlib.sha256()
    with target.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return {"source": str(source), "path": str(target), "sha256": digest.hexdigest()}


def copy_libraries(directory: Path, output: Path) -> list[dict[str, str]]:
    """Preserve DLL dependencies adjacent to the installed Windows executables."""
    return [
        copy_file(path, output / path.name)
        for path in sorted(directory.iterdir())
        if path.suffix.lower() == ".dll"
    ]


def executable_version(name: str, executable: Path) -> str:
    """Check the copied executable can load its dependencies and identify itself."""
    result = subprocess.run(
        [str(executable), "-version"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=20,
        check=False,
    )
    if result.returncode or len(result.stdout) > 65536:
        raise ValueError(
            f"Copied {name} -version failed ({result.returncode}): {result.stdout[:1000]}"
        )
    first_line = result.stdout.splitlines()[0] if result.stdout else ""
    if not first_line.startswith(name + " version "):
        raise ValueError(
            f"Copied {name} did not identify as the real executable: {first_line[:1000]}"
        )
    return result.stdout.strip()


def stage_media(output: Path, report: Path, github_env: Path, *, windows: bool) -> dict[str, Any]:
    """Stage the real pair and publish its canonical location only after validation.

    Args:
        output: New directory reserved for copies of the installed tools.
        report: JSON evidence destination, outside the binary directory if archived.
        github_env: GitHub Actions environment file receiving the validated location.
        windows: Whether to resolve Chocolatey shims and copy adjacent DLLs.

    Returns:
        Executable identities, copied file hashes, and version-command output.
    """
    sources = discover_tools(windows)
    output.mkdir(parents=True)
    output = output.resolve(strict=True)
    if "\n" in str(output) or "\r" in str(output):
        raise ValueError("Media-tools directory cannot contain a newline")
    suffix = ".exe" if windows else ""
    tools = {name: copy_file(source, output / (name + suffix)) for name, source in sources.items()}
    libraries = []
    if windows:
        libraries = copy_libraries(sources["ffmpeg"].parent, output)
    for name, metadata in tools.items():
        metadata["version"] = executable_version(name, Path(metadata["path"]))
    evidence = {"directory": str(output), "tools": tools, "libraries": libraries}
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    with github_env.open("a", encoding="utf-8") as environment:
        environment.write(f"MCP_MEDIA_TOOLS={output}\nGROUNDZERO_MEDIA_TOOLS={output}\n")
    return evidence


def main() -> int:
    """Stage installed CI media tools, reporting actionable setup errors."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--github-env", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        evidence = stage_media(
            arguments.output, arguments.report, arguments.github_env, windows=os.name == "nt"
        )
        print(json.dumps(evidence, indent=2))
        return 0
    except Exception as error:
        annotation("MCP media-tools staging", f"{type(error).__name__}: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
