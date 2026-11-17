"""Check portable dependency staging through real temporary package trees."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

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


class RuntimeStagingTests(unittest.TestCase):
    """Check the dependency closure copied from installed package trees."""

    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.source = self.root / "source"
        self.destination = self.root / "staged"

    def test_stages_runtime_closure_with_dependency_cycle(self) -> None:
        """A dependency cycle must copy each installed package exactly once."""
        server, _ = roots(self.source)
        metadata = json.loads((server / "package.json").read_text())
        metadata["dependencies"] = {"cycle-a": "1.0.0"}
        (server / "package.json").write_text(json.dumps(metadata))
        package(self.source / "node_modules/cycle-a", "cycle-a", "1.0.0", {"cycle-b": "1.0.0"})
        package(self.source / "node_modules/cycle-b", "cycle-b", "1.0.0", {"cycle-a": "1.0.0"})
        identities = stage_dependencies(self.source, self.destination)
        self.assertEqual(len(identities), 4)
        for identity in identities:
            copied = self.destination / "node_modules" / identity["name"]
            self.assertEqual(
                (copied / "index.mjs").read_text(),
                f'export const version = "{identity["version"]}";\n',
            )
            self.assertFalse((copied / "node_modules").exists())

    def _reject_nested(self, other_version: str, message: str) -> None:
        server, client = roots(self.source)
        for parent, version in ((server, "1.0.0"), (client, other_version)):
            metadata = json.loads((parent / "package.json").read_text())
            metadata["dependencies"] = {"shared": version}
            (parent / "package.json").write_text(json.dumps(metadata))
            package(parent / "node_modules/shared", "shared", version)
        with self.assertRaisesRegex(ValueError, message):
            stage_dependencies(self.source, self.destination)

    def test_rejects_conflicting_nested_versions(self) -> None:
        """Flattening must not select a different installed dependency version."""
        self._reject_nested("2.0.0", "conflicting installed versions")

    def test_rejects_distinct_nested_contexts(self) -> None:
        """Equal package versions must not hide distinct installed dependency contexts."""
        self._reject_nested("1.0.0", "distinct installed copies")

    def test_rejects_missing_required_dependency(self) -> None:
        """An incomplete installed tree cannot produce a prepared result."""
        server, _ = roots(self.source)
        metadata = json.loads((server / "package.json").read_text())
        metadata["dependencies"] = {"missing": "1.0.0"}
        (server / "package.json").write_text(json.dumps(metadata))
        with self.assertRaisesRegex(FileNotFoundError, "runtime dependency is missing: missing"):
            stage_dependencies(self.source, self.destination)

    def test_preserves_absent_optional_dependency(self) -> None:
        """A dependency omitted from the actual installation must remain absent."""
        server, _ = roots(self.source)
        metadata = json.loads((server / "package.json").read_text())
        metadata["optionalDependencies"] = {"uninstalled-platform-package": "1.0.0"}
        (server / "package.json").write_text(json.dumps(metadata))
        self.assertEqual(len(stage_dependencies(self.source, self.destination)), 2)

    def test_rejects_runtime_package_aliases(self) -> None:
        """A flattened package name must remain the name resolved by its importer."""
        server, _ = roots(self.source)
        metadata = json.loads((server / "package.json").read_text())
        metadata["dependencies"] = {"alias": "npm:actual@1.0.0"}
        (server / "package.json").write_text(json.dumps(metadata))
        package(self.source / "node_modules/alias", "actual", "1.0.0")
        with self.assertRaisesRegex(ValueError, "Aliased runtime packages"):
            stage_dependencies(self.source, self.destination)

    def test_rejects_bundled_dependencies_before_omitting_them(self) -> None:
        """Bundled modules cannot disappear when nested modules are excluded."""
        server, _ = roots(self.source)
        metadata = json.loads((server / "package.json").read_text())
        metadata["bundledDependencies"] = ["hidden"]
        (server / "package.json").write_text(json.dumps(metadata))
        with self.assertRaisesRegex(ValueError, "Bundled runtime dependencies"):
            stage_dependencies(self.source, self.destination)
