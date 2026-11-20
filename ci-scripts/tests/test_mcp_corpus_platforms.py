"""Prove per-platform corpus baselines stay within their stated rule and are never skipped."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mcp_corpus_build as build
import mcp_corpus_compare as corpus
from test_mcp_corpus import make_corpus, picture, png

EDGE = 5  # x 1, y 1: no probe reads it
SECOND_EDGE = 6
REFERENCE = "macos-arm64-apple-m1"
PLATFORM = {"os": "linux", "cpu_architecture": "x86_64", "renderer": "llvmpipe (LLVM 15)"}
IDENTITY = "linux-x86_64-llvmpipe-llvm-15"
RULE = {"pixels_per_frame": 1, "alpha": 1, "why": "GL renderers round edge coverage differently"}


def frames(root: Path, edge_alpha: int, change: dict[int, tuple[int, int, int, int]]) -> None:
    """Write two frames like the reference, with the edge pixel's alpha and any changes."""
    root.mkdir(parents=True, exist_ok=True)
    for frame in range(2):
        pixels = picture()
        pixels[1] = (110, 20, 30, 255) if frame else pixels[1]
        pixels[EDGE] = (10, 20, 30, edge_alpha)
        pixels[SECOND_EDGE] = (10, 20, 30, 64)
        for index, value in change.items():
            pixels[index] = value
        png(root / f"frame.{frame + 1:04d}.png", pixels, 4)


class PlatformBaselines(unittest.TestCase):
    def setUp(self) -> None:
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name) / "corpus"
        self.root.mkdir()
        manifest = make_corpus(self.root)
        frames(self.root / "baselines" / "one", 128, {})
        scene = manifest["scenes"]["one"]
        scene["baseline"]["files"] = corpus.baseline_files(self.root, "one")
        manifest["history"][-1]["baseline_set_sha256"] = corpus.baseline_set(manifest)
        (self.root / "manifest.json").write_text(corpus.dump_manifest(manifest))
        environment = mock.patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        os.environ.pop("CI", None)
        build.mark_renderer_dependent("one", dict(RULE), REFERENCE, self.root)
        self.frames = Path(folder.name) / "frames"

    def record(self, edge_alpha: int = 129, change: dict | None = None) -> str:
        frames(self.frames, edge_alpha, change or {})
        provenance = {**PLATFORM, "source": "run 1", "reason": "frames from the Linux CI runner"}
        return build.record_platform("one", self.frames, provenance, self.root)

    def test_an_edge_alpha_off_by_one_is_recorded_and_compared_exactly(self) -> None:
        self.assertEqual(self.record(), IDENTITY)
        self.assertEqual(corpus.check(self.root), [])
        manifest = corpus.load_manifest(self.root / "manifest.json")
        rendered = sorted(self.frames.glob("*.png"))
        self.assertEqual(corpus.compare_scene(manifest, "one", rendered, self.root, IDENTITY), [])
        found = corpus.compare_scene(manifest, "one", rendered, self.root, REFERENCE)
        self.assertTrue(any("alpha differs by 1" in problem for problem in found), found)

    def test_an_opaque_pixel_that_differs_is_refused(self) -> None:
        with self.assertRaisesRegex(SystemExit, "where the reference is opaque"):
            self.record(change={10: (10, 20, 30, 254)})
        self.assertFalse(corpus.platform_folder(self.root, IDENTITY, "one").exists())
        self.assertEqual(corpus.check(self.root), [])

    def test_an_edge_alpha_off_by_two_is_refused(self) -> None:
        with self.assertRaisesRegex(SystemExit, "alpha differs by 2"):
            self.record(edge_alpha=130)

    def test_a_colour_change_is_refused(self) -> None:
        with self.assertRaisesRegex(SystemExit, "colour differs"):
            self.record(change={EDGE: (11, 20, 30, 129)})

    def test_more_differing_pixels_than_the_rule_allows_is_refused(self) -> None:
        with self.assertRaisesRegex(SystemExit, "2 pixels differ, more than 1"):
            self.record(change={SECOND_EDGE: (10, 20, 30, 65)})

    def test_a_renderer_without_a_baseline_fails_and_names_it(self) -> None:
        manifest = corpus.load_manifest(self.root / "manifest.json")
        frames(self.frames, 128, {})
        rendered = sorted(self.frames.glob("*.png"))
        found = corpus.compare_scene(manifest, "one", rendered, self.root, "windows-x86_64-mesa")
        self.assertEqual(len(found), 1)
        self.assertIn("no baseline for renderer windows-x86_64-mesa", found[0])
        found = corpus.compare_scene(manifest, "one", rendered, self.root, None)
        self.assertIn("depend on the GL renderer", found[0])

    def test_a_platform_frame_changed_on_disk_fails_the_check(self) -> None:
        self.record()
        folder = corpus.platform_folder(self.root, IDENTITY, "one")
        frames(folder, 129, {EDGE: (10, 20, 30, 127)})
        self.assertTrue(any("differ from the manifest" in p for p in corpus.check(self.root)))

    def test_a_widened_rule_is_refused(self) -> None:
        manifest: dict[str, Any] = corpus.load_manifest(self.root / "manifest.json")
        manifest["scenes"]["one"]["cross_platform"]["alpha"] = 2
        (self.root / "manifest.json").write_text(corpus.dump_manifest(manifest))
        self.assertTrue(any("alpha must be 0 to 1" in p for p in corpus.check(self.root)))

    def test_recording_is_refused_in_ci(self) -> None:
        with mock.patch.dict(os.environ, {"CI": "true"}):
            with self.assertRaisesRegex(SystemExit, "never regenerated in CI"):
                self.record()


if __name__ == "__main__":
    unittest.main()
