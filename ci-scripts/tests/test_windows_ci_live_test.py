"""Check portable dependency staging through real temporary package trees."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from windows_ci_live_test import stage_dependencies


def package(
    directory: Path,
    name: str,
    version: str,
    dependencies: dict[str, str] | None = None,
) -> Path:
    """Create one installed-package fixture with observable source contents."""
    directory.mkdir(parents=True)
    metadata = {"name": name, "version": version, "dependencies": dependencies or {}}
    (directory / "package.json").write_text(json.dumps(metadata), encoding="utf-8")
    (directory / "index.mjs").write_text(f'export const version = "{version}";\n')
    return directory


def roots(directory: Path) -> tuple[Path, Path]:
    """Create the two required installed SDK entry packages."""
    modules = directory / "node_modules" / "@modelcontextprotocol"
    server = package(modules / "server", "@modelcontextprotocol/server", "2.4.0")
    client = package(modules / "client", "@modelcontextprotocol/client", "2.4.0")
    return server, client


def test_stages_runtime_closure_with_dependency_cycle(tmp_path: Path) -> None:
    """A dependency cycle must terminate with each installed package copied once."""
    source, destination = tmp_path / "source", tmp_path / "staged"
    server, _ = roots(source)
    metadata = json.loads((server / "package.json").read_text())
    metadata["dependencies"] = {"cycle-a": "1.0.0"}
    (server / "package.json").write_text(json.dumps(metadata))
    package(source / "node_modules/cycle-a", "cycle-a", "1.0.0", {"cycle-b": "1.0.0"})
    package(source / "node_modules/cycle-b", "cycle-b", "1.0.0", {"cycle-a": "1.0.0"})
    identities = stage_dependencies(source, destination)
    assert len(identities) == 4
    for identity in identities:
        copied = destination / "node_modules" / identity["name"]
        assert (copied / "index.mjs").read_text() == (
            f'export const version = "{identity["version"]}";\n'
        )
        assert not (copied / "node_modules").exists()


@pytest.mark.parametrize(
    ("other_version", "message"),
    [
        ("2.0.0", "conflicting installed versions"),
        ("1.0.0", "distinct installed copies"),
    ],
)
def test_rejects_conflicting_nested_packages(
    tmp_path: Path, other_version: str, message: str
) -> None:
    """Flattening must not silently select a different installed dependency context."""
    source = tmp_path / "source"
    server, client = roots(source)
    for parent, version in ((server, "1.0.0"), (client, other_version)):
        metadata = json.loads((parent / "package.json").read_text())
        metadata["dependencies"] = {"shared": version}
        (parent / "package.json").write_text(json.dumps(metadata))
        package(parent / "node_modules/shared", "shared", version)
    with pytest.raises(ValueError, match=message):
        stage_dependencies(source, tmp_path / "staged")


def test_rejects_missing_required_dependency(tmp_path: Path) -> None:
    """An incomplete installed dependency tree cannot produce a prepared result."""
    source = tmp_path / "source"
    server, _ = roots(source)
    metadata = json.loads((server / "package.json").read_text())
    metadata["dependencies"] = {"missing": "1.0.0"}
    (server / "package.json").write_text(json.dumps(metadata))
    with pytest.raises(
        FileNotFoundError, match="runtime dependency is missing: missing"
    ):
        stage_dependencies(source, tmp_path / "staged")


def test_preserves_absent_optional_dependency(tmp_path: Path) -> None:
    """A dependency omitted from the actual installation must remain absent."""
    source = tmp_path / "source"
    server, _ = roots(source)
    metadata = json.loads((server / "package.json").read_text())
    metadata["optionalDependencies"] = {"uninstalled-platform-package": "1.0.0"}
    (server / "package.json").write_text(json.dumps(metadata))
    assert len(stage_dependencies(source, tmp_path / "staged")) == 2


def test_rejects_runtime_package_aliases(tmp_path: Path) -> None:
    """A flattened package name must remain the name resolved by its importer."""
    source = tmp_path / "source"
    server, _ = roots(source)
    metadata = json.loads((server / "package.json").read_text())
    metadata["dependencies"] = {"alias": "npm:actual@1.0.0"}
    (server / "package.json").write_text(json.dumps(metadata))
    package(source / "node_modules/alias", "actual", "1.0.0")
    with pytest.raises(ValueError, match="Aliased runtime packages"):
        stage_dependencies(source, tmp_path / "staged")


def test_rejects_bundled_dependencies_before_omitting_them(tmp_path: Path) -> None:
    """Bundled modules cannot disappear when nested node_modules directories are excluded."""
    source = tmp_path / "source"
    server, _ = roots(source)
    metadata = json.loads((server / "package.json").read_text())
    metadata["bundledDependencies"] = ["hidden"]
    (server / "package.json").write_text(json.dumps(metadata))
    with pytest.raises(ValueError, match="Bundled runtime dependencies"):
        stage_dependencies(source, tmp_path / "staged")
