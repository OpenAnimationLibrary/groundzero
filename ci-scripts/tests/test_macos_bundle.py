"""Qualify macOS deployment through fake native-tool process boundaries."""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import macos_bundle as bundling


def _record(path: Path) -> dict[str, Any]:
    return json.loads(path.read_bytes()[4:])


def _write(path: Path, record: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\xcf\xfa\xed\xfe" + json.dumps(record, sort_keys=True).encode())


def _otool(record: dict[str, Any]) -> str:
    blocks = [f"cmd LC_UUID\n uuid {record['uuid']}"]
    if record["identifier"] is not None:
        blocks.append(f"cmd LC_ID_DYLIB\n name {record['identifier']} (offset 24)")
    blocks.extend(
        f"cmd LC_LOAD_DYLIB\n name {name} (offset 24)"
        for name in record["dependencies"]
    )
    blocks.extend(
        f"cmd LC_RPATH\n path {name} (offset 12)" for name in record["rpaths"]
    )
    return "fixture:\n" + "\n".join(
        f"Load command {index}\n {block}\n" for index, block in enumerate(blocks)
    )


def _install_name(arguments: list[str], record: dict[str, Any]) -> None:
    record["signed"] = False
    position = 1
    while position < len(arguments) - 1:
        option, value = arguments[position : position + 2]
        if option == "-change":
            record["dependencies"] = [
                arguments[position + 2] if item == value else item
                for item in record["dependencies"]
            ]
            position += 1
        elif option == "-rpath":
            record["rpaths"] = [
                arguments[position + 2] if item == value else item
                for item in record["rpaths"]
            ]
            position += 1
        elif option == "-id":
            record["identifier"] = value
        elif option == "-delete_rpath":
            record["rpaths"].remove(value)
        elif option == "-add_rpath":
            if value in record["rpaths"]:
                raise AssertionError("install_name_tool rejects duplicate runpaths")
            record["rpaths"].append(value)
        else:
            raise AssertionError(f"Unexpected native option {option}")
        position += 2


class MacosBundleContracts(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.app = self.root / "App with spaces.app"
        self.main = self.app / "Contents" / "MacOS" / "OpenToonz"
        self.frameworks = self.app / "Contents" / "Frameworks"
        self.libraries = self.root / "brew" / "lib"
        self.libraries.mkdir(parents=True)
        self.image(self.main)
        (self.app / "portablestuff").mkdir()
        self.native = patch.object(
            bundling.subprocess, "run", side_effect=self.run_tool
        ).start()
        self.addCleanup(patch.stopall)

    def image(
        self,
        path: Path,
        *,
        dependencies: tuple[str, ...] = (),
        rpaths: tuple[str, ...] = (),
        identifier: str | None = None,
        architectures: tuple[str, ...] = ("x86_64",),
    ) -> Path:
        record = {
            "dependencies": list(dependencies),
            "rpaths": list(rpaths),
            "identifier": identifier,
            "uuid": hashlib.sha256(str(path).encode()).hexdigest()[:32],
            "architectures": list(architectures),
            "signed": True,
        }
        _write(path, record)
        return path

    def run_tool(
        self, arguments: list[str], **options: object
    ) -> subprocess.CompletedProcess[str]:
        self.assertTrue(options["check"])
        self.assertEqual(options["timeout"], 60)
        path = Path(arguments[-1])
        record = _record(path)
        if Path(arguments[0]).name == "otool":
            self.assertEqual(arguments[1], "-l")
            return subprocess.CompletedProcess(arguments, 0, _otool(record), "")
        if Path(arguments[0]).name == "lipo":
            self.assertEqual(arguments[1], "-archs")
            return subprocess.CompletedProcess(
                arguments, 0, " ".join(record["architectures"]), ""
            )
        if Path(arguments[0]).name == "codesign":
            if path == self.main:
                raise subprocess.CalledProcessError(
                    1, arguments, stderr="unsealed app root"
                )
            if arguments[1] == "--force":
                record["signed"] = True
                _write(path, record)
            elif not record["signed"]:
                raise subprocess.CalledProcessError(
                    1, arguments, stderr="invalid signature"
                )
            return subprocess.CompletedProcess(arguments, 0, "", "")
        self.assertEqual(Path(arguments[0]).name, "install_name_tool")
        _install_name(arguments, record)
        _write(path, record)
        return subprocess.CompletedProcess(arguments, 0, "", "")

    def test_recursive_copy_rewrites_helpers_and_preserves_original_files(self) -> None:
        sharp = self.image(
            self.libraries / "libsharpyuv.0.dylib",
            dependencies=("/usr/lib/libSystem.B.dylib",),
        )
        webp = self.image(
            self.libraries / "libwebp.7.dylib",
            dependencies=("@rpath/libsharpyuv.0.dylib",),
            rpaths=("@loader_path/../lib",),
        )
        outer = self.image(
            self.libraries / "libouter.dylib",
            dependencies=("@loader_path/libwebp.7.dylib",),
        )
        self.image(self.main, dependencies=(str(outer),), rpaths=(str(self.libraries),))
        helper = self.image(self.main.parent / "tcomposer", dependencies=(str(webp),))
        module = self.image(
            self.app / "Contents" / "Resources" / "sdk" / "module.so",
            dependencies=(str(webp),),
        )
        originals = {path: path.read_bytes() for path in (outer, webp, sharp)}
        self.assertEqual(bundling.deploy_bundle(self.app, (self.libraries,)), 6)
        self.assertEqual(bundling.verify_bundle(self.app), 6)
        self.assertEqual(
            _record(self.main)["rpaths"], ["@executable_path/../Frameworks"]
        )
        self.assertEqual(
            _record(helper)["dependencies"],
            ["@loader_path/../Frameworks/libwebp.7.dylib"],
        )
        self.assertEqual(
            _record(module)["dependencies"],
            ["@loader_path/../../Frameworks/libwebp.7.dylib"],
        )
        self.assertEqual(
            _record(self.frameworks / webp.name)["dependencies"],
            ["@loader_path/libsharpyuv.0.dylib"],
        )
        for path, original in originals.items():
            self.assertEqual(path.read_bytes(), original)
        moved = self.root / "Installed elsewhere.app"
        self.app.rename(moved)
        self.assertEqual(bundling.verify_bundle(moved), 6)

    def test_missing_transitive_dependency_fails_before_path_rewrites(self) -> None:
        source = self.image(
            self.libraries / "libwebp.7.dylib",
            dependencies=("@rpath/libmissing.dylib",),
        )
        self.image(self.main, dependencies=(str(source),))
        original = self.main.read_bytes()
        with self.assertRaisesRegex(RuntimeError, "Unresolved dependency"):
            bundling.deploy_bundle(self.app, (self.libraries,))
        self.assertEqual(self.main.read_bytes(), original)

    def test_ambient_libraries_and_dyld_environment_cannot_pass_verification(
        self,
    ) -> None:
        source = self.image(self.libraries / "libambient.dylib")
        self.image(
            self.main,
            dependencies=(str(source),),
            rpaths=("@executable_path/../Frameworks",),
        )
        with patch.dict(os.environ, {"DYLD_LIBRARY_PATH": str(self.libraries)}):
            with self.assertRaisesRegex(
                RuntimeError, "Nonportable absolute dependency"
            ):
                bundling.verify_bundle(self.app)
        self.image(
            self.main,
            dependencies=("@rpath/libambient.dylib",),
            rpaths=("@executable_path/../Frameworks",),
        )
        with self.assertRaisesRegex(RuntimeError, "Unresolved dependency"):
            bundling.verify_bundle(self.app)

    def test_conflicting_library_roots_and_destination_names_reject(self) -> None:
        first = self.image(self.libraries / "libsame.dylib")
        other = self.root / "other"
        self.image(other / first.name)
        self.image(self.main, dependencies=("@rpath/libsame.dylib",))
        with self.assertRaisesRegex(RuntimeError, "Conflicting dependency candidates"):
            bundling.deploy_bundle(self.app, (self.libraries, other))
        self.image(self.main, dependencies=(str(first),))
        self.image(self.frameworks / first.name)
        with self.assertRaisesRegex(RuntimeError, "Conflicting bundled dependency"):
            bundling.deploy_bundle(self.app, (self.libraries,))

    def test_framework_layout_and_internal_symlinks_survive_relocation(self) -> None:
        framework = self.libraries / "Fixture.framework"
        library = self.image(
            framework / "Versions" / "A" / "Fixture",
            dependencies=(
                "/System/Library/PrivateFrameworks/Fixture.framework/Fixture",
            ),
        )
        (framework / "Versions" / "Current").symlink_to("A", target_is_directory=True)
        (framework / "Fixture").symlink_to("Versions/Current/Fixture")
        self.image(self.main, dependencies=(str(library),))
        self.assertEqual(bundling.deploy_bundle(self.app, (self.libraries,)), 2)
        copied = self.frameworks / "Fixture.framework"
        self.assertTrue((copied / "Fixture").is_symlink())
        self.assertEqual(
            _record(self.main)["dependencies"],
            ["@loader_path/../Frameworks/Fixture.framework/Versions/A/Fixture"],
        )
        self.assertEqual(bundling.verify_bundle(self.app), 2)

    def test_nested_helpers_frameworks_and_plugins_get_direct_paths(self) -> None:
        leaf = self.image(self.libraries / "libnested.dylib")
        framework = self.image(
            self.libraries / "Nested.framework" / "Versions" / "A" / "Nested",
            dependencies=("@rpath/libnested.dylib",),
            rpaths=(str(self.libraries),),
        )
        helper = self.image(
            self.main.parent / "Helpers" / "nested-helper",
            dependencies=("@rpath/Nested.framework/Versions/A/Nested",),
            rpaths=(str(self.libraries),),
        )
        plugin = self.image(
            self.app / "Contents" / "PlugIns" / "imageformats" / "plugin.dylib",
            dependencies=("@rpath/libnested.dylib",),
            rpaths=("@executable_path/../Frameworks",),
        )
        self.assertEqual(bundling.deploy_bundle(self.app, (self.libraries,)), 5)
        self.assertEqual(
            _record(helper)["dependencies"],
            ["@loader_path/../../Frameworks/Nested.framework/Versions/A/Nested"],
        )
        self.assertEqual(
            _record(plugin)["dependencies"],
            ["@loader_path/../../Frameworks/libnested.dylib"],
        )
        copied = self.frameworks / framework.relative_to(self.libraries)
        self.assertEqual(
            _record(copied)["dependencies"], ["@loader_path/../../../libnested.dylib"]
        )
        self.assertEqual(_record(copied)["rpaths"], [])
        leaf.unlink()
        self.assertEqual(bundling.verify_bundle(self.app), 5)

    def test_arm_only_transitive_rejects_and_universal_dependency_passes(self) -> None:
        library = self.image(
            self.libraries / "libarchitecture.dylib", architectures=("arm64",)
        )
        outer = self.image(
            self.libraries / "libouter.dylib", dependencies=(str(library),)
        )
        self.image(self.main, dependencies=(str(outer),))
        original = self.main.read_bytes()
        with self.assertRaisesRegex(
            RuntimeError, "Missing application architectures.*x86_64"
        ):
            bundling.deploy_bundle(self.app, (self.libraries,))
        self.assertEqual(self.main.read_bytes(), original)
        self.assertFalse((self.frameworks / library.name).exists())
        self.image(library, architectures=("x86_64", "arm64"))
        self.assertEqual(bundling.deploy_bundle(self.app, (self.libraries,)), 3)
        self.assertEqual(bundling.verify_bundle(self.app), 3)
        self.image(self.frameworks / library.name, architectures=("arm64",))
        with self.assertRaisesRegex(
            RuntimeError, "Missing application architectures.*x86_64"
        ):
            bundling.verify_bundle(self.app)
        self.image(self.main, dependencies=(str(library),))
        original = self.main.read_bytes()
        with self.assertRaisesRegex(
            RuntimeError, "Missing application architectures.*x86_64"
        ):
            bundling.deploy_bundle(self.app, (self.libraries,))
        self.assertEqual(self.main.read_bytes(), original)

    def test_signature_repair_preserves_mode_and_verification_keeps_app_unchanged(
        self,
    ) -> None:
        library = self.image(self.libraries / "libsignature.dylib")
        self.image(self.main, dependencies=(str(library),))
        self.main.chmod(0o755)
        self.assertEqual(bundling.deploy_bundle(self.app, (self.libraries,)), 2)
        original = self.main.read_bytes()
        self.assertEqual(self.main.stat().st_mode & 0o777, 0o755)
        self.assertTrue(_record(self.main)["signed"])
        self.assertEqual(bundling.verify_bundle(self.app), 2)
        self.assertEqual(self.main.read_bytes(), original)
        record = _record(self.main)
        record["signed"] = False
        _write(self.main, record)
        with self.assertRaisesRegex(RuntimeError, "invalid signature"):
            bundling.verify_bundle(self.app)

    def test_escaping_symlink_and_absolute_bundle_runpath_reject(self) -> None:
        outside = self.image(self.libraries / "liboutside.dylib")
        self.frameworks.mkdir(parents=True)
        link = self.frameworks / outside.name
        link.symlink_to(outside)
        with self.assertRaisesRegex(RuntimeError, "symlink escapes"):
            bundling.verify_bundle(self.app)
        link.unlink()
        self.image(self.main, rpaths=(str(self.frameworks),))
        with self.assertRaisesRegex(RuntimeError, "Ambient runpath"):
            bundling.verify_bundle(self.app)
        bundling.deploy_bundle(self.app, (self.libraries,))
        self.assertEqual(bundling.verify_bundle(self.app), 1)

    def test_only_apple_system_paths_are_allowed_without_files(self) -> None:
        self.image(
            self.main,
            dependencies=(
                "/usr/lib/libSystem.B.dylib",
                "/System/Library/PrivateFrameworks/Fixture.framework/Fixture",
            ),
            rpaths=("@executable_path/../Frameworks", "/usr/lib", "/usr/lib/swift"),
        )
        self.assertEqual(bundling.verify_bundle(self.app), 1)
        self.image(
            self.main,
            dependencies=("/System/Library/../../usr/local/lib/ambient.dylib",),
            rpaths=("@executable_path/../Frameworks",),
        )
        with self.assertRaisesRegex(RuntimeError, "Nonportable absolute dependency"):
            bundling.verify_bundle(self.app)

    def test_universal_commands_keep_all_loads_and_reject_conflicting_ids(self) -> None:
        first = _record(self.main)
        first["identifier"] = "@rpath/libfixture.dylib"
        first["dependencies"] = ["@rpath/library with spaces.dylib"]
        second = {
            **first,
            "uuid": "BBBB-CCCC",
            "dependencies": ["/usr/lib/libSystem.B.dylib"],
        }
        output = _otool(first) + _otool(second).replace(
            "LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB"
        )
        image = bundling.parse_image(output)
        self.assertEqual(len(image.uuids), 2)
        self.assertEqual(
            image.dependencies,
            ("@rpath/library with spaces.dylib", "/usr/lib/libSystem.B.dylib"),
        )
        second["identifier"] = "@rpath/different.dylib"
        with self.assertRaisesRegex(RuntimeError, "different install names"):
            bundling.parse_image(_otool(first) + _otool(second))

    def test_malformed_tool_output_and_native_tool_failures_are_actionable(
        self,
    ) -> None:
        with self.assertRaisesRegex(RuntimeError, "Malformed Mach-O name"):
            bundling.parse_image("Load command 0\n cmd LC_LOAD_DYLIB\n missing-name\n")
        with patch.object(
            bundling.subprocess,
            "run",
            side_effect=subprocess.CalledProcessError(
                1, ["otool"], stderr="native diagnostic"
            ),
        ):
            with self.assertRaisesRegex(RuntimeError, "native diagnostic"):
                bundling.verify_bundle(self.app)


if __name__ == "__main__":
    unittest.main()
