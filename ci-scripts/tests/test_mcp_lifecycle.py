"""Prove each way the lifecycle grid can drift makes its check fail."""

from __future__ import annotations

import copy
import sys
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_lifecycle as lifecycle
from mcp_census import load_census


def family(grid: dict[str, Any], name: str) -> dict[str, Any]:
    return next(item for item in grid["families"] if item["id"] == name)


class LifecycleTest(unittest.TestCase):
    def setUp(self) -> None:
        self.census = load_census()
        self.grid = copy.deepcopy(lifecycle.load_grid())

    def problems(self) -> list[str]:
        return lifecycle.check(self.grid, self.census)

    def test_the_committed_grid_is_complete(self) -> None:
        self.assertEqual(self.problems(), [])

    def test_every_listed_family_has_all_eight_cells(self) -> None:
        for item in self.grid["families"]:
            self.assertEqual(set(item["cells"]), set(lifecycle.OPERATIONS), item["id"])

    def test_a_missing_cell_names_its_family_and_operation(self) -> None:
        del family(self.grid, "level")["cells"]["remove"]
        found = self.problems()
        self.assertEqual(len(found), 1)
        self.assertIn("family level lacks a cell for remove", found[0])

    def test_a_new_family_needs_every_cell(self) -> None:
        cells = {"create": {"tools": ["column.create"]}}
        self.grid["families"].append({"id": "stamp", "name": "Stamp", "cells": cells})
        found = self.problems()
        self.assertEqual(sum("lacks a cell" in problem for problem in found), 7)

    def test_a_tool_that_is_not_in_the_census_is_reported(self) -> None:
        family(self.grid, "column")["cells"]["remove"] = {"tools": ["column.vanish"]}
        self.assertTrue(any("column.vanish is not in the census" in p for p in self.problems()))

    def test_a_write_tool_in_no_cell_is_reported(self) -> None:
        for item in self.grid["families"]:
            for cell in item["cells"].values():
                if "tools" in cell and "hook.delete" in cell["tools"]:
                    cell["tools"].remove("hook.delete")
                    cell["tools"] = cell["tools"] or ["hook.inspect"]
        found = [problem for problem in self.problems() if "hook.delete" in problem]
        self.assertEqual(len(found), 1)
        self.assertIn("is in no lifecycle cell", found[0])

    def test_a_tool_listed_outside_and_in_a_cell_is_reported(self) -> None:
        self.grid["outside"]["column.delete"] = "not a resource"
        self.assertTrue(any("also in a family cell" in p for p in self.problems()))

    def test_a_planned_cell_needs_a_planned_census_outcome(self) -> None:
        cell = {"disposition": "planned", "outcome": "level.create_empty"}
        family(self.grid, "level")["cells"]["remove"] = cell
        self.assertTrue(any("is implemented in the census" in p for p in self.problems()))
        cell["outcome"] = "no.such.outcome"
        self.assertTrue(any("is not one" in p for p in self.problems()))

    def test_unsupported_restricted_and_no_undo_cells_need_a_reason(self) -> None:
        for kind in ("unsupported", "restricted", "no_undo"):
            family(self.grid, "hook")["cells"]["reorder"] = {"disposition": kind, "reason": " "}
            self.assertTrue(any(f"a {kind} cell needs a reason" in p for p in self.problems()))

    def test_a_cell_with_neither_tools_nor_disposition_is_reported(self) -> None:
        family(self.grid, "hook")["cells"]["reorder"] = {"reason": "none"}
        self.assertTrue(any("name the tools that do it" in p for p in self.problems()))

    def test_persist_and_recover_need_evidence_that_exercises_the_tools(self) -> None:
        cell = family(self.grid, "camera")["cells"]["persist"]
        del cell["evidence"]
        self.assertTrue(any("needs `evidence`" in p for p in self.problems()))
        cell["evidence"] = "mcp_no_such_cases.py"
        self.assertTrue(any("does not exist" in p for p in self.problems()))
        cell["evidence"] = "mcp_lifecycle.py"
        self.assertTrue(any("never name scene.save_copy" in p for p in self.problems()))

    def test_evidence_must_name_one_of_the_familys_own_tools(self) -> None:
        cell = family(self.grid, "palette_page")["cells"]["recover"]
        cell["evidence"] = "mcp_recipe_cases.py"
        self.assertTrue(any("name none of the family's tools" in p for p in self.problems()))

    def test_a_family_listed_twice_is_reported(self) -> None:
        self.grid["families"].append(copy.deepcopy(family(self.grid, "level")))
        self.assertTrue(any("level is listed twice" in p for p in self.problems()))

    def test_the_operations_are_fixed(self) -> None:
        self.grid["operations"] = ["inspect", "create"]
        self.assertTrue(any("operations must be exactly" in p for p in self.problems()))

    def test_the_document_has_a_row_per_family_and_a_gap_count_per_operation(self) -> None:
        text = lifecycle.render_document(self.grid, self.census)
        for item in self.grid["families"]:
            self.assertIn(f"| {item['name']} |", text)
        for operation in lifecycle.OPERATIONS:
            self.assertIn(f"| {operation} |", text)
        self.assertIn("not kept in the repository", text)

    def test_a_tool_the_live_worker_lacks_is_reported(self) -> None:
        names = {
            tool
            for item in self.grid["families"]
            for c in item["cells"].values()
            for tool in c.get("tools", [])
        }
        live = {"tools": [{"name": name} for name in names if name != "column.delete"]}
        self.assertEqual(
            lifecycle.check_live(self.grid, live), ["the live worker has no tool column.delete"]
        )


if __name__ == "__main__":
    unittest.main()
