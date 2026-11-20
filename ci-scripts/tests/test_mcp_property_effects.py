"""Prove the effect-channel part of the property model by hand-checked bounds and its cover."""

from __future__ import annotations

import collections
import json
import random
import sys
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_property_cases as cases
import mcp_property_effects as effects
import mcp_property_model as model
from mcp_property_curves import Key

CENSUS = json.loads((Path(__file__).resolve().parents[1] / "mcp_census.json").read_text())


def speed_segment(out_value: float, in_value: float) -> dict[int, Key]:
    first = Key(0, 100, "speed_in_out", outgoing=("speed", 4, out_value), linked=False)
    last = Key(12, 150, "linear", incoming=("speed", -4, in_value), linked=False)
    return {0: first, 12: last}


class BoundsTest(unittest.TestCase):
    def test_keys_and_handles_inside_are_admitted(self) -> None:
        self.assertTrue(effects.in_bounds(speed_segment(100, -150)))

    def test_an_outgoing_handle_past_the_top_is_refused(self) -> None:
        self.assertFalse(effects.in_bounds(speed_segment(100.25, 0)))

    def test_an_incoming_handle_below_the_bottom_is_refused(self) -> None:
        self.assertFalse(effects.in_bounds(speed_segment(0, -150.25)))

    def test_a_key_value_outside_is_refused(self) -> None:
        self.assertFalse(effects.in_bounds({0: Key(0, -0.25, "linear")}))

    def test_ease_and_constant_segments_have_no_handle_points(self) -> None:
        first = Key(0, 200, "ease_in_out", outgoing=("ease", 6), linked=False)
        last = Key(12, 0, "constant", incoming=("ease", 6), linked=False)
        self.assertTrue(effects.in_bounds({0: first, 12: last}))

    def test_a_refused_edit_leaves_the_curve(self) -> None:
        scene = model.new_scene()
        scene.effects.append("F1")
        step = {
            "fx": "F1",
            "keys": [{"frame": 0, "value": 260, "interpolation": "linear", "step": 1}],
        }
        self.assertEqual(
            effects.EFFECT_OPERATIONS["fx_keys_set"].effect(scene, step), "OUT_OF_RANGE"
        )
        self.assertEqual(scene.curves.get(("F1", "value"), {}), {})


class GenerationTest(unittest.TestCase):
    def features(self, seed: int) -> set[str]:
        scene = model.new_scene()
        found: set[str] = set()
        for step in effects.generate_effects(seed, cases.DEFAULT_LENGTH):
            keyed = any(scene.curves.values())
            error = effects.EFFECT_OPERATIONS[step["op"]].effect(scene, step["args"])
            found.add(step["op"])
            if error:
                found.add("OUT_OF_RANGE" if error == "OUT_OF_RANGE" else "curve_rule")
            elif step["op"] == "save_reopen" and keyed:
                found.add("reopened_keys")
        return found

    def test_a_seed_gives_the_same_sequence(self) -> None:
        self.assertEqual(effects.generate_effects(7, 24), effects.generate_effects(7, 24))

    def test_every_generated_step_applies_to_the_model(self) -> None:
        for seed in range(1, 80):
            scene = model.new_scene()
            for step in effects.generate_effects(seed, 24):
                effects.EFFECT_OPERATIONS[step["op"]].effect(scene, step["args"])

    def test_every_operation_is_chosen_and_out_of_range_is_predicted(self) -> None:
        chosen: collections.Counter[str] = collections.Counter()
        for seed in range(1, 120):
            for feature in self.features(seed):
                chosen[feature] += 1
        self.assertEqual(set(effects.EFFECT_OPERATIONS) - set(chosen), set())
        self.assertGreater(chosen["OUT_OF_RANGE"], 10)
        self.assertGreater(chosen["curve_rule"], 10)

    def test_default_seeds_cover_every_operation_and_refusal(self) -> None:
        covered: set[str] = set()
        for seed in cases.EFFECT_SEEDS:
            covered |= self.features(seed)
        wanted = set(effects.EFFECT_OPERATIONS) | {"OUT_OF_RANGE", "curve_rule", "reopened_keys"}
        self.assertEqual(wanted - covered, set())

    def test_every_operation_calls_a_tool_the_census_lists(self) -> None:
        handles: Any = collections.defaultdict(lambda: collections.defaultdict(lambda: "handle"))
        used = set()
        for seed in range(1, 40):
            for step in effects.generate_effects(seed, 24):
                tool, params = effects.EFFECT_OPERATIONS[step["op"]].request(step["args"], handles)
                used.add(tool)
                if tool.startswith("animation."):
                    self.assertEqual(params["fx_id"], "handle")
                    self.assertNotIn("object_id", params)
        self.assertEqual(used - set(CENSUS["tools"]), set())

    def test_effect_and_stage_operations_do_not_share_a_name_by_accident(self) -> None:
        shared = set(effects.EFFECT_OPERATIONS) & set(model.OPERATIONS)
        self.assertEqual(shared, {"save_reopen"})
        self.assertIs(effects.EFFECT_OPERATIONS["save_reopen"], model.OPERATIONS["save_reopen"])

    def test_stage_sequences_never_choose_an_effect(self) -> None:
        rng = random.Random(3)
        for seed in (rng.randrange(1000) for _ in range(20)):
            for step in model.generate(seed, 24):
                self.assertNotIn(step["op"], set(effects.EFFECT_OPERATIONS) - {"save_reopen"})


if __name__ == "__main__":
    unittest.main()
