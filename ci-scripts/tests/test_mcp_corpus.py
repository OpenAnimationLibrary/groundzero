"""Prove the corpus check and the image comparison fail for each way they could be fooled."""

from __future__ import annotations

import copy
import os
import shutil
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from typing import Any
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_corpus_build as build
import mcp_corpus_compare as corpus
import mcp_native_oracle as oracle

Pixel = tuple[int, int, int, int]


def png(path: Path, pixels: list[Pixel], width: int) -> None:
    height = len(pixels) // width
    rows = b"".join(
        b"\x00" + b"".join(bytes(pixel) for pixel in pixels[row * width : (row + 1) * width])
        for row in range(height)
    )

    def chunk(kind: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        )

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )


def picture(width: int = 4, height: int = 4, base: Pixel = (10, 20, 30, 255)) -> list[Pixel]:
    return [base] * (width * height)


def big() -> list[Pixel]:
    return picture(20, 20)


class CompareTest(unittest.TestCase):
    def decoded(self, pixels: list[Pixel]) -> tuple[int, int, list[Pixel]]:
        return 20, len(pixels) // 20, pixels

    def test_equal_images_are_equal_under_the_exact_policy(self) -> None:
        image = self.decoded(big())
        self.assertTrue(oracle.compare_images(image, image).equal)

    def test_one_code_value_is_a_difference_when_exact(self) -> None:
        other = big()
        other[5] = (11, 20, 30, 255)
        found = oracle.compare_images(self.decoded(big()), self.decoded(other))
        self.assertFalse(found.equal)
        self.assertEqual(
            (found.differing, found.max_color, found.first, found.box), (1, 1, (5, 0), (5, 0, 5, 0))
        )

    def test_a_stated_tolerance_passes_a_small_color_difference_only(self) -> None:
        other = big()
        other[0] = (12, 20, 30, 255)
        policy = oracle.Policy(color=2, pixels=0.05, why="rounding of a blend")
        self.assertTrue(
            oracle.compare_images(self.decoded(big()), self.decoded(other), policy).equal
        )
        other[0] = (14, 20, 30, 255)
        self.assertFalse(
            oracle.compare_images(self.decoded(big()), self.decoded(other), policy).equal
        )

    def test_a_color_tolerance_never_covers_alpha(self) -> None:
        other = big()
        other[3] = (10, 20, 30, 254)
        policy = oracle.Policy(color=4, pixels=0.05, why="rounding")
        found = oracle.compare_images(self.decoded(big()), self.decoded(other), policy)
        self.assertFalse(found.equal)
        self.assertIn("alpha differs by 1", found.problems[0])

    def test_a_tolerance_cannot_hide_a_moved_edge(self) -> None:
        want = big()
        got = big()
        for index in (4, 5, 6, 7):
            got[index] = (0, 0, 0, 0)
        policy = oracle.Policy(color=4, alpha=2, pixels=0.05, why="rounding")
        found = oracle.compare_images(self.decoded(want), self.decoded(got), policy)
        self.assertFalse(found.equal)

    def test_a_different_size_fails(self) -> None:
        found = oracle.compare_images(self.decoded(big()), (20, 3, picture(20, 3)))
        self.assertIn("size 20x3", found.problems[0])

    def test_a_tolerance_needs_a_reason_and_a_bound(self) -> None:
        self.assertIn("needs its reason", oracle.Policy(color=1).problems()[0])
        self.assertTrue(oracle.Policy(color=9, why="x").problems())
        self.assertTrue(oracle.Policy(alpha=3, why="x").problems())
        self.assertTrue(oracle.Policy(pixels=0.5, why="x").problems())
        self.assertEqual(oracle.Policy().problems(), [])
        self.assertTrue(oracle.Policy().exact)

    def test_a_missing_or_broken_file_is_an_error_not_a_pass(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            good = Path(directory) / "good.png"
            png(good, picture(), 4)
            with self.assertRaises(oracle.OracleError):
                oracle.compare_files(good, Path(directory) / "missing.png")
            broken = Path(directory) / "broken.png"
            broken.write_bytes(b"not a png")
            with self.assertRaises(oracle.OracleError):
                oracle.compare_files(good, broken)


EFFECTS = ["aFx", "bFx", "cFx", "dFx", "eFx", "fFx"]
SCENE_TEXT = (
    "scene "
    + "".join(f"<{tag} id='{n}'> " for n, tag in enumerate(EFFECTS))
    + "<Toonz_columnFx id='9'>"
)
PROBES = [
    {"kind": "vector", "what": "a stroke", "x": 0, "y": 0, "frame": 0, "rgba": [10, 20, 30, 255]},
    {
        "kind": "toonz_raster",
        "what": "a block",
        "x": 0,
        "y": 1,
        "frame": 0,
        "rgba": [10, 20, 30, 255],
    },
    {"kind": "png", "what": "art", "x": 2, "y": 2, "frame": 1, "rgba": [10, 20, 30, 255]},
    {"kind": "effect", "what": "a key", "x": 1, "y": 0, "frames": [0, 1], "min_difference": 50},
]


COMPARISONS: dict[str, Any] = {
    "bridge_disabled": {
        "status": "compared",
        "cases": ["corpus_baselines_are_reproduced_by_this_builds_batch_renderer"],
    },
    "bridge_enabled": {"status": "not_built", "reason": "the live bridge cannot open a scene " * 2},
    "assisted_save_reopen": {"status": "not_built", "reason": "an editable import is missing " * 2},
}


def make_corpus(root: Path) -> dict[str, Any]:
    """A one-scene corpus with a baseline set and a history entry, on disk."""
    scene = root / "one"
    (scene / "scenes").mkdir(parents=True)
    (scene / "scenes" / "shot.tnz").write_text(SCENE_TEXT)
    (root / "baselines" / "one").mkdir(parents=True)
    for frame in range(2):
        pixels = picture()
        pixels[1] = (110, 20, 30, 255) if frame else pixels[1]
        png(root / "baselines" / "one" / f"frame.{frame + 1:04d}.png", pixels, 4)
    manifest: dict[str, Any] = {
        "schema_version": 1,
        "provenance": {"kind": "generated_by_script", "note": "x", "human_review": "none"},
        "frames": {"first": 0, "last": 1, "width": 4, "height": 4},
        "scenes": {
            "one": {
                "expected": {"levels": 1},
                "files": corpus.scene_files(root, "one"),
                "policy": {"color": 0, "alpha": 0, "pixels": 0.0, "why": ""},
                "effects": EFFECTS,
                "probes": PROBES,
                "baseline": {"files": corpus.baseline_files(root, "one")},
            }
        },
        "comparisons": copy.deepcopy(COMPARISONS),
        "history": [],
    }
    entry = manifest["scenes"]["one"]
    entry["scene_sha256"] = corpus.digest(entry["files"])
    entry["baseline"]["rendered_from"] = entry["scene_sha256"]
    manifest["history"].append(
        {
            "when": "now",
            "reason": "the first baselines, rendered for the test",
            "baseline_set_sha256": corpus.baseline_set(manifest),
        }
    )
    (root / "manifest.json").write_text(corpus.dump_manifest(manifest))
    return manifest


class CheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.manifest = make_corpus(self.root)

    def rewrite(self, manifest: dict[str, Any]) -> None:
        (self.root / "manifest.json").write_text(corpus.dump_manifest(manifest))

    def test_a_consistent_corpus_passes(self) -> None:
        self.assertEqual(corpus.check(self.root), [])

    def test_the_committed_corpus_passes(self) -> None:
        if corpus.MANIFEST.is_file():
            self.assertEqual(corpus.check(), [])

    def test_a_changed_baseline_fails(self) -> None:
        png(self.root / "baselines" / "one" / "frame.0001.png", picture(base=(10, 20, 31, 255)), 4)
        self.assertTrue(any("changed without regeneration" in p for p in corpus.check(self.root)))

    def test_a_replaced_baseline_set_fails_even_if_the_manifest_is_updated_with_it(self) -> None:
        png(self.root / "baselines" / "one" / "frame.0001.png", picture(base=(1, 2, 3, 255)), 4)
        manifest = copy.deepcopy(self.manifest)
        manifest["scenes"]["one"]["baseline"]["files"] = corpus.baseline_files(self.root, "one")
        self.rewrite(manifest)
        found = corpus.check(self.root)
        self.assertTrue(any("replaced without a recorded" in p for p in found), found)

    def test_a_missing_baseline_is_a_failure(self) -> None:
        (self.root / "baselines" / "one" / "frame.0002.png").unlink()
        self.assertTrue(any("missing reference is a failure" in p for p in corpus.check(self.root)))

    def test_an_extra_baseline_fails(self) -> None:
        png(self.root / "baselines" / "one" / "frame.0003.png", picture(), 4)
        self.assertTrue(any("not listed" in p for p in corpus.check(self.root)))

    def test_a_changed_or_added_scene_file_fails(self) -> None:
        (self.root / "one" / "scenes" / "shot.tnz").write_text("changed")
        self.assertTrue(any("differs from the manifest" in p for p in corpus.check(self.root)))
        (self.root / "one" / "scenes" / "shot.tnz").write_text("scene")
        (self.root / "one" / "extra.png").write_bytes(b"x")
        self.assertTrue(any("not listed" in p for p in corpus.check(self.root)))

    def test_scene_files_that_changed_after_their_baselines_fail(self) -> None:
        (self.root / "one" / "scenes" / "shot.tnz").write_text("changed")
        manifest = copy.deepcopy(self.manifest)
        entry = manifest["scenes"]["one"]
        entry["files"] = corpus.scene_files(self.root, "one")
        entry["scene_sha256"] = corpus.digest(entry["files"])
        self.rewrite(manifest)
        self.assertTrue(any("regenerate them deliberately" in p for p in corpus.check(self.root)))

    def test_a_history_entry_needs_a_real_reason(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["history"][0]["reason"] = "update"
        self.rewrite(manifest)
        self.assertTrue(any("reason is missing or too short" in p for p in corpus.check(self.root)))

    def test_a_scene_folder_the_manifest_does_not_list_fails(self) -> None:
        (self.root / "stray").mkdir()
        self.assertTrue(any("stray" in p for p in corpus.check(self.root)))

    def test_an_unstated_tolerance_fails_the_check(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["scenes"]["one"]["policy"] = {"color": 2, "alpha": 0, "pixels": 0.0, "why": ""}
        self.rewrite(manifest)
        self.assertTrue(any("needs its reason" in p for p in corpus.check(self.root)))

    def with_scene(self, **changes: Any) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["scenes"]["one"].update(changes)
        self.rewrite(manifest)

    def test_a_probe_the_baseline_does_not_hold_fails(self) -> None:
        probes = copy.deepcopy(PROBES)
        probes[0]["rgba"] = [10, 20, 31, 255]
        self.with_scene(probes=probes)
        self.assertTrue(
            any("the baseline holds [10, 20, 30, 255]" in p for p in corpus.check(self.root))
        )

    def test_an_effect_that_changes_no_pixel_between_its_frames_fails(self) -> None:
        probes = copy.deepcopy(PROBES)
        probes[3]["min_difference"] = 101
        self.with_scene(probes=probes)
        self.assertTrue(any("changes by 100" in p for p in corpus.check(self.root)))

    def test_a_corpus_that_shows_no_vector_column_fails(self) -> None:
        self.with_scene(probes=[p for p in PROBES if p["kind"] != "vector"])
        self.assertTrue(any("no probe shows a vector" in p for p in corpus.check(self.root)))

    def test_a_corpus_that_shows_no_png_column_fails(self) -> None:
        self.with_scene(probes=[p for p in PROBES if p["kind"] != "png"])
        self.assertTrue(any("no probe shows a png" in p for p in corpus.check(self.root)))

    def test_a_corpus_with_no_animated_effect_fails(self) -> None:
        self.with_scene(probes=[p for p in PROBES if "frames" not in p])
        self.assertTrue(
            any("changing a pixel between frames" in p for p in corpus.check(self.root))
        )

    def test_a_probe_outside_the_image_or_frames_fails(self) -> None:
        probes = copy.deepcopy(PROBES)
        probes[0]["x"] = 4
        probes[1]["frame"] = 2
        self.with_scene(probes=probes)
        found = corpus.check(self.root)
        self.assertTrue(any("pixel outside the image" in p for p in found), found)
        self.assertTrue(any("frame outside the baselines" in p for p in found), found)

    def test_declared_effects_must_be_the_effects_the_scene_file_holds(self) -> None:
        self.with_scene(effects=EFFECTS[:-1])
        self.assertTrue(any("its scene file holds" in p for p in corpus.check(self.root)))

    def test_too_few_effect_types_in_the_corpus_fail(self) -> None:
        (self.root / "one" / "scenes" / "shot.tnz").write_text("scene <aFx id='1'>")
        manifest = copy.deepcopy(self.manifest)
        entry = manifest["scenes"]["one"]
        entry["effects"] = ["aFx"]
        entry["files"] = corpus.scene_files(self.root, "one")
        entry["scene_sha256"] = corpus.digest(entry["files"])
        entry["baseline"]["rendered_from"] = entry["scene_sha256"]
        self.rewrite(manifest)
        self.assertTrue(any("fewer than 6" in p for p in corpus.check(self.root)))

    def test_a_comparison_without_a_statement_fails(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        del manifest["comparisons"]["bridge_enabled"]
        self.rewrite(manifest)
        self.assertTrue(
            any("no statement for bridge_enabled" in p for p in corpus.check(self.root))
        )

    def test_a_compared_statement_must_name_a_case_that_exists(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["comparisons"]["bridge_disabled"]["cases"] = ["a_case_nobody_wrote"]
        self.rewrite(manifest)
        self.assertTrue(any("does not define" in p for p in corpus.check(self.root)))

    def test_a_comparison_that_is_not_built_must_say_why(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["comparisons"]["assisted_save_reopen"]["reason"] = "later"
        self.rewrite(manifest)
        self.assertTrue(any("reason is missing or short" in p for p in corpus.check(self.root)))

    def test_a_missing_manifest_is_reported(self) -> None:
        (self.root / "manifest.json").unlink()
        self.assertTrue(any("does not exist" in p for p in corpus.check(self.root)))


class CompareSceneTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.manifest = make_corpus(self.root)
        self.frames = self.root / "frames"
        self.frames.mkdir()
        for frame in range(2):
            shutil.copyfile(
                self.root / "baselines" / "one" / f"frame.{frame + 1:04d}.png",
                self.frames / f"f{frame}.png",
            )

    def compare(self) -> list[str]:
        return corpus.compare_scene(
            self.manifest, "one", sorted(self.frames.glob("*.png")), self.root
        )

    def test_matching_frames_pass(self) -> None:
        self.assertEqual(self.compare(), [])

    def test_a_different_frame_fails_and_is_named(self) -> None:
        png(self.frames / "f1.png", picture(base=(10, 20, 40, 255)), 4)
        found = self.compare()
        self.assertTrue(found)
        self.assertTrue(all("frame.0002.png" in problem for problem in found), found)

    def test_too_few_frames_and_an_unknown_scene_fail(self) -> None:
        (self.frames / "f1.png").unlink()
        self.assertIn("1 frames rendered, 2 baselines", self.compare()[0])
        self.assertEqual(
            corpus.compare_scene(self.manifest, "none", [], self.root), ["none: not in the corpus"]
        )

    def test_a_missing_baseline_file_fails_and_is_never_created(self) -> None:
        (self.root / "baselines" / "one" / "frame.0001.png").unlink()
        found = self.compare()
        self.assertTrue(any("cannot read" in p for p in found), found)
        self.assertFalse((self.root / "baselines" / "one" / "frame.0001.png").exists())

    def test_the_compare_never_writes_a_baseline(self) -> None:
        before = corpus.baseline_files(self.root, "one")
        png(self.frames / "f0.png", picture(base=(0, 0, 0, 0)), 4)
        self.compare()
        self.assertEqual(corpus.baseline_files(self.root, "one"), before)


class RegenerationTest(unittest.TestCase):
    def setUp(self) -> None:
        # These tests run in CI too, where the guard refuses every regeneration.
        environment = mock.patch.dict("os.environ")
        environment.start()
        self.addCleanup(environment.stop)
        os.environ.pop("CI", None)

    def test_a_regeneration_without_a_reason_or_in_ci_is_refused(self) -> None:
        with self.assertRaisesRegex(SystemExit, "reason"):
            build.refuse_unless_deliberate("fix")
        with self.assertRaisesRegex(SystemExit, "reason"):
            build.refuse_unless_deliberate("   ")
        with mock.patch.dict("os.environ", {"CI": "true"}):
            with self.assertRaisesRegex(SystemExit, "never regenerated in CI"):
                build.refuse_unless_deliberate("a perfectly good and long enough reason")
        build.refuse_unless_deliberate("a perfectly good and long enough reason")

    def test_building_baselines_refuses_before_rendering_anything(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_corpus(root)
            before = corpus.baseline_files(root, "one")
            with mock.patch.object(build, "_render", side_effect=AssertionError("rendered")):
                with self.assertRaises(SystemExit):
                    build.build_baselines(Path("tcomposer"), "short", root)
            self.assertEqual(corpus.baseline_files(root, "one"), before)

    def test_a_regeneration_replaces_the_frames_and_appends_its_history(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_corpus(root)
            scratch = root.parent / (root.name + "-frames")
            scratch.mkdir()
            frames = []
            for index in range(2):
                path = scratch / f"frame.{index + 1:04d}.png"
                pixels = picture()
                pixels[1] = (110, 20, 30, 255) if index else pixels[1]
                pixels[15] = (1, 1, 1, 255)
                png(path, pixels, 4)
                frames.append(path)
            tool = scratch / "tcomposer"
            tool.write_bytes(b"binary")
            with mock.patch.object(build, "_render", return_value=frames):
                build.build_baselines(tool, "the renderer changed how it rounds blends", root)
            self.addCleanup(shutil.rmtree, scratch, ignore_errors=True)
            self.assertEqual(corpus.check(root), [])
            history = corpus.load_manifest(root / "manifest.json")["history"]
            self.assertEqual(len(history), 2)
            self.assertIn("rounds blends", history[-1]["reason"])
            self.assertNotEqual(
                history[0]["baseline_set_sha256"], history[1]["baseline_set_sha256"]
            )


class RenderTest(unittest.TestCase):
    def test_a_renderer_that_writes_the_wrong_number_of_frames_is_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            done = mock.Mock(returncode=0, stdout="", stderr="")
            with mock.patch.object(oracle.subprocess, "run", return_value=done):
                with self.assertRaisesRegex(oracle.OracleError, "0 of 2 frames"):
                    oracle.render_with_tcomposer(
                        root / "tcomposer", root / "shot.tnz", root, range(2), stuff=root / "stuff"
                    )

    def test_find_tcomposer_needs_an_executable_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            self.assertIsNone(oracle.find_tcomposer(folder))
            (folder / "tcomposer").write_bytes(b"x")
            (folder / "tcomposer").chmod(0o755)
            self.assertEqual(oracle.find_tcomposer(folder), folder / "tcomposer")


if __name__ == "__main__":
    unittest.main()
