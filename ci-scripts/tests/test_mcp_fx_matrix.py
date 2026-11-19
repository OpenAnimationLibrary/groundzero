"""Prove the FX matrix check fails for each way the matrix could overstate what was run."""

from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_fx_matrix as matrix
import mcp_fx_matrix_cases as cases

TYPES = ["alphaFx", "betaFx"]
DIGEST = "ab" * 32
FORMAT = {"job_format": "x", "profile": "p", "compared": "frames", "cap": "none"}
CENSUS: dict[str, Any] = {
    "platforms": {"qualified": ["macOS"], "not_recorded": ["Linux"]},
    "formats": [
        {"id": "png", "state": "supported"},
        {"id": "mp4", "state": "supported"},
    ],
    "claims": {"fx_builtins": 2},
    "tools": {"fx.create": {}, "fx.graph": {}},
}


def cell_passed(form: str, run: str = "r1") -> dict[str, Any]:
    if form == "png_sequence":
        tolerance = dict(cases.PNG_TOLERANCE)
        observed = {"max_color": 0, "max_alpha": 0, "differing_pixels": 0}
    else:
        tolerance = {
            "block_error": 6.0,
            "mean_error": 3.0,
            "decoded_differing_bytes": 0,
            "why": "from the observed error",
        }
        observed = {
            "block_error": 4.0,
            "mean_error": 1.5,
            "max_error": 20,
            "decoded_differing_bytes": 0,
        }
    extra = (
        {}
        if form == "png_sequence"
        else {
            "decoded_reference": "cd" * 32,
            "separation": {"nearest_other_block_error": 40.0, "nearest_other_type": "betaFx"},
        }
    )
    return {
        **extra,
        "status": "passed",
        "tolerance": tolerance,
        "observed": observed,
        "reference": DIGEST,
        "run": run,
    }


def make_data() -> dict[str, Any]:
    data: dict[str, Any] = {
        "schema_version": 1,
        "formats": [
            {**FORMAT, "id": "png_sequence", "census_format": "png"},
            {**FORMAT, "id": "mp4", "census_format": "mp4"},
        ],
        "platforms": {"macOS": {"latest_run": "r1"}, "Linux": {"latest_run": None}},
        "runs": {
            "r1": {
                "platform": "macOS",
                "reason": "the first run, for the tests",
                "recorded": "2026-11-19 12:00:00",
                "covered": "2 of 2 types",
                "identity": {"build": "x"},
            }
        },
        "cells": {},
        "negative_test_gaps": {},
    }
    for kind in TYPES:
        data["cells"][kind] = {
            form: {
                "macOS": cell_passed(form),
                "Linux": {"status": "not_tested", "reason": "no run on this platform"},
            }
            for form in ("png_sequence", "mp4")
        }
    return data


class CheckTest(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.scripts = Path(directory.name)
        (self.scripts / "mcp_revision_matrix.json").write_text(
            json.dumps({"tools": {"fx.create": {"reject": [{"args": {}}]}}})
        )
        (self.scripts / "mcp_one_cases.py").write_text(
            'def case(case):\n    case.failure("fx.graph", {})\n'
        )
        self.data = make_data()

    def problems(self) -> list[str]:
        return matrix.check_data(self.data, CENSUS, TYPES, self.scripts)

    def named(self, text: str) -> bool:
        return any(text in problem for problem in self.problems())

    def test_a_consistent_matrix_passes(self) -> None:
        self.assertEqual(self.problems(), [])

    def test_a_type_without_cells_is_reported(self) -> None:
        del self.data["cells"]["betaFx"]
        self.assertTrue(self.named("no cell for betaFx / png_sequence / macOS"))

    def test_a_missing_platform_cell_is_reported(self) -> None:
        del self.data["cells"]["alphaFx"]["mp4"]["Linux"]
        self.assertTrue(self.named("no cell for alphaFx / mp4 / Linux"))

    def test_a_cell_of_a_removed_type_is_reported(self) -> None:
        self.data["cells"]["gone"] = self.data["cells"]["alphaFx"]
        self.assertTrue(self.named("no longer exists"))

    def test_a_platform_the_census_does_not_list_is_reported(self) -> None:
        self.data["platforms"]["Plan9"] = {"latest_run": None}
        self.assertTrue(self.named("differ from the census"))

    def test_a_status_outside_the_four_is_reported(self) -> None:
        self.data["cells"]["alphaFx"]["mp4"]["macOS"]["status"] = "ok"
        self.assertTrue(self.named("is not one of"))

    def test_a_cell_that_did_not_pass_needs_its_reason(self) -> None:
        self.data["cells"]["alphaFx"]["mp4"]["Linux"]["reason"] = ""
        self.assertTrue(self.named("needs its reason"))

    def test_an_unavailable_cell_cannot_carry_a_measurement(self) -> None:
        cell = self.data["cells"]["alphaFx"]["mp4"]["macOS"]
        cell["status"], cell["reason"] = "unavailable", "no media tools were configured"
        self.assertTrue(self.named("measurement it did not make"))

    def test_a_tolerance_above_the_cap_is_reported(self) -> None:
        cell = self.data["cells"]["alphaFx"]["png_sequence"]["macOS"]
        cell["tolerance"]["max_color"] = 2
        cell["tolerance"]["why"] = "rounding"
        self.assertTrue(self.named("above the cap"))

    def test_a_movie_tolerance_above_the_cap_is_reported(self) -> None:
        self.data["cells"]["alphaFx"]["mp4"]["macOS"]["tolerance"]["block_error"] = 17
        self.assertTrue(self.named("above the cap"))

    def test_a_pass_outside_its_own_tolerance_is_reported(self) -> None:
        self.data["cells"]["alphaFx"]["mp4"]["macOS"]["observed"]["mean_error"] = 7
        self.assertTrue(self.named("is above its tolerance"))

    def test_a_nonzero_tolerance_needs_its_reason(self) -> None:
        self.data["cells"]["alphaFx"]["mp4"]["macOS"]["tolerance"]["why"] = ""
        self.assertTrue(self.named("needs its reason in `why`"))

    def test_a_passed_cell_needs_its_reference(self) -> None:
        del self.data["cells"]["alphaFx"]["png_sequence"]["macOS"]["reference"]
        self.assertTrue(self.named("without the SHA-256 of its reference"))

    def test_a_movie_must_be_compared_with_the_png_of_its_type(self) -> None:
        self.data["cells"]["alphaFx"]["mp4"]["macOS"]["reference"] = "ef" * 32
        self.assertTrue(self.named("was not compared with the PNG render"))

    def test_a_passed_movie_whose_tolerance_does_not_separate_it_is_reported(self) -> None:
        cell = self.data["cells"]["alphaFx"]["mp4"]["macOS"]
        cell["separation"]["nearest_other_block_error"] = 5.0
        self.assertTrue(self.named("does not separate it from another type's picture"))

    def test_an_unverified_movie_is_accepted_only_when_it_is_blind(self) -> None:
        cell = self.data["cells"]["alphaFx"]["mp4"]["macOS"]
        cell["status"], cell["reason"] = "unverified", "the movie cannot tell it from betaFx"
        self.assertTrue(self.named("unverified although its tolerance separates it"))
        cell["separation"]["nearest_other_block_error"] = 5.0
        self.assertEqual(self.problems(), [])

    def test_a_movie_cell_without_a_separation_measure_is_reported(self) -> None:
        del self.data["cells"]["alphaFx"]["mp4"]["macOS"]["separation"]
        self.assertTrue(self.named("no measure of how far the nearest other picture is"))

    def test_a_passed_movie_cell_needs_its_reference_decode(self) -> None:
        del self.data["cells"]["alphaFx"]["mp4"]["macOS"]["decoded_reference"]
        self.assertTrue(self.named("without the SHA-256 of its reference decode"))

    def test_a_cell_naming_an_unrecorded_run_is_reported(self) -> None:
        self.data["cells"]["alphaFx"]["mp4"]["macOS"]["run"] = "ghost"
        self.assertTrue(self.named("which is not recorded"))

    def test_a_run_needs_a_reason_and_a_build(self) -> None:
        self.data["runs"]["r1"]["reason"] = "x"
        self.data["runs"]["r1"]["identity"] = {}
        self.assertTrue(self.named("reason is missing"))
        self.assertTrue(self.named("no build identity"))

    def test_a_platform_with_an_untested_cell_cannot_be_labelled_complete(self) -> None:
        self.data["platforms"]["Linux"]["complete"] = True
        self.assertTrue(self.named("a subset run cannot be labelled complete"))

    def test_an_honest_incomplete_label_passes(self) -> None:
        self.data["platforms"]["Linux"]["complete"] = False
        self.assertEqual(self.problems(), [])

    def test_a_subset_run_leaves_the_platform_incomplete_and_the_label_cannot_say_otherwise(
        self,
    ) -> None:
        self.data["runs"]["r2"] = copy.deepcopy(self.data["runs"]["r1"])
        self.data["platforms"]["macOS"]["latest_run"] = "r2"
        self.data["platforms"]["macOS"]["complete"] = True
        self.assertTrue(self.named("1 from an older run") or self.named("4 from an older run"))
        summary = matrix.platform_summary(self.data, "macOS", TYPES)
        self.assertFalse(summary["complete"])
        self.assertEqual(summary["stale"], 4)

    def test_a_full_run_is_complete(self) -> None:
        for kind in TYPES:
            self.data["cells"][kind]["png_sequence"]["Linux"] = cell_passed("png_sequence")
            self.data["cells"][kind]["mp4"]["Linux"] = {
                "status": "unavailable",
                "reason": "no media tools on this platform",
            }
        self.data["platforms"]["Linux"]["latest_run"] = "r1"
        self.assertTrue(matrix.platform_summary(self.data, "Linux", TYPES)["complete"])

    def test_a_tool_without_a_negative_test_is_reported(self) -> None:
        (self.scripts / "mcp_one_cases.py").write_text("def case(case):\n    pass\n")
        self.assertTrue(self.named("fx.graph has no negative test"))

    def test_a_listed_gap_is_accepted_and_goes_stale_when_filled(self) -> None:
        (self.scripts / "mcp_one_cases.py").write_text("def case(case):\n    pass\n")
        self.data["negative_test_gaps"]["fx.graph"] = "takes no input, so nothing is refused"
        self.assertEqual(self.problems(), [])
        (self.scripts / "mcp_one_cases.py").write_text(
            'def case(case):\n    case.failure("fx.graph", {})\n'
        )
        self.assertTrue(self.named("now has a negative test; delete the entry"))

    def test_a_gap_needs_a_reason_and_a_released_tool(self) -> None:
        self.data["negative_test_gaps"]["fx.nonexistent"] = "x"
        self.assertTrue(self.named("not a released tool"))
        self.assertTrue(self.named("the reason is missing"))

    def test_a_success_call_is_not_a_negative_test(self) -> None:
        (self.scripts / "mcp_one_cases.py").write_text(
            'def case(case):\n    case.success("fx.graph", {})\n'
        )
        self.assertTrue(self.named("fx.graph has no negative test"))

    def test_a_type_count_that_disagrees_with_the_census_is_reported(self) -> None:
        census = copy.deepcopy(CENSUS)
        census["claims"]["fx_builtins"] = 3
        found = matrix.check_data(self.data, census, TYPES, self.scripts)
        self.assertTrue(any("census claims 3" in problem for problem in found))


class DocumentTest(unittest.TestCase):
    def test_a_stale_document_is_reported(self) -> None:
        data = make_data()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "matrix.md"
            self.assertTrue(matrix.check_document(data, TYPES, path))
            path.write_text(matrix.document_of(data, TYPES), encoding="utf-8")
            self.assertEqual(matrix.check_document(data, TYPES, path), [])
            data["cells"]["alphaFx"]["mp4"]["macOS"]["status"] = "failed"
            data["cells"]["alphaFx"]["mp4"]["macOS"]["reason"] = "outside the tolerance"
            self.assertTrue(matrix.check_document(data, TYPES, path))

    def test_the_document_keeps_the_four_outcomes_apart(self) -> None:
        text = matrix.document_of(make_data(), TYPES)
        self.assertIn("not tested", text)
        self.assertIn("passed", text)
        self.assertIn("incomplete", text)


class UpdateTest(unittest.TestCase):
    def test_update_adds_missing_cells_as_not_tested_and_keeps_recorded_ones(self) -> None:
        data = make_data()
        del data["cells"]["betaFx"]
        recorded = copy.deepcopy(data["cells"]["alphaFx"])
        matrix.update_grid(data, [*TYPES, "gammaFx"])
        self.assertEqual(data["cells"]["alphaFx"], recorded)
        self.assertEqual(data["cells"]["gammaFx"]["mp4"]["macOS"]["status"], "not_tested")
        matrix.update_grid(data, ["alphaFx"])
        self.assertEqual(list(data["cells"]), ["alphaFx"])


class MeasurementTest(unittest.TestCase):
    def test_the_png_cell_fails_when_a_single_pixel_differs_from_the_preview(self) -> None:
        pixels = [(1, 2, 3, 255)] * 4
        other = [*pixels[:3], (1, 2, 4, 255)]
        measured = {
            "digest": cases.pixel_digest(other),
            "preview_difference": cases.png_difference(pixels, other),
        }
        cell = cases.cell_from(measured, "png_sequence", {"png": cases.pixel_digest(other)})
        self.assertEqual(cell["status"], "failed")
        self.assertIn("differing_pixels", cell["reason"])

    def test_the_png_cell_fails_when_the_reference_differs(self) -> None:
        pixels = [(1, 2, 3, 255)] * 4
        measured = {
            "digest": cases.pixel_digest(pixels),
            "preview_difference": cases.png_difference(pixels, pixels),
        }
        cell = cases.cell_from(measured, "png_sequence", {"png": DIGEST})
        self.assertEqual(cell["status"], "failed")
        self.assertEqual(
            cases.cell_from(measured, "png_sequence", {"png": cases.pixel_digest(pixels)})[
                "status"
            ],
            "passed",
        )

    def movie(self, **changes: object) -> dict[str, object]:
        measured: dict[str, object] = {
            "mean_error": 1.5,
            "block_error": 4.0,
            "max_error": 20,
            "decoded_digest": "cd" * 32,
            "nearest_other_block_error": 40.0,
            "nearest_other_type": "betaFx",
        }
        return {**measured, **changes}

    references = {"png": DIGEST, "decoded": "cd" * 32}

    def test_the_movie_cell_takes_its_tolerance_from_the_observed_error(self) -> None:
        cell = cases.cell_from(self.movie(), "mp4", self.references)
        self.assertEqual(cell["status"], "passed")
        self.assertEqual(cell["tolerance"]["block_error"], 6.0)
        self.assertEqual(cell["tolerance"]["mean_error"], 3.0)

    def test_a_stored_tolerance_fails_a_worse_frame(self) -> None:
        stored = cases.cell_from(self.movie(), "mp4", self.references)["tolerance"]
        worse = self.movie(block_error=9.0)
        cell = cases.cell_from(worse, "mp4", self.references, stored)
        self.assertEqual(cell["status"], "failed")
        self.assertIn("block_error", cell["reason"])

    def test_the_movie_cell_fails_when_the_decoded_bytes_change(self) -> None:
        stored = cases.cell_from(self.movie(), "mp4", self.references)["tolerance"]
        cell = cases.cell_from(self.movie(decoded_digest="ee" * 32), "mp4", self.references, stored)
        self.assertEqual(cell["status"], "failed")
        self.assertIn("decoded_differing_bytes", cell["reason"])

    def test_a_movie_that_cannot_be_told_from_another_picture_is_unverified_not_passed(
        self,
    ) -> None:
        blind = self.movie(nearest_other_block_error=5.0, nearest_other_type="gammaFx")
        cell = cases.cell_from(blind, "mp4", self.references)
        self.assertEqual(cell["status"], "unverified")
        self.assertIn("gammaFx", cell["reason"])

    def test_a_run_without_media_tools_is_unavailable_never_passed(self) -> None:
        cell = cases.cell_from(
            {"skipped": "no media tools were configured"}, "mp4", {"png": DIGEST}
        )
        self.assertEqual(cell["status"], "unavailable")

    def test_movie_error_is_measured_against_the_matte_of_the_png(self) -> None:
        pixels = [(200, 100, 50, 128)] * 256
        matte = cases.black_matte(pixels)
        self.assertEqual(list(matte[:3]), [100, 50, 25])
        measured = cases.movie_difference(matte, bytes([100, 50, 28] * 256), size=16)
        self.assertEqual(measured["max_error"], 3)
        self.assertEqual(measured["block_error"], 1.0)

    def test_a_small_wrong_region_shows_in_its_block_but_hardly_in_the_mean(self) -> None:
        size = 64
        expected = bytes(size * size * 3)
        wrong = bytearray(expected)
        for row in range(8):
            wrong[row * size * 3 : row * size * 3 + 8 * 3] = bytes([200] * 24)
        found = cases.movie_difference(expected, bytes(wrong), size=size)
        self.assertLess(found["mean_error"], 4.0)
        self.assertGreater(found["block_error"], 12.0)

    def test_apply_run_stores_the_run_and_marks_a_subset_incomplete(self) -> None:
        data = make_data()
        pixels = [(1, 2, 3, 255)] * 4
        entry = {
            "png_sequence": {
                "digest": cases.pixel_digest(pixels),
                "preview_difference": cases.png_difference(pixels, pixels),
            },
            "mp4": {"skipped": "no media tools were configured"},
        }
        original = cases.admitted_types
        cases.admitted_types = lambda: TYPES  # type: ignore[assignment]
        try:
            name = cases.apply_run(data, {"alphaFx": entry}, {"build": "y"}, "a subset, for test")
        finally:
            cases.admitted_types = original  # type: ignore[assignment]
        platform = cases.host_platform()
        data["platforms"].setdefault(platform, {"latest_run": None})
        self.assertEqual(data["platforms"][platform]["latest_run"], name)
        self.assertEqual(data["cells"]["alphaFx"]["mp4"][platform]["status"], "unavailable")
        self.assertEqual(data["cells"]["alphaFx"]["png_sequence"][platform]["run"], name)


class CommittedMatrixTest(unittest.TestCase):
    def test_the_committed_matrix_and_document_are_consistent(self) -> None:
        data = matrix.load_data()
        census = json.loads(matrix.CENSUS_PATH.read_text(encoding="utf-8"))
        types = matrix.admitted_types()
        self.assertEqual(matrix.check_data(data, census, types), [])
        self.assertEqual(matrix.check_document(data, types), [])


if __name__ == "__main__":
    unittest.main()
