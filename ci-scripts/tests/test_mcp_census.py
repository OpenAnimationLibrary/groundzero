"""Prove each way the capability census can drift makes its check fail."""

from __future__ import annotations

import copy
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_census as census_check
from mcp_census_text import render_document


def committed() -> tuple[dict[str, Any], str]:
    census = census_check.load_census()
    readme = census_check.README_PATH.read_text(encoding="utf-8")
    return census, readme


def fake_catalog(census: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    """Build the worker answers that agree with the census, evidence included."""
    tools: dict[str, dict[str, Any]] = {}
    for name, entry in census["tools"].items():
        if entry["origin"] != "worker":
            continue
        tools[name] = {
            "name": name,
            "toolset": entry["toolset"],
            "description": "",
            "annotations": {"readOnlyHint": not entry["writes"]},
            "inputSchema": {"properties": {}},
        }
    for entry in census["formats"]:
        if entry["state"] != "supported":
            continue
        evidence = entry["evidence"]
        tool = tools[evidence["tool"]]
        if evidence["kind"] == "description":
            tool["description"] += f" {evidence['text']}"
        if evidence["kind"] == "enum":
            schema = tool["inputSchema"]["properties"].setdefault(evidence["property"], {})
            schema.setdefault("enum", []).append(evidence["value"])
    names = sorted({tool["toolset"] for tool in tools.values()})
    capabilities = {
        "protocol_version": census["claims"]["protocol_version"],
        "tools": list(tools.values()),
        "toolsets": [{"name": name} for name in names],
    }
    claims = census["claims"]
    fx_catalog = {
        "profile": claims["fx_profile"],
        "effects": [{}] * claims["fx_builtins"],
    }
    return capabilities, fx_catalog


class CensusFailureCases(unittest.TestCase):
    def setUp(self) -> None:
        self.census, self.readme = committed()
        self.capabilities, self.fx_catalog = fake_catalog(self.census)

    def live(self) -> list[str]:
        return census_check.check_live(self.census, self.capabilities, self.fx_catalog, self.readme)

    def static(self) -> list[str]:
        return census_check.static_problems(self.census, self.readme)

    def assert_reports(self, problems: list[str], fragment: str) -> None:
        self.assertTrue(
            any(fragment in problem for problem in problems),
            f"expected a problem containing {fragment!r}, got {problems}",
        )

    def outcome(self, identifier: str) -> dict[str, Any]:
        return next(o for o in self.census["outcomes"] if o["id"] == identifier)

    def test_committed_files_pass(self) -> None:
        self.assertEqual(self.static(), [])

    def test_catalog_that_matches_the_census_passes(self) -> None:
        self.assertEqual(self.live(), [])

    def test_catalog_tool_without_a_census_entry_fails(self) -> None:
        extra = copy.deepcopy(self.capabilities["tools"][0])
        extra["name"] = "scene.invented"
        self.capabilities["tools"].append(extra)
        self.assert_reports(self.live(), "scene.invented: in the catalog but not in the census")

    def test_missing_tool_message_names_the_file_and_the_entry(self) -> None:
        extra = copy.deepcopy(self.capabilities["tools"][0])
        extra.update(name="scene.invented", toolset="core")
        extra["annotations"]["readOnlyHint"] = False
        self.capabilities["tools"].append(extra)
        message = next(p for p in self.live() if p.startswith("tool scene.invented"))
        self.assertIn("ci-scripts/mcp_census.json", message)
        self.assertIn(
            '{"scene.invented": {"origin": "worker", "toolset": "core", "writes": true}}',
            message,
        )
        self.assertIn('"tools": ["scene.invented"]', message)
        self.assertIn('"classification": "exposed"', message)

    def test_tool_without_an_outcome_message_names_the_file_and_the_entry(self) -> None:
        self.census["tools"]["scene.invented"] = {
            "origin": "worker",
            "toolset": "core",
            "writes": False,
            "tests": [],
        }
        message = next(p for p in self.static() if p.startswith("tool scene.invented"))
        self.assertIn("not delivered by any outcome", message)
        self.assertIn("ci-scripts/mcp_census.json", message)
        self.assertIn('"tools": ["scene.invented"]', message)

    def test_a_checkout_without_a_readme_skips_only_the_readme_comparison(self) -> None:
        self.assertEqual(
            census_check.check_live(self.census, self.capabilities, self.fx_catalog, None), []
        )
        self.capabilities["tools"].pop()
        problems = census_check.check_live(self.census, self.capabilities, self.fx_catalog, None)
        self.assert_reports(problems, "in the census but not in the catalog")

    def test_census_tool_missing_from_the_catalog_fails(self) -> None:
        self.capabilities["tools"] = [
            tool for tool in self.capabilities["tools"] if tool["name"] != "scene.summary"
        ]
        problems = self.live()
        self.assert_reports(problems, "scene.summary: in the census but not in the catalog")
        self.assert_reports(problems, "README names tool scene.summary, which the catalog lacks")

    def test_catalog_toolset_or_write_flag_drift_fails(self) -> None:
        tool = next(t for t in self.capabilities["tools"] if t["name"] == "scene.create")
        tool["toolset"] = "elsewhere"
        tool["annotations"]["readOnlyHint"] = True
        problems = self.live()
        self.assert_reports(problems, "scene.create: toolset elsewhere differs")
        self.assert_reports(problems, "scene.create: write annotation differs")

    def test_catalog_toolset_missing_fails(self) -> None:
        self.capabilities["toolsets"] = [
            t for t in self.capabilities["toolsets"] if t["name"] != "audio"
        ]
        self.assert_reports(self.live(), "toolset audio: in the census but not in the catalog")

    def test_readme_tool_the_census_lacks_fails(self) -> None:
        self.readme += "\nUse `scene.invented` to do it.\n"
        self.assert_reports(self.static(), "README names tool scene.invented")
        self.assert_reports(self.live(), "README names tool scene.invented")

    def test_readme_tool_of_an_unknown_family_fails(self) -> None:
        self.readme += "\nUse `ghost.thing` to do it.\n"
        self.assert_reports(self.static(), "README names tool ghost.thing")

    def test_readme_omitting_a_catalog_tool_fails(self) -> None:
        self.readme = re.sub(r"`palette\.page\.move`,\s+", "", self.readme)
        self.assertNotIn("`palette.page.move`", self.readme)
        self.assert_reports(self.static(), "README omits tool palette.page.move")
        self.assert_reports(self.live(), "README omits tool palette.page.move")

    def test_readme_toolset_the_catalog_lacks_fails(self) -> None:
        self.readme += "\nEnable the `ghost` toolset first.\n"
        self.assert_reports(self.static(), "README names toolset ghost")
        self.assert_reports(self.live(), "README names toolset ghost")

    def test_readme_toolsets_flag_the_catalog_lacks_fails(self) -> None:
        self.readme += "\nLaunch with `--toolsets audio,ghost`.\n"
        self.assert_reports(self.static(), "README names toolset ghost")

    def test_readme_file_format_the_census_lacks_fails(self) -> None:
        self.readme += "\nRender can also write TIFF files.\n"
        self.assert_reports(self.static(), "README names file format TIFF")

    def test_readme_format_literal_the_census_lacks_fails(self) -> None:
        self.readme += '\nPass `format: "gif"` to render.submit.\n'
        self.assert_reports(self.static(), "README names file format GIF")

    def test_readme_extension_the_census_lacks_fails(self) -> None:
        self.readme += "\nWrites a `.tga` file.\n"
        self.assert_reports(self.static(), "README names file format TGA")

    def test_format_the_catalog_does_not_show_fails(self) -> None:
        tool = next(t for t in self.capabilities["tools"] if t["name"] == "render.submit")
        tool["inputSchema"]["properties"]["format"]["enum"] = ["png"]
        self.assert_reports(self.live(), "format mp4: the catalog does not show")

    def test_format_evidence_tool_missing_from_the_catalog_fails(self) -> None:
        self.capabilities["tools"] = [
            t for t in self.capabilities["tools"] if t["name"] != "lipsync.import"
        ]
        self.assert_reports(self.live(), "format papagayo_dat: evidence tool lipsync.import")

    def test_outcome_without_a_disposition_fails(self) -> None:
        del self.outcome("scene.create_detached")["disposition"]
        self.assert_reports(self.static(), "scene.create_detached: has no disposition")

    def test_planned_outcome_needs_a_milestone_label(self) -> None:
        self.outcome("scene.compare_revisions")["disposition"] = {"kind": "planned"}
        self.assert_reports(self.static(), "needs a milestone label")

    def test_restricted_outcome_needs_a_reason(self) -> None:
        self.outcome("admin.lifecycle")["disposition"] = {"kind": "restricted", "reason": " "}
        self.assert_reports(self.static(), "admin.lifecycle: a restricted outcome needs a reason")

    def test_classification_must_match_the_disposition(self) -> None:
        self.outcome("scene.create_detached")["disposition"] = {
            "kind": "unsupported",
            "reason": "x",
        }
        self.assert_reports(self.static(), "cannot have disposition 'unsupported'")

    def test_unknown_classification_fails(self) -> None:
        self.outcome("scene.create_detached")["classification"] = "mostly"
        self.assert_reports(self.static(), "unknown classification 'mostly'")

    def test_compositional_outcome_needs_a_journey(self) -> None:
        del self.outcome("scene.author_editable_shot")["journey"]
        self.assert_reports(self.static(), "needs a journey test")

    def test_optional_outcome_must_say_what_it_requires(self) -> None:
        del self.outcome("render.mp4_movie")["requires"]
        self.assert_reports(self.static(), "must say what it requires")

    def test_tool_without_an_outcome_fails(self) -> None:
        self.outcome("scene.inspect")["tools"] = []
        self.outcome("scene.inspect")["disposition"] = {"kind": "planned", "milestone": "W2"}
        self.outcome("scene.inspect")["classification"] = "not_exposed"
        self.assert_reports(self.static(), "tool scene.summary: not delivered by any outcome")

    def test_outcome_naming_an_unknown_tool_fails(self) -> None:
        self.outcome("scene.inspect")["tools"].append("scene.invented")
        self.assert_reports(self.static(), "names tool scene.invented")

    def test_listed_test_that_does_not_exist_fails(self) -> None:
        tests = self.census["tools"]["scene.create"]["tests"]
        tests.append("ci-scripts/mcp_acceptance.py::test_that_was_deleted")
        tests.append("ci-scripts/no_such_file.py")
        problems = self.static()
        self.assert_reports(problems, "test_that_was_deleted does not exist")
        self.assert_reports(problems, "no_such_file.py does not exist")

    def test_journey_that_does_not_exist_fails(self) -> None:
        self.outcome("scene.author_editable_shot")["journey"] = ["ci-scripts/gone.py"]
        self.assert_reports(self.static(), "journey ci-scripts/gone.py does not exist")

    def test_stored_census_has_no_derived_tests(self) -> None:
        stored = json.loads(census_check.dump_census(self.census))
        self.assertTrue(all("tests" not in entry for entry in stored["tools"].values()))
        self.assertEqual(
            census_check.dump_census(self.census),
            census_check.CENSUS_PATH.read_text(encoding="utf-8"),
        )

    def test_marked_cases_count_as_acceptance_tests(self) -> None:
        marked = census_check.marked_cases()
        self.assertIn("test_fx_selected_output_survives_reopen", marked)
        listed = {test for entry in self.census["tools"].values() for test in entry["tests"]}
        prefix = f"{census_check.ACCEPTANCE}::"
        self.assertTrue(any(test.removeprefix(prefix) in marked for test in listed))

    def test_document_names_every_tool(self) -> None:
        document = render_document(self.census, census_check.report(self.census))
        missing = [name for name in self.census["tools"] if f"`{name}`" not in document]
        self.assertEqual(missing, [])

    def test_stale_readme_index_fails(self) -> None:
        self.readme = self.readme.replace(
            "`toolsets.disable`, `toolsets.enable`", "`toolsets.enable`, `toolsets.disable`"
        )
        self.assert_reports(self.static(), "README tool index is stale")

    def test_adapter_and_live_tools_must_match_their_sources(self) -> None:
        self.census["tools"]["toolsets.invented"] = {
            "origin": "adapter",
            "toolset": "adapter",
            "writes": False,
            "tests": [],
        }
        del self.census["tools"]["live.receipt"]
        problems = self.static()
        self.assert_reports(problems, "toolsets.invented: in the census but not in toolsets.ts")
        self.assert_reports(problems, "live.receipt: in live-protocol.ts but not in the census")

    def test_numeric_claims_must_match(self) -> None:
        self.readme = self.readme.replace("admits 31 builtins", "admits 30 builtins")
        self.assert_reports(self.static(), "README FX profile or builtin count differs")
        claimed = self.census["claims"]["protocol_version"]
        self.readme = self.readme.replace(
            f"protocol version {claimed}", f"protocol version {claimed + 1}"
        )
        self.assert_reports(self.static(), "README protocol version differs")

    def test_catalog_claims_must_match(self) -> None:
        self.fx_catalog["effects"].pop()
        self.capabilities["protocol_version"] += 1
        problems = self.live()
        self.assert_reports(problems, "catalog FX profile or builtin count differs")
        self.assert_reports(problems, "catalog protocol version differs")

    def test_report_counts_the_three_measures_separately(self) -> None:
        counts = census_check.report(self.census)
        self.assertEqual(counts["inventoried"], len(self.census["outcomes"]))
        self.assertLessEqual(counts["tested"], counts["implemented"])
        self.assertLess(counts["implemented"], counts["inventoried"])
        self.assertIn("macOS only so far", counts["platforms"]["summary"])

    def test_untested_tool_lowers_the_tested_count(self) -> None:
        before = census_check.report(self.census)["tested"]
        self.census["tools"]["scene.summary"]["tests"] = []
        after = census_check.report(self.census)
        self.assertLess(after["tested"], before)
        self.assertIn("scene.summary", after["tools_without_tests"])

    def test_tool_that_no_source_names_fails(self) -> None:
        self.census["tools"]["scene.ghost"] = {
            "origin": "worker",
            "toolset": "scene",
            "writes": False,
            "tests": [],
        }
        self.census["tools"]["toolsets.ghost"] = {
            "origin": "adapter",
            "toolset": "adapter",
            "writes": False,
            "tests": [],
        }
        problems = self.static()
        self.assert_reports(problems, "tool scene.ghost: no native or adapter source names it")
        self.assert_reports(problems, "tool toolsets.ghost: no native or adapter source names it")
        sources = census_check.derive_sources(self.census)
        self.assertTrue(
            any(name.endswith("automation_scene.cpp") for name in sources["scene.summary"])
        )
        self.assertEqual(sources["live.receipt"], ["tools/mcp/src/live-protocol.ts"])

    def test_report_names_the_source_revision_and_counts_native_evidence(self) -> None:
        revision = census_check.source_revision()
        self.assertRegex(revision, r"^[0-9a-f]{40}( \(uncommitted changes\))?$")
        text = census_check.format_report(census_check.report(self.census), revision)
        self.assertIn(f"source revision: {revision}", text)
        total = len(self.census["tools"])
        self.assertIn(f"named by a native or adapter source: {total} of {total}", text)
        with tempfile.TemporaryDirectory() as empty:
            self.assertEqual(
                census_check.source_revision(Path(empty)), "unknown (not a git checkout)"
            )
        self.census["tools"]["scene.ghost"] = {"origin": "worker", "tests": []}
        counts = census_check.report(self.census)
        self.assertEqual(counts["tools_without_source"], ["scene.ghost"])

    def test_executed_evidence_needs_a_passing_log_line(self) -> None:
        tests = self.census["tools"]["scene.summary"]["tests"]
        listed = next(t for t in tests if t.startswith(f"{census_check.ACCEPTANCE}::"))
        name = listed.removeprefix(f"{census_check.ACCEPTANCE}::")
        self.assertIsNone(census_check.report(self.census)["executed"])
        with tempfile.TemporaryDirectory() as scratch:
            log = Path(scratch) / "shard.log"
            passing = f"{name} (mcp_acceptance.NativeAcceptance.{name}) ... ok\n"
            log.write_text(passing, encoding="utf-8")
            passed = census_check.passed_tests([log])
            self.assertEqual(passed, {name})
            self.assertIn("scene.summary", census_check.executed_tools(self.census, passed))
            log.write_text(passing.replace("... ok", "... FAIL"), encoding="utf-8")
            failed = census_check.passed_tests([log])
            self.assertEqual(failed, set())
            self.assertNotIn("scene.summary", census_check.executed_tools(self.census, failed))
        counts = census_check.report(self.census, passed={name})
        self.assertLess(counts["executed"], counts["implemented"])
        self.assertGreater(counts["executed"], 0)


if __name__ == "__main__":
    unittest.main()
