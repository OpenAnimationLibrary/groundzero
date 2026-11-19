"""Prove the property model computes by hand-checked values and that its table is complete."""

from __future__ import annotations

import collections
import json
import sys
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_property_cases as cases
import mcp_property_model as model

CENSUS = json.loads((Path(__file__).resolve().parents[1] / "mcp_census.json").read_text())


def keys(*rows: tuple[int, float, str, int]) -> list[model.Key]:
    return [model.Key(*row) for row in rows]


class SampleTest(unittest.TestCase):
    def test_holds_before_the_first_key_and_after_the_last(self) -> None:
        curve = keys((6, 1, "linear", 1), (12, 3, "linear", 1))
        self.assertEqual([model.sample(curve, f) for f in (0, 6, 12, 99)], [1, 1, 3, 3])

    def test_linear_follows_fractions_when_the_step_is_one(self) -> None:
        curve = keys((0, 0, "linear", 1), (12, 12, "linear", 1))
        self.assertAlmostEqual(model.sample(curve, 5.5), 5.5)

    def test_linear_holds_each_step_frames(self) -> None:
        curve = keys((0, 0, "linear", 3), (12, 12, "linear", 1))
        values = [model.sample(curve, f) for f in (0, 2.99, 3, 5.99, 6, 11.99, 12)]
        self.assertEqual(values, [0, 0, 3, 3, 6, 9, 12])

    def test_constant_holds_until_the_next_key(self) -> None:
        curve = keys((0, 4, "constant", 1), (6, 9, "linear", 1))
        self.assertEqual([model.sample(curve, f) for f in (0, 5.99, 6)], [4, 4, 9])

    def test_unsorted_keys_are_ordered(self) -> None:
        curve = keys((6, 3, "linear", 1), (0, 1, "linear", 1))
        self.assertEqual(model.sample(curve, 3), 2)

    def test_opacity_rounds_half_up_to_eight_bits(self) -> None:
        self.assertEqual([model.quantized_opacity(v) for v in (0, 0.5, 0.3, 1)], [0, 128, 77, 255])


class GenerationTest(unittest.TestCase):
    def test_a_seed_gives_the_same_sequence(self) -> None:
        self.assertEqual(model.generate(7, 24), model.generate(7, 24))
        self.assertNotEqual(model.generate(7, 24), model.generate(8, 24))

    def test_a_prefix_of_a_longer_sequence_is_the_shorter_sequence(self) -> None:
        self.assertEqual(model.generate(5, 24)[:10], model.generate(5, 10))

    def test_every_generated_step_applies_to_the_model(self) -> None:
        for seed in range(1, 60):
            scene = model.new_scene()
            for step in model.generate(seed, 24):
                model.OPERATIONS[step["op"]].effect(scene, step["args"])

    def test_every_operation_is_chosen_and_every_refusal_is_predicted(self) -> None:
        chosen: collections.Counter[str] = collections.Counter()
        for seed in range(1, 120):
            scene = model.new_scene()
            for step in model.generate(seed, 24):
                chosen[step["op"]] += 1
                error = model.OPERATIONS[step["op"]].effect(scene, step["args"])
                chosen[f"error:{error}"] += bool(error)
        self.assertEqual({name for name in model.OPERATIONS} - set(chosen), set())
        for code in ("locked", "occupied_cells", "empty_level"):
            self.assertGreater(chosen[f"error:{code}"], 0, code)

    def test_a_step_that_names_nothing_is_not_applicable(self) -> None:
        scene = model.new_scene()
        with self.assertRaises(model.ModelError):
            model.OPERATIONS["keys_set"].effect(scene, {"peg": "none", "channel": "x", "keys": []})

    def test_every_operation_calls_a_tool_the_census_lists(self) -> None:
        handles: Any = collections.defaultdict(lambda: collections.defaultdict(lambda: "handle"))
        used: set[str] = set()
        for seed in range(1, 40):
            for step in model.generate(seed, 24):
                tool, params = model.OPERATIONS[step["op"]].request(step["args"], handles)
                used.add(tool)
                self.assertIsInstance(params, dict)
        self.assertEqual(used - set(CENSUS["tools"]), set(), "an operation calls an unknown tool")
        self.assertGreaterEqual(len(used), 10)


class ExpectedTest(unittest.TestCase):
    def test_reopen_drops_trailing_empty_columns_only(self) -> None:
        scene = model.new_scene()
        for name in ("a", "b", "c"):
            scene.columns.append(model.Column(name))
        scene.columns[1].cells[0] = ("L", (1, ""))
        model.after_reopen(scene)
        self.assertEqual([column.name for column in scene.columns], ["a", "b"])

    def test_the_frame_count_is_the_last_cell_plus_one(self) -> None:
        scene = model.new_scene()
        column = model.Column("a")
        column.cells.update({3: ("L", (1, "")), 9: ("L", (1, ""))})
        scene.columns.append(column)
        self.assertEqual(model.expected(scene)["frame_count"], 10)


class DifferencesTest(unittest.TestCase):
    def test_equal_observations_have_no_difference(self) -> None:
        value = {"a": [1, 2.0, {"b": "x"}], "c": None}
        self.assertEqual(cases.differences(value, json.loads(json.dumps(value))), [])

    def test_a_number_beyond_the_tolerance_is_reported_with_its_path(self) -> None:
        found = cases.differences({"a": [1.0, 2.0]}, {"a": [1.0, 2.0000001]})
        self.assertEqual(len(found), 1)
        self.assertIn("/a[1]", found[0])

    def test_missing_extra_and_shorter_values_are_reported(self) -> None:
        self.assertEqual(len(cases.differences({"a": 1}, {"b": 1})), 2)
        self.assertEqual(len(cases.differences([1, 2], [1])), 1)
        self.assertEqual(len(cases.differences("x", "y")), 1)

    def test_a_boolean_is_not_a_number(self) -> None:
        self.assertEqual(len(cases.differences(True, 1)), 1)


class FakeCase:
    operations = 0


FAKE: Any = FakeCase()


class ShrinkTest(unittest.TestCase):
    def replay(self, case: Any, steps: list[dict[str, Any]]) -> cases.Failure | None:
        names = [step["op"] for step in steps]
        if "keys_set" in names and "peg_create" in names[: names.index("keys_set")]:
            return cases.Failure(names.index("keys_set"), "keys_set", ["differs"])
        return None

    def test_the_shortest_prefix_and_then_the_fewest_steps_remain(self) -> None:
        ops = ["level_create", "peg_create", "camera_update", "keys_set", "scene_settings"]
        steps = [{"op": name, "args": {}} for name in ops]
        failure = self.replay(None, steps[:4])
        assert failure is not None
        with mock.patch.object(cases, "replay", self.replay):
            smallest, found = cases.shrink(FAKE, steps, failure)
        self.assertEqual([step["op"] for step in smallest], ["peg_create", "keys_set"])
        self.assertEqual(found.operation, "keys_set")

    def test_a_failure_that_does_not_repeat_is_called_not_reproducible(self) -> None:
        steps = [{"op": "peg_create", "args": {}}]
        failure = cases.Failure(0, "peg_create", ["x"])
        with mock.patch.object(cases, "replay", lambda case, steps: None):
            _kept, found = cases.shrink(FAKE, steps, failure)
        self.assertTrue(found.messages[0].startswith("not reproducible"))


if __name__ == "__main__":
    unittest.main()
