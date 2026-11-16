"""Filesystem and subprocess boundary contracts for CI media staging."""

from __future__ import annotations

import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_stage_media as staging


class MediaStagingContracts(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / "source"
        self.output = self.root / "media tools"
        self.report = self.root / "media-report.json"
        self.environment = self.root / "github.env"
        self.environment.write_text("EXISTING=value\n", encoding="utf-8")

    def pair(self, directory: Path, *, windows: bool = False) -> dict[str, str]:
        directory.mkdir(parents=True, exist_ok=True)
        result = {}
        for tool in ("ffmpeg", "ffprobe"):
            name = tool + (".exe" if windows else "")
            path = directory / name
            path.write_bytes(f"fixture {tool} from {directory.relative_to(self.root)}\n".encode())
            path.chmod(0o700)
            result[name] = str(path)
        return result

    def version(self, command: list[str], **options: object) -> subprocess.CompletedProcess[str]:
        self.assertEqual(command[1:], ["-version"])
        self.assertEqual(options["timeout"], 20)
        tool = Path(command[0]).stem
        return subprocess.CompletedProcess(command, 0, f"{tool} version fixture\n", "")

    def assert_unpublished(self) -> None:
        self.assertFalse(self.report.exists())
        self.assertEqual(self.environment.read_text(encoding="utf-8"), "EXISTING=value\n")

    def assert_record(self, record: dict[str, str]) -> None:
        source = Path(record["source"])
        destination = Path(record["path"])
        self.assertEqual(source, source.resolve(strict=True))
        self.assertEqual(destination, destination.resolve(strict=True))
        self.assertEqual(destination.parent, self.output.resolve(strict=True))
        self.assertFalse(destination.is_symlink())
        self.assertEqual(destination.read_bytes(), source.read_bytes())
        self.assertEqual(record["sha256"], hashlib.sha256(destination.read_bytes()).hexdigest())

    def test_canonical_pair_manifest_hashes_and_environment_exports(self) -> None:
        paths = self.pair(self.source)
        lookup = {name: str(self.source / ".." / "source" / name) for name in paths}
        with patch.object(staging.shutil, "which", side_effect=lookup.get):
            with patch.object(staging.subprocess, "run", side_effect=self.version) as run:
                report = staging.stage_media(
                    self.output, self.report, self.environment, windows=False
                )
        self.assertEqual(report, json.loads(self.report.read_text(encoding="utf-8")))
        self.assertEqual(report["directory"], str(self.output.resolve(strict=True)))
        self.assertEqual(set(report["tools"]), {"ffmpeg", "ffprobe"})
        self.assertEqual(report["libraries"], [])
        for name, record in report["tools"].items():
            self.assert_record(record)
            self.assertEqual(record["version"], f"{name} version fixture")
        self.assertEqual(
            [Path(call.args[0][0]).parent for call in run.call_args_list],
            [self.output.resolve(), self.output.resolve()],
        )
        self.assertEqual(
            self.environment.read_text(encoding="utf-8").splitlines(),
            [
                "EXISTING=value",
                f"MCP_MEDIA_TOOLS={self.output}",
                f"GROUNDZERO_MEDIA_TOOLS={self.output}",
            ],
        )

    def test_chocolatey_shims_resolve_the_real_pair_and_copy_runtime_dlls(self) -> None:
        chocolatey = self.root / "chocolatey"
        paths = self.pair(chocolatey / "bin", windows=True)
        source = chocolatey / "lib" / "ffmpeg" / "tools" / "release" / "bin"
        self.pair(source, windows=True)
        for name in ("avcodec.dll", "avutil.DLL", "license.txt"):
            (source / name).write_bytes(name.encode())
        with patch.dict(os.environ, {"ChocolateyInstall": str(chocolatey)}):
            with patch.object(staging.shutil, "which", side_effect=paths.get):
                discovered = staging.discover_tools(windows=True)
                self.assertEqual(
                    discovered, {name: source / f"{name}.exe" for name in ("ffmpeg", "ffprobe")}
                )
                with patch.object(staging.subprocess, "run", side_effect=self.version):
                    report = staging.stage_media(
                        self.output, self.report, self.environment, windows=True
                    )
        self.assertEqual(
            {path.name for path in self.output.iterdir()},
            {"ffmpeg.exe", "ffprobe.exe", "avcodec.dll", "avutil.DLL"},
        )
        self.assertEqual(len(report["libraries"]), 2)
        self.assertNotEqual(
            (self.output / "ffmpeg.exe").read_bytes(), Path(paths["ffmpeg.exe"]).read_bytes()
        )
        for record in [*report["tools"].values(), *report["libraries"]]:
            self.assert_record(record)
            self.assertEqual(Path(record["source"]).parent, source)

    def test_missing_pair_never_executes_a_tool_or_exports_a_directory(self) -> None:
        paths = self.pair(self.source)
        paths.pop("ffprobe")
        with patch.object(staging.shutil, "which", side_effect=paths.get):
            with patch.object(staging.subprocess, "run") as run:
                with self.assertRaises((OSError, ValueError, RuntimeError)):
                    staging.stage_media(self.output, self.report, self.environment, windows=False)
        run.assert_not_called()
        self.assert_unpublished()

    def test_pair_from_different_installations_is_rejected(self) -> None:
        paths = self.pair(self.source)
        paths["ffprobe"] = self.pair(self.root / "another")["ffprobe"]
        with patch.object(staging.shutil, "which", side_effect=paths.get):
            with self.assertRaises((OSError, ValueError, RuntimeError)):
                staging.discover_tools(windows=False)

    def test_chocolatey_requires_exactly_one_real_pair(self) -> None:
        chocolatey = self.root / "chocolatey"
        paths = self.pair(chocolatey / "bin", windows=True)
        with patch.dict(os.environ, {"ChocolateyInstall": str(chocolatey)}):
            with patch.object(staging.shutil, "which", side_effect=paths.get):
                with self.assertRaises((OSError, ValueError, RuntimeError)):
                    staging.discover_tools(windows=True)
                for version in ("first", "second"):
                    self.pair(chocolatey / "lib" / "ffmpeg" / "tools" / version, windows=True)
                with self.assertRaises((OSError, ValueError, RuntimeError)):
                    staging.discover_tools(windows=True)

    def test_failed_version_checks_never_publish_partial_success(self) -> None:
        paths = self.pair(self.source)
        failures = (
            subprocess.CompletedProcess([], 1, "ffprobe version fixture\n", "failed"),
            subprocess.CompletedProcess([], 0, "unexpected executable\n", ""),
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 0, "ffprobe version fixture\n" + "x" * 65536, ""),
            subprocess.TimeoutExpired(["ffprobe", "-version"], 20),
            OSError("Cannot launch the staged executable"),
        )
        for index, failure in enumerate(failures):
            with self.subTest(case=index, failure=type(failure).__name__):
                self.output = self.root / f"failed-stage-{index}"
                first = subprocess.CompletedProcess([], 0, "ffmpeg version fixture\n", "")
                with patch.object(staging.shutil, "which", side_effect=paths.get):
                    with patch.object(staging.subprocess, "run", side_effect=[first, failure]):
                        with self.assertRaises(
                            (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired)
                        ):
                            staging.stage_media(
                                self.output, self.report, self.environment, windows=False
                            )
                self.assert_unpublished()

    def test_existing_output_is_preserved_and_not_reused(self) -> None:
        paths = self.pair(self.source)
        self.output.mkdir()
        marker = self.output / "existing"
        marker.write_bytes(b"preserve")
        with patch.object(staging.shutil, "which", side_effect=paths.get):
            with patch.object(staging.subprocess, "run") as run:
                with self.assertRaises((OSError, ValueError, RuntimeError)):
                    staging.stage_media(self.output, self.report, self.environment, windows=False)
        run.assert_not_called()
        self.assertEqual(marker.read_bytes(), b"preserve")
        self.assert_unpublished()

    def test_newline_path_cannot_inject_an_environment_export(self) -> None:
        paths = self.pair(self.source)
        self.output = self.root / "invalid\nINJECTED=value"
        with patch.object(staging.shutil, "which", side_effect=paths.get):
            with patch.object(staging.subprocess, "run", side_effect=self.version):
                with self.assertRaises((OSError, ValueError, RuntimeError)):
                    staging.stage_media(self.output, self.report, self.environment, windows=False)
        self.assert_unpublished()

    @unittest.skipIf(os.name == "nt", "Creating filesystem symlinks requires Windows privileges")
    def test_path_symlinks_stage_regular_files_from_the_resolved_pair(self) -> None:
        sources = self.pair(self.source)
        aliases = self.root / "aliases"
        aliases.mkdir()
        paths = {}
        for name, source in sources.items():
            versioned = self.source / f"{name}-fixture"
            Path(source).rename(versioned)
            alias = aliases / name
            alias.symlink_to(versioned)
            paths[name] = str(alias)
        with patch.object(staging.shutil, "which", side_effect=paths.get):
            with patch.object(staging.subprocess, "run", side_effect=self.version):
                report = staging.stage_media(
                    self.output, self.report, self.environment, windows=False
                )
        for record in report["tools"].values():
            self.assert_record(record)
            self.assertEqual(Path(record["source"]).parent, self.source)
        self.assertEqual({path.name for path in self.output.iterdir()}, {"ffmpeg", "ffprobe"})

    def test_copy_failure_does_not_publish_an_unusable_directory(self) -> None:
        paths = self.pair(self.source)
        with patch.object(staging.shutil, "which", side_effect=paths.get):
            with patch.object(staging.shutil, "copy2", side_effect=PermissionError("copy denied")):
                with patch.object(staging.subprocess, "run") as run:
                    with self.assertRaises(PermissionError):
                        staging.stage_media(
                            self.output, self.report, self.environment, windows=False
                        )
        run.assert_not_called()
        self.assert_unpublished()

    def test_cli_reports_discovery_failure_with_nonzero_status(self) -> None:
        arguments = [
            "mcp_stage_media.py",
            "--output",
            str(self.output),
            "--report",
            str(self.report),
            "--github-env",
            str(self.environment),
        ]
        captured = io.StringIO()
        with patch.object(sys, "argv", arguments), redirect_stdout(captured):
            with patch.object(staging.shutil, "which", return_value=None):
                self.assertEqual(staging.main(), 1)
        self.assertIn("::error title=MCP media-tools staging::", captured.getvalue())
        self.assertIn("ffmpeg", captured.getvalue())
        self.assert_unpublished()


if __name__ == "__main__":
    unittest.main()
