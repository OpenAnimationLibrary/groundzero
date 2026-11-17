"""Stage existing runtimes for contained Windows live identity qualification.

This preparation command creates no accounts, changes no ACLs, and launches no
application. Identity-changing qualification remains a separate reviewed step.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from collections import deque
from pathlib import Path
from typing import Any

PACKAGE_NAME = re.compile(r"^(?:@[a-z0-9._-]+/)?[a-z0-9][a-z0-9._-]*$")


def require_hosted_windows() -> None:
    """Reject preparation outside an explicitly hosted Windows Actions job."""
    expected = {
        "GITHUB_ACTIONS": "true",
        "RUNNER_ENVIRONMENT": "github-hosted",
        "RUNNER_OS": "Windows",
    }
    if os.name != "nt" or any(
        os.environ.get(key) != value for key, value in expected.items()
    ):
        raise RuntimeError(
            "This preparer requires a Windows GitHub-hosted Actions runner"
        )


def read_object(path: Path) -> dict[str, Any]:
    """Read bounded package metadata without interpreting scripts.

    Args:
        path: Existing package JSON file.

    Returns:
        The decoded JSON object.
    """
    if path.stat().st_size > 1024 * 1024:
        raise ValueError("Package metadata exceeds the preparation limit")
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError("Package metadata must be an object")
    return value


def package_identity(package: Path) -> tuple[str, str]:
    """Validate the installed package identity used to preserve resolution.

    Args:
        package: Resolved installed package directory.

    Returns:
        Its package name and exact installed version.
    """
    metadata = read_object(package / "package.json")
    name, version = metadata.get("name"), metadata.get("version")
    if not isinstance(name, str) or PACKAGE_NAME.fullmatch(name) is None:
        raise ValueError("Installed package name is outside the staging profile")
    if not isinstance(version, str) or not version or len(version) > 128:
        raise ValueError("Installed package version is invalid")
    return name, version


def resolve_package(start: Path, name: str) -> Path:
    """Follow Node's ancestor lookup for an already installed dependency.

    Args:
        start: Package or adapter root from which the dependency is resolved.
        name: Validated package name.

    Returns:
        The actual installed package directory behind pnpm links.
    """
    if PACKAGE_NAME.fullmatch(name) is None:
        raise ValueError("Dependency name is outside the staging profile")
    for parent in (start, *start.parents):
        candidate = parent / "node_modules" / name
        if (candidate / "package.json").is_file():
            resolved = candidate.resolve(strict=True)
            if package_identity(resolved)[0] != name:
                raise ValueError(
                    "Aliased runtime packages require a different staging profile"
                )
            return resolved
    raise FileNotFoundError(f"Installed runtime dependency is missing: {name}")


def dependencies(package: Path) -> list[Path]:
    """Collect installed runtime dependencies without installing anything.

    Args:
        package: Resolved installed package directory.

    Returns:
        Resolved mandatory and available optional runtime dependencies.
    """
    metadata = read_object(package / "package.json")
    required = metadata.get("dependencies", {})
    optional = metadata.get("optionalDependencies", {})
    if not isinstance(required, dict) or not isinstance(optional, dict):
        raise ValueError("Runtime dependency metadata must be an object")
    found = [
        resolve_package(package, name) for name in required if name not in optional
    ]
    for name in optional:
        try:
            found.append(resolve_package(package, name))
        except FileNotFoundError:
            continue  # An uninstalled optional dependency stays absent in the copy.
    return found


def copy_package(package: Path, destination: Path) -> None:
    """Copy one installed package without following cyclic dependency links.

    Args:
        package: Resolved source directory.
        destination: New flat node_modules package directory.
    """
    metadata = read_object(package / "package.json")
    if metadata.get("bundledDependencies") or metadata.get("bundleDependencies"):
        raise ValueError(
            "Bundled runtime dependencies require a different staging profile"
        )
    shutil.copytree(package, destination, ignore=shutil.ignore_patterns("node_modules"))


def stage_dependencies(adapter: Path, destination: Path) -> list[dict[str, str]]:
    """Materialize the installed SDK closure and reject version flattening conflicts.

    Args:
        adapter: Existing adapter package root.
        destination: New adapter package root receiving flat dependencies.

    Returns:
        Exact package names and versions copied from the installed runtime.
    """
    roots = ("@modelcontextprotocol/server", "@modelcontextprotocol/client")
    pending = deque(resolve_package(adapter, name) for name in roots)
    versions: dict[str, str] = {}
    sources: dict[str, Path] = {}
    while pending:
        package = pending.popleft()
        name, version = package_identity(package)
        if name in versions:
            if versions[name] != version:
                raise ValueError(
                    f"Cannot flatten conflicting installed versions of {name}"
                )
            if sources[name] != package:
                raise ValueError(f"Cannot flatten distinct installed copies of {name}")
            continue
        versions[name] = version
        sources[name] = package
        copy_package(package, destination / "node_modules" / name)
        pending.extend(dependencies(package))
    return [
        {"name": name, "version": version} for name, version in sorted(versions.items())
    ]


def copy_directory(source: Path, destination: Path) -> Path:
    """Copy an existing runtime directory into the new owned staging tree.

    Args:
        source: Existing runtime directory.
        destination: New destination directory.

    Returns:
        The copied destination.
    """
    shutil.copytree(source, destination)
    return destination


def file_digest(path: Path) -> str:
    """Hash one staged artifact without loading its entire contents.

    Args:
        path: Existing staged file.

    Returns:
        Its SHA-256 hexadecimal digest.
    """
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def stage_adapter(arguments: argparse.Namespace, root: Path) -> list[dict[str, str]]:
    """Copy built adapter code, fixed fixtures, and the exact installed SDK closure.

    Args:
        arguments: Explicit source adapter and client paths.
        root: New staging root.

    Returns:
        Dependency identities used in the retained preparation manifest.
    """
    source = arguments.adapter.parent.parent
    if arguments.adapter != source / "dist" / "cli.js":
        raise ValueError("The adapter input must be the built package dist/cli.js")
    if arguments.client != source / "fixtures" / "live-client.mjs":
        raise ValueError("The SDK client must be the fixed live-client.mjs fixture")
    target = root / "adapter"
    target.mkdir()
    shutil.copy2(source / "package.json", target / "package.json")
    copy_directory(source / "dist", target / "dist")
    copy_directory(source / "fixtures", target / "fixtures")
    return stage_dependencies(source, target)


def stage(arguments: argparse.Namespace) -> dict[str, Any]:
    """Prepare inputs only; account and desktop mutation are deliberately absent.

    Args:
        arguments: Validated hosted-runner inputs and new output root.

    Returns:
        A complete configuration and input identity manifest for review.
    """
    root = arguments.output
    copy_directory(arguments.executable.parent, root / "application")
    copy_directory(arguments.stuff, root / "stuff")
    copy_directory(arguments.windows_mesa, root / "mesa")
    copy_directory(Path(sys.base_prefix), root / "python")
    copy_directory(Path(__file__).resolve().parent, root / "ci-scripts")
    shutil.copy2(arguments.node, root / "node.exe")
    shutil.copy2(arguments.helper, root / "application" / "windows_ci_live_test.exe")
    packages = stage_adapter(arguments, root)
    files = {
        "python": root / "python" / "python.exe",
        "suite": root / "ci-scripts" / "mcp_live.py",
        "executable": root / "application" / arguments.executable.name,
        "node": root / "node.exe",
        "adapter": root / "adapter" / "dist" / "cli.js",
        "client": root / "adapter" / "fixtures" / "live-client.mjs",
        "helper": root / "application" / "windows_ci_live_test.exe",
    }
    return {
        "status": "prepared",
        "qualification_executed": False,
        "identity_mutations_enabled": False,
        "protocol": "groundzero-windows-live-ci-v1",
        "files": {name: str(path) for name, path in files.items()},
        "sha256": {name: file_digest(path) for name, path in files.items()},
        "stuff": str(root / "stuff"),
        "windows_mesa": str(root / "mesa"),
        "packages": packages,
    }


def parse_arguments() -> argparse.Namespace:
    """Require explicit inputs and a new descendant of the hosted temporary root.

    Returns:
        Canonical existing inputs and a new prepared-output directory.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = (
        "helper",
        "executable",
        "stuff",
        "node",
        "adapter",
        "client",
        "windows-mesa",
    )
    for name in (*inputs, "output"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    arguments = parser.parse_args()
    for name in inputs:
        attribute = name.replace("-", "_")
        setattr(
            arguments, attribute, getattr(arguments, attribute).resolve(strict=True)
        )
    temporary = Path(os.environ["RUNNER_TEMP"]).resolve(strict=True)
    arguments.output = arguments.output.resolve()
    if arguments.output == temporary or not arguments.output.is_relative_to(temporary):
        raise ValueError("Prepared output must be a new descendant of RUNNER_TEMP")
    arguments.output.mkdir(parents=False, exist_ok=False)
    return arguments


def main() -> int:
    """Retain preparation evidence without claiming Windows runtime qualification."""
    require_hosted_windows()
    arguments = parse_arguments()
    try:
        report = stage(arguments)
    except (OSError, ValueError, RuntimeError) as error:
        report = {
            "status": "failed",
            "qualification_executed": False,
            "identity_mutations_enabled": False,
            "error": str(error),
        }
    (arguments.output / "prepared.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        f"Windows input preparation: {report['status']}; no application or account was created."
    )
    return 0 if report["status"] == "prepared" else 1


if __name__ == "__main__":
    raise SystemExit(main())
