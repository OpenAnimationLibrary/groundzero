"""Prove the revision check catches each way a writing tool can break the revision rule."""

from __future__ import annotations

import copy
import itertools
import sys
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_census
import mcp_revision_cases as cases
import mcp_revision_matrix as table
from mcp_worker import joined_capabilities

PROPERTIES = {"document_id": {}, "expected_revision": {}, "operation_id": {}, "delta": {}}
SCHEMAS = {
    name: {"inputSchema": {"properties": PROPERTIES}}
    for name in ("scene.create", "fake.set", "fake.get", "object.create", "scene.discard")
}


class FakeWorker:
    """A worker with one writing tool that can be told to break each part of the rule."""

    def __init__(self, *defects: str) -> None:
        self.defects = set(defects)
        self.documents: dict[str, dict[str, Any]] = {}
        self.receipts: dict[str, Any] = {}

    def call(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        handler = getattr(self, "do_" + method.replace(".", "_"))
        try:
            return {"id": "x", "result": handler(params)}
        except ValueError as error:
            return {"id": "x", "error": {"code": str(error), "message": "m", "details": {}}}

    def do_automation_list_documents(self, _params: dict[str, Any]) -> dict[str, Any]:
        listed = [{"document_id": k, "revision": d["revision"]} for k, d in self.documents.items()]
        return {"documents": listed}

    def do_scene_create(self, params: dict[str, Any]) -> dict[str, Any]:
        identifier = f"document-{len(self.documents)}"
        self.documents[identifier] = {"revision": "1", "value": 0, "objects": 0}
        return {"document_id": identifier, "revision": "1", "operation_id": params["operation_id"]}

    def do_scene_discard(self, params: dict[str, Any]) -> dict[str, Any]:
        del self.documents[params["document_id"]]
        return {}

    def do_fake_get(self, params: dict[str, Any]) -> dict[str, Any]:
        document = self.documents[params["document_id"]]
        shown = 0 if "read_blind" in self.defects else document["value"]
        return {"value": shown, "revision": document["revision"]}

    def do_object_create(self, params: dict[str, Any]) -> dict[str, Any]:
        document = self.documents[params["document_id"]]
        document["objects"] += 1
        document["revision"] = str(int(document["revision"]) + 1)
        if "late_retry_reapplies" in self.defects:
            self.receipts.clear()
        return {"revision": document["revision"]}

    def do_fake_set(self, params: dict[str, Any]) -> dict[str, Any]:
        operation = params["operation_id"]
        if operation in self.receipts:
            return self.receipts[operation]
        document = self.documents[params["document_id"]]
        if (
            params["expected_revision"] != document["revision"]
            and "stale_ignored" not in self.defects
        ):
            raise ValueError("revision_conflict")
        delta = params["delta"]
        if delta > 100:
            if "reject_changes_state" in self.defects:
                document["value"] += 1
                document["revision"] = str(int(document["revision"]) + 1)
            raise ValueError("other" if "wrong_code" in self.defects else "invalid_params")
        result = self._apply(document, delta)
        if "retry_reapplies" not in self.defects:
            self.receipts[operation] = result
        return result

    def _apply(self, document: dict[str, Any], delta: int) -> dict[str, Any]:
        changed = delta != 0
        if changed:
            document["value"] += delta
        if (
            changed
            and "no_bump" not in self.defects
            or not changed
            and "bump_on_noop" in self.defects
        ):
            document["revision"] = str(int(document["revision"]) + 1)
        reported = changed or "changed_lies" in self.defects
        return {"revision": document["revision"], "changed": reported}


def matrix(**overrides: Any) -> dict[str, Any]:
    entry = {
        "group": "fake",
        "kind": "document",
        "fixture": "base",
        "domain": "Fake",
        "class": "content",
        "changes": "a value",
        "bumps": "document revision",
        "reads": [{"tool": "fake.get"}],
        "success": {"args": {"delta": 5}},
        "reject": [{"args": {"delta": 500}, "code": "invalid_params"}],
        "noop": {"policy": "preserves", "args": {"delta": 0}, "documented": "x"},
    }
    entry.update(overrides)
    return {
        "fixtures": {"base": {"steps": [{"tool": "scene.create", "args": {"name": "n"}}]}},
        "tools": {"fake.set": entry},
        "known_violations": [],
    }


def run(defects: tuple[str, ...] = (), **overrides: Any) -> list[dict[str, str]]:
    worker = FakeWorker(*defects)
    session = cases.Session(worker, SCHEMAS, {}, itertools.count(1))
    return cases.run_tool(session, matrix(**overrides), "fake.set")


class RevisionRuleFailures(unittest.TestCase):
    def phases(self, *defects: str, **overrides: Any) -> set[str]:
        return {item["phase"] for item in run(defects, **overrides)}

    def test_a_tool_that_follows_the_rule_passes(self) -> None:
        self.assertEqual(run(), [])

    def test_success_without_a_revision_change_fails(self) -> None:
        found = run(("no_bump",))
        self.assertEqual({item["phase"] for item in found}, {"success"})
        self.assertTrue(any("revision advanced by 1" in item["expected"] for item in found))

    def test_success_that_no_read_shows_fails(self) -> None:
        self.assertIn("success", self.phases("read_blind"))

    def test_noop_that_advances_the_revision_fails(self) -> None:
        found = run(("bump_on_noop",))
        self.assertEqual({item["phase"] for item in found}, {"noop"})
        self.assertTrue(any(item["observed"] == "revision advanced by 1" for item in found))

    def test_noop_may_be_documented_to_advance(self) -> None:
        noop = {"policy": "bumps", "args": {"delta": 0}, "documented": "x"}
        self.assertEqual(run(("bump_on_noop",), noop=noop), [])
        self.assertIn("noop", self.phases(noop=noop))

    def test_changed_flag_that_contradicts_the_revision_fails(self) -> None:
        self.assertIn("noop", self.phases("changed_lies"))

    def test_rejected_call_that_changes_state_fails(self) -> None:
        found = run(("reject_changes_state",))
        self.assertEqual({item["phase"] for item in found}, {"reject"})
        self.assertTrue(any("revision advanced" in item["observed"] for item in found))
        self.assertTrue(any(item["observed"] == "read changed" for item in found))

    def test_rejection_with_another_error_code_fails(self) -> None:
        found = run(("wrong_code",))
        self.assertEqual(found[0]["observed"], "error other")

    def test_the_clients_schema_rejection_stands_in_for_invalid_params_only(self) -> None:
        def run_with(code: str) -> list[dict[str, str]]:
            worker = FakeWorker()
            original = worker.do_fake_set
            worker.do_fake_set = lambda p: (  # type: ignore[method-assign]
                (_ for _ in ()).throw(ValueError("INVALID_ARGUMENTS"))
                if p["delta"] > 100
                else original(p)
            )
            session = cases.Session(worker, SCHEMAS, {}, itertools.count(1), "INVALID_ARGUMENTS")
            data = matrix(reject=[{"args": {"delta": 500}, "code": code}])
            return cases.run_tool(session, data, "fake.set")

        self.assertEqual(run_with("invalid_params"), [])
        self.assertTrue(run_with("name_conflict"))

    def test_stale_revision_that_is_accepted_fails(self) -> None:
        found = run(("stale_ignored",))
        self.assertTrue(any(item["observed"] == "call succeeded" for item in found))

    def test_retry_that_runs_again_fails(self) -> None:
        self.assertIn("retry", self.phases("retry_reapplies"))

    def test_retry_after_a_later_write_that_runs_again_fails(self) -> None:
        self.assertIn("late retry", self.phases("late_retry_reapplies"))

    def test_successful_call_that_is_refused_fails(self) -> None:
        found = run(success={"args": {"delta": 500}})
        self.assertEqual(found[0]["observed"], "error invalid_params")


class FakeCase(unittest.TestCase):
    """The parts of the acceptance case that check_group uses."""

    def __init__(self, worker: FakeWorker) -> None:
        super().__init__("runTest")
        self.worker = worker
        self.workspace = Path("/unused")
        self.inputs = Path("/unused")
        self.fixture = Path("/unused")

    def runTest(self) -> None:  # noqa: N802 required name of the unittest default method
        return None

    def success(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        return self.worker.call(method, params)["result"]

    def capabilities(self) -> dict[str, Any]:
        return joined_capabilities(lambda params: self.success("automation.capabilities", params))

    def schema_code(self) -> str:
        return "INVALID_ARGUMENTS"


class KnownViolations(unittest.TestCase):
    def play(self, defects: tuple[str, ...], known: list[dict[str, str]]) -> None:
        table_data = matrix()
        table_data["known_violations"] = known
        worker = FakeWorker(*defects)
        case = FakeCase(worker)
        original = cases.load_matrix
        cases.load_matrix = lambda: table_data
        self.addCleanup(setattr, cases, "load_matrix", original)
        worker_call = worker.call
        catalog = {"tools": [{"name": n, **v} for n, v in SCHEMAS.items()], "next_offset": None}
        worker.call = lambda m, p: (  # type: ignore[method-assign]
            {"id": "x", "result": catalog} if m == "automation.capabilities" else worker_call(m, p)
        )
        cases.check_group(case, "fake")

    def recorded(self) -> list[dict[str, str]]:
        return [
            {
                "entry": "fake.set",
                "phase": "success",
                "expected": "changed is False",
                "observed": "changed is True",
            },
            {
                "entry": "fake.set",
                "phase": "success",
                "expected": "revision advanced by 1, read changed",
                "observed": "read changed",
            },
        ]

    def test_a_listed_defect_keeps_the_check_green(self) -> None:
        self.play(("no_bump",), self.recorded())

    def test_an_unlisted_defect_fails(self) -> None:
        with self.assertRaises(AssertionError):
            self.play(("no_bump",), [])

    def test_a_defect_that_no_longer_reproduces_fails(self) -> None:
        with self.assertRaises(AssertionError):
            self.play((), self.recorded())

    def test_a_different_defect_fails(self) -> None:
        with self.assertRaises(AssertionError):
            self.play(("bump_on_noop",), self.recorded())


class TableChecks(unittest.TestCase):
    def setUp(self) -> None:
        self.matrix = table.load_matrix()
        self.census = mcp_census.load_census()
        self.document = table.DOCUMENT_PATH.read_text(encoding="utf-8")
        self.acceptance = table.ACCEPTANCE_PATH.read_text(encoding="utf-8")

    def static(self) -> list[str]:
        return table.static_problems(self.matrix, self.census, self.document, self.acceptance)

    def assert_reports(self, problems: list[str], fragment: str) -> None:
        self.assertTrue(
            any(fragment in problem for problem in problems),
            f"expected a problem containing {fragment!r}, got {problems}",
        )

    def test_committed_table_passes(self) -> None:
        self.assertEqual(self.static(), [])

    def test_a_writing_tool_without_an_entry_fails(self) -> None:
        del self.matrix["tools"]["column.update"]
        self.assert_reports(
            self.static(), "writing tool column.update: has no revision table entry"
        )

    def test_missing_entry_message_names_the_file_and_an_example(self) -> None:
        self.census["tools"]["scene.invented"] = {
            "origin": "worker",
            "toolset": "core",
            "writes": True,
            "tests": [],
        }
        message = next(p for p in self.static() if p.startswith("writing tool scene.invented"))
        self.assertIn("ci-scripts/mcp_revision_matrix.json", message)
        self.assertIn('{"scene.invented": {"group"', message)
        for field in ('"success"', '"reject"', '"code"', '"noop"', "no_noop", '"fixture"'):
            self.assertIn(field, message)

    def test_an_entry_for_a_read_only_tool_fails(self) -> None:
        self.matrix["tools"]["scene.summary"] = copy.deepcopy(self.matrix["tools"]["column.update"])
        self.assert_reports(self.static(), "scene.summary is not a writing worker tool")

    def test_a_new_writing_tool_in_the_census_fails(self) -> None:
        self.census["tools"]["scene.invented"] = {
            "origin": "worker",
            "toolset": "core",
            "writes": True,
            "tests": [],
        }
        self.assert_reports(
            self.static(), "writing tool scene.invented: has no revision table entry"
        )

    def test_a_rejection_without_a_pinned_code_fails(self) -> None:
        del self.matrix["tools"]["column.update"]["reject"][0]["code"]
        self.assert_reports(self.static(), "does not pin its error code")

    def test_an_entry_without_a_rejection_fails(self) -> None:
        self.matrix["tools"]["column.update"]["reject"] = []
        self.assert_reports(self.static(), "needs at least one rejected call")

    def test_a_document_entry_needs_a_no_op_or_a_reason(self) -> None:
        del self.matrix["tools"]["column.update"]["noop"]
        self.assert_reports(self.static(), "needs exactly one of noop and no_noop")

    def test_a_document_entry_needs_an_observing_read(self) -> None:
        self.matrix["tools"]["column.update"]["reads"] = []
        self.assert_reports(self.static(), "needs a read that observes it")

    def test_a_skipped_success_needs_a_reason(self) -> None:
        entry = self.matrix["tools"]["assistance.neural.preview"]
        del entry["unexercised"]
        self.assert_reports(self.static(), "has no successful call and gives no reason")

    def test_an_unknown_fixture_fails(self) -> None:
        self.matrix["tools"]["column.update"]["fixture"] = "nowhere"
        self.assert_reports(self.static(), "unknown fixture 'nowhere'")

    def test_a_known_violation_must_name_an_entry_and_a_defect(self) -> None:
        self.matrix["known_violations"].append({"entry": "no.such", "tool": "no.such"})
        problems = self.static()
        self.assert_reports(problems, "known violation no.such: names no table entry")
        self.assert_reports(problems, "missing defect")

    def test_a_group_without_an_acceptance_method_fails(self) -> None:
        self.acceptance = self.acceptance.replace("def test_revision_matrix_fx(", "def other(")
        self.assert_reports(self.static(), "group fx: acceptance has no test_revision_matrix_fx")

    def test_a_stale_document_fails(self) -> None:
        self.document += "edited\n"
        self.assert_reports(self.static(), "revision coverage document is stale")

    def test_a_changed_table_makes_the_document_stale(self) -> None:
        self.matrix["tools"]["column.update"]["changes"] += " and more"
        self.assert_reports(self.static(), "revision coverage document is stale")

    def catalog(self) -> dict[str, Any]:
        tools = []
        for name, entry in self.census["tools"].items():
            if entry["origin"] == "worker":
                tool: dict[str, Any] = {
                    "name": name,
                    "annotations": {"readOnlyHint": not entry["writes"]},
                }
                if self.matrix["tools"].get(name, {}).get("kind") in table.OUTSIDE_KINDS:
                    tool["mutates_document"] = False
                tools.append(tool)
        return {"tools": tools}

    def test_a_catalog_that_matches_passes(self) -> None:
        self.assertEqual(table.check_live(self.matrix, self.catalog()), [])

    def test_a_new_writing_tool_in_the_catalog_fails(self) -> None:
        catalog = self.catalog()
        catalog["tools"].append({"name": "scene.invented", "annotations": {"readOnlyHint": False}})
        self.assert_reports(
            table.check_live(self.matrix, catalog), "scene.invented: has no table entry"
        )

    def test_a_tool_that_became_read_only_fails(self) -> None:
        catalog = self.catalog()
        for tool in catalog["tools"]:
            if tool["name"] == "column.update":
                tool["annotations"]["readOnlyHint"] = True
        self.assert_reports(
            table.check_live(self.matrix, catalog), "column.update: is not a writing tool"
        )

    def test_a_kind_that_contradicts_the_catalog_fails(self) -> None:
        catalog = self.catalog()
        for tool in catalog["tools"]:
            if tool["name"] == "column.update":
                tool["mutates_document"] = False
        self.assert_reports(table.check_live(self.matrix, catalog), "contradicts mutates_document")


if __name__ == "__main__":
    unittest.main()
