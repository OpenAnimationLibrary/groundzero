"""Prove the object, camera, group and output operations of the property model by hand."""

from __future__ import annotations

import collections
import sys
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_property_cameras as cams
import mcp_property_model as model
import mcp_property_objects as rig
import mcp_property_output as output
from mcp_property_curves import Key
from mcp_property_state import Column


def scene_with(columns: list[str], pegs: list[str] | None = None) -> model.Scene:
    scene = model.new_scene()
    for name in columns:
        rig.insert_column(scene, Column(name), len(scene.columns))
    for name in pegs or []:
        scene.pegs.append(name)
        scene.parents[name] = None
    return scene


def names(scene: model.Scene) -> list[str]:
    return [column.name for column in scene.columns]


class ColumnOrderTest(unittest.TestCase):
    def test_a_moved_block_keeps_its_order_and_goes_before_an_original_position(self) -> None:
        self.assertEqual(rig.moved_order(["Z", "A", "B", "C"], ["A", "B"], 4), ["Z", "C", "A", "B"])
        self.assertEqual(rig.moved_order(["A", "B", "C", "D"], ["C", "A"], 1), ["A", "C", "B", "D"])
        self.assertEqual(rig.moved_order(["A", "B", "C", "D"], ["A"], 3), ["B", "C", "A", "D"])
        self.assertEqual(rig.moved_order(["A", "B", "C"], ["B"], 1), ["A", "B", "C"])

    def test_move_refuses_a_position_past_the_end_and_a_column_named_twice(self) -> None:
        scene = scene_with(["A", "B"])
        self.assertEqual(
            rig.column_move_effect(scene, {"columns": ["A"], "before": 3}),
            "invalid_params/position",
        )
        self.assertEqual(
            rig.column_move_effect(scene, {"columns": ["A", "A"], "before": 0}),
            "invalid_params/duplicate_column",
        )
        self.assertEqual(names(scene), ["A", "B"])

    def test_insert_puts_a_column_at_a_position_and_refuses_one_past_the_end(self) -> None:
        scene = scene_with(["A", "B"])
        self.assertIsNone(rig.column_insert_effect(scene, {"name": "Z", "position": 0}))
        self.assertEqual(names(scene), ["Z", "A", "B"])
        self.assertEqual(
            rig.column_insert_effect(scene, {"name": "Y", "position": 4}), "invalid_params/position"
        )

    def test_a_duplicate_follows_its_source_and_copies_cells_and_settings(self) -> None:
        scene = scene_with(["A", "B"], ["P"])
        source = scene.column("A")
        source.cells[2] = ("L", (1, ""))
        source.opacity = 100
        scene.parents["A"] = "P"
        step = {"column": "A", "name": "Copy", "parent_policy": "same_parent"}
        self.assertIsNone(rig.column_duplicate_effect(scene, step))
        self.assertEqual(names(scene), ["A", "Copy", "B"])
        copy = scene.column("Copy")
        self.assertEqual((copy.cells, copy.opacity), ({2: ("L", (1, ""))}, 100))
        self.assertIsNot(copy.cells, source.cells)
        self.assertEqual(scene.parents["Copy"], "P")
        step = {"column": "A", "name": "Loose", "parent_policy": "table", "position": 0}
        rig.column_duplicate_effect(scene, step)
        self.assertEqual((names(scene)[0], scene.parents["Loose"]), ("Loose", None))


class ParentTest(unittest.TestCase):
    def parented(self, scene: model.Scene, name: str, parent: str, policy: str) -> str | None:
        step = {"object": name, "parent": parent, "policy": policy}
        return rig.parent_effect(scene, step)

    def test_preserve_local_changes_only_the_parent(self) -> None:
        scene = scene_with([], ["A", "B"])
        scene.curves[("A", "x")] = {0: Key(0, 2, "linear")}
        self.assertIsNone(self.parented(scene, "B", "A", "preserve_local"))
        self.assertEqual(scene.parents["B"], "A")
        self.assertEqual(rig.origin(scene, "B"), [0, 0])

    def test_preserve_world_shifts_the_objects_own_position_by_the_frame_it_leaves(self) -> None:
        scene = scene_with([], ["A", "B", "C"])
        scene.curves[("A", "x")] = {0: Key(0, 1, "linear")}
        scene.curves[("C", "x")] = {0: Key(0, 0.5, "linear"), 10: Key(10, 2.5, "linear")}
        self.assertIsNone(self.parented(scene, "C", "A", "preserve_world"))
        self.assertEqual([k.value for k in rig.ordered(scene.curves[("C", "x")])], [-0.5, 1.5])
        self.assertEqual(scene.defaults[("C", "x")], -1)
        self.assertNotIn(("C", "y"), scene.defaults)
        self.assertIsNone(self.parented(scene, "C", "B", "preserve_world"))
        self.assertEqual(rig.origin(scene, "C")[0], 0.5)

    def test_preserve_world_adds_the_new_parents_chain_up_to_the_common_ancestor(self) -> None:
        scene = scene_with([], ["A", "B", "C"])
        scene.defaults[("A", "y")] = 1.5
        scene.parents["B"] = "A"
        scene.defaults[("B", "y")] = 0.25
        self.assertIsNone(self.parented(scene, "C", "B", "preserve_world"))
        self.assertEqual(scene.defaults[("C", "y")], -1.75)
        self.assertIsNone(self.parented(scene, "C", "A", "preserve_world"))
        self.assertEqual(scene.defaults[("C", "y")], -1.5)

    def test_preserve_world_is_refused_when_a_parent_branch_is_animated(self) -> None:
        scene = scene_with([], ["A", "B"])
        scene.curves[("A", "y")] = {0: Key(0, 0, "linear"), 5: Key(5, 1, "linear")}
        self.assertEqual(self.parented(scene, "B", "A", "preserve_world"), "unsupported_target")
        self.assertIsNone(scene.parents["B"])
        self.assertIsNone(self.parented(scene, "B", "A", "preserve_local"))

    def test_a_rotated_frame_is_not_modelled(self) -> None:
        scene = scene_with([], ["A", "B"])
        scene.curves[("A", "rotation")] = {0: Key(0, 30, "linear")}
        with self.assertRaises(model.ModelError):
            self.parented(scene, "B", "A", "preserve_world")
        self.assertTrue(rig.tilted(scene, "B", "A"))

    def test_a_parent_below_the_object_or_the_object_itself_is_a_cycle(self) -> None:
        scene = scene_with([], ["A", "B"])
        scene.parents["B"] = "A"
        self.assertEqual(self.parented(scene, "A", "B", "preserve_local"), "cycle")
        self.assertEqual(self.parented(scene, "A", "A", "preserve_local"), "cycle")

    def test_a_locked_column_is_not_reparented(self) -> None:
        scene = scene_with(["C"], ["A"])
        scene.column("C").locked = True
        self.assertEqual(self.parented(scene, "C", "A", "preserve_local"), "locked")

    def test_the_same_parent_changes_nothing(self) -> None:
        scene = scene_with([], ["A", "B"])
        scene.parents["B"] = "A"
        scene.defaults[("A", "x")] = 4
        self.assertIsNone(self.parented(scene, "B", "A", "preserve_world"))
        self.assertNotIn(("B", "x"), scene.defaults)


class LifecycleTest(unittest.TestCase):
    def test_deleting_a_peg_with_children_needs_a_policy_and_moves_them_up(self) -> None:
        scene = scene_with(["C"], ["A", "B"])
        scene.parents["B"] = "A"
        scene.parents["C"] = "B"
        step = {"object": "B", "policy": "refuse"}
        self.assertEqual(rig.delete_effect(scene, step), "has_dependents")
        step["policy"] = "reattach_to_parent"
        self.assertIsNone(rig.delete_effect(scene, step))
        self.assertEqual((scene.pegs, scene.parents["C"]), (["A"], "A"))

    def test_reattaching_a_locked_child_is_refused(self) -> None:
        scene = scene_with(["C"], ["A"])
        scene.parents["C"] = "A"
        scene.column("C").locked = True
        step = {"object": "A", "policy": "reattach_to_parent"}
        self.assertEqual(rig.delete_effect(scene, step), "locked")
        self.assertEqual(scene.pegs, ["A"])

    def test_rename_moves_curves_parents_and_the_column(self) -> None:
        scene = scene_with(["C"], ["A"])
        scene.parents["C"] = "A"
        scene.curves[("A", "x")] = {0: Key(0, 1, "linear")}
        self.assertIsNone(rig.rename_effect(scene, {"object": "A", "name": "Z"}))
        self.assertEqual((scene.pegs, scene.parents["C"]), (["Z"], "Z"))
        self.assertIn(("Z", "x"), scene.curves)
        self.assertIsNone(rig.rename_effect(scene, {"object": "C", "name": "D"}))
        self.assertEqual((names(scene), scene.parents["D"]), (["D"], "Z"))
        scene.column("D").locked = True
        self.assertEqual(rig.rename_effect(scene, {"object": "D", "name": "E"}), "locked")

    def test_duplicating_a_peg_copies_its_curves_defaults_and_parent_independently(self) -> None:
        scene = scene_with([], ["A", "B"])
        scene.parents["B"] = "A"
        scene.curves[("B", "x")] = {0: Key(0, 1, "linear")}
        scene.cycles.add(("B", "x"))
        scene.defaults[("B", "y")] = 2
        step = {"object": "B", "name": "B2", "scope": "object"}
        self.assertIsNone(rig.duplicate_effect(scene, step))
        scene.curves[("B", "x")][0].value = 9
        self.assertEqual(scene.curves[("B2", "x")][0].value, 1)
        self.assertEqual((scene.parents["B2"], scene.defaults[("B2", "y")]), ("A", 2))
        self.assertIn(("B2", "x"), scene.cycles)

    def test_a_column_is_copied_to_the_end_by_subtree_and_never_by_object(self) -> None:
        scene = scene_with(["C", "D"])
        step = {"object": "C", "name": "C2", "scope": "subtree", "level_sharing": "shared"}
        self.assertIsNone(rig.duplicate_effect(scene, step))
        self.assertEqual(names(scene), ["C", "D", "C2"])
        step = {"object": "C", "name": "C3", "scope": "object"}
        self.assertEqual(rig.duplicate_effect(scene, step), "unsupported_kind")
        step = {"object": "C", "name": "C3", "scope": "subtree"}
        self.assertEqual(rig.duplicate_effect(scene, step), "invalid_params")


class GroupTest(unittest.TestCase):
    def group(self, scene: model.Scene, objects: list[str], name: str = "G") -> str | None:
        step = {"objects": objects, "name": name, "key": scene.group_counter + 1}
        return rig.group_effect(scene, step)

    def test_groups_nest_and_only_whole_groups_are_grouped_again(self) -> None:
        scene = scene_with([], ["A", "B", "C"])
        self.assertIsNone(self.group(scene, ["A", "B"], "inner"))
        self.assertEqual(self.group(scene, ["A", "C"]), "invalid_params")
        self.assertIsNone(self.group(scene, ["A", "B", "C"], "outer"))
        record = rig.expected_objects(scene)["groups"]
        self.assertEqual(
            record,
            {
                "1": {"name": "inner", "members": ["A", "B"], "parent": "2"},
                "2": {"name": "outer", "members": ["A", "B", "C"], "parent": None},
            },
        )

    def test_only_an_outermost_group_is_dissolved(self) -> None:
        scene = scene_with([], ["A", "B", "C"])
        self.group(scene, ["A", "B"])
        self.group(scene, ["A", "B", "C"])
        self.assertEqual(rig.ungroup_effect(scene, {"group": 1}), "invalid_params")
        self.assertIsNone(rig.ungroup_effect(scene, {"group": 2}))
        self.assertIsNone(rig.ungroup_effect(scene, {"group": 1}))
        with self.assertRaises(model.ModelError):
            rig.ungroup_effect(scene, {"group": 1})
        self.assertEqual(rig.expected_objects(scene)["groups"], {})

    def test_an_object_named_twice_is_refused_and_a_deleted_member_leaves_the_group(self) -> None:
        scene = scene_with([], ["A", "B"])
        self.assertEqual(self.group(scene, ["A", "A"]), "invalid_params")
        self.assertIsNone(self.group(scene, ["A", "B"]))
        rig.delete_effect(scene, {"object": "A", "policy": "refuse"})
        self.assertEqual(rig.expected_objects(scene)["groups"]["1"]["members"], ["B"])
        rig.delete_effect(scene, {"object": "B", "policy": "refuse"})
        self.assertEqual(rig.expected_objects(scene)["groups"], {})

    def test_rename_changes_the_name_of_a_group_with_members(self) -> None:
        scene = scene_with([], ["A", "B"])
        self.group(scene, ["A", "B"])
        self.assertIsNone(rig.group_rename_effect(scene, {"group": 1, "name": "new"}))
        self.assertEqual(scene.group_names[1], "new")
        rig.delete_effect(scene, {"object": "A", "policy": "refuse"})
        rig.delete_effect(scene, {"object": "B", "policy": "refuse"})
        self.assertEqual(rig.group_rename_effect(scene, {"group": 1, "name": "x"}), "not_found")
        self.assertEqual(rig.ungroup_effect(scene, {"group": 1}), "not_found")
        with self.assertRaises(model.ModelError):
            rig.group_rename_effect(scene, {"group": 7, "name": "x"})


class CameraTest(unittest.TestCase):
    SIZE = {"width_px": 64, "height_px": 32, "width_in": 1, "height_in": 0.5}

    def made(self, scene: model.Scene, name: str) -> None:
        self.assertIsNone(cams.create_effect(scene, {"name": name, **self.SIZE}))

    def test_a_new_camera_has_no_role_and_a_reserved_or_taken_name_is_refused(self) -> None:
        scene = model.new_scene()
        self.made(scene, "Side")
        self.assertEqual(
            [(c["name"], c["roles"]) for c in cams.expected_cameras(scene)],
            [("Camera1", ["output", "preview"]), ("Side", [])],
        )
        reserved = cams.create_effect(scene, {"name": "Camera7", **self.SIZE})
        self.assertEqual(reserved, "invalid_params/reserved_name")
        self.assertEqual(cams.create_effect(scene, {"name": "Side", **self.SIZE}), "name_conflict")

    def test_select_moves_the_roles_and_the_scene_settings_follow_the_output_camera(self) -> None:
        scene = model.new_scene()
        self.made(scene, "Wide")
        cams.select_effect(scene, {"camera": "Wide", "roles": ["output"]})
        self.assertEqual(scene.camera["width_px"], 64)
        scene.camera["width_px"] = 8
        self.assertEqual(cams.camera_named(scene, "Wide").size["width_px"], 8)
        self.assertEqual(cams.camera_named(scene, "Camera1").size["width_px"], 32)
        step = {"camera": "Wide", "roles": ["preview", "preview"]}
        self.assertEqual(cams.select_effect(scene, step), "invalid_params/roles")

    def test_a_duplicate_copies_the_size_and_none_of_the_roles(self) -> None:
        scene = model.new_scene()
        step = {"camera": "Camera1", "name": "Copy", "parent_policy": "none"}
        self.assertIsNone(cams.duplicate_effect(scene, step))
        copy = cams.camera_named(scene, "Copy")
        self.assertEqual(copy.size, scene.camera)
        self.assertIsNot(copy.size, scene.camera)
        self.assertEqual(cams.held_roles(scene, copy), [])

    def test_removing_the_camera_with_both_roles_needs_a_replacement_for_each(self) -> None:
        scene = model.new_scene()
        for name in ("Middle", "Last"):
            self.made(scene, name)
        cams.select_effect(scene, {"camera": "Middle", "roles": ["output", "preview"]})
        step = {"camera": "Middle", "replacement": {}}
        self.assertEqual(cams.remove_effect(scene, step), "in_use/role")
        step = {"camera": "Middle", "replacement": {"output": "Last"}}
        self.assertEqual(cams.remove_effect(scene, step), "in_use/role")
        step = {"camera": "Last", "replacement": {"output": "Camera1"}}
        self.assertEqual(cams.remove_effect(scene, step), "invalid_params/replacement")
        step = {"camera": "Middle", "replacement": {"output": "Middle", "preview": "Last"}}
        self.assertEqual(cams.remove_effect(scene, step), "invalid_params/replacement")
        step = {"camera": "Middle", "replacement": {"output": "Last", "preview": "Camera1"}}
        self.assertIsNone(cams.remove_effect(scene, step))
        self.assertEqual([c.name for c in scene.cameras], ["Camera1", "Last"])
        self.assertEqual(scene.camera, cams.camera_named(scene, "Last").size)
        self.assertEqual(
            [c["roles"] for c in cams.expected_cameras(scene)], [["preview"], ["output"]]
        )

    def test_the_last_camera_is_never_removed(self) -> None:
        scene = model.new_scene()
        step = {"camera": "Camera1", "replacement": {}}
        self.assertEqual(cams.remove_effect(scene, step), "in_use/last_camera")


class OutputTest(unittest.TestCase):
    def test_blocks_change_only_where_named_and_keep_what_a_step_leaves_out(self) -> None:
        scene = model.new_scene()
        letterbox = {"mode": "letterbox", "ratio": 1.5, "color": {"r": 1, "g": 2, "b": 3, "a": 4}}
        step = {
            "apply_to": ["output"],
            "settings": {"step": 2, "aspect_export": letterbox, "range": {"kind": "all"}},
        }
        self.assertIsNone(output.set_effect(scene, step))
        step = {"apply_to": ["preview"], "settings": {"shrink": 3}, "sync": True}
        self.assertIsNone(output.set_effect(scene, step))
        step = {"apply_to": ["output"], "settings": {"aspect_export": {"mode": "none"}}}
        self.assertIsNone(output.set_effect(scene, step))
        record = output.expected_properties(scene, 5)
        self.assertEqual(
            record["output"],
            {
                "range": {"kind": "all"},
                "effective_range": {"start_frame": 0, "end_frame": 4},
                "step": 2,
                "aspect_export": {"mode": "none", "ratio": 1.5, "color": letterbox["color"]},
            },
        )
        self.assertEqual(record["preview"], {"shrink": 3})
        self.assertTrue(record["sync_color_settings"])

    def test_a_range_is_cut_to_the_scene_and_empty_when_it_starts_after_the_last_frame(
        self,
    ) -> None:
        block = {"range": {"kind": "frames", "start_frame": 2, "end_frame": 9}}
        self.assertEqual(output.effective_range(block, 5), {"start_frame": 2, "end_frame": 4})
        self.assertIsNone(output.effective_range(block, 2))
        self.assertIsNone(output.effective_range({"range": {"kind": "all"}}, 0))

    def test_the_refusals_come_in_the_workers_order(self) -> None:
        scene = model.new_scene()
        reversed_range = {"kind": "frames", "start_frame": 5, "end_frame": 1}
        cases = [
            ({"apply_to": ["output"]}, "invalid_params/nothing_to_set"),
            (
                {"apply_to": ["output", "output"], "settings": {"step": 1}},
                "invalid_params/apply_to",
            ),
            (
                {"apply_to": ["output"], "settings": {"step": 1}, "sync": True},
                "invalid_params/sync_color_settings",
            ),
            (
                {"apply_to": ["preview"], "settings": {"range": reversed_range}},
                "invalid_params/range",
            ),
        ]
        for step, code in cases:
            self.assertEqual(output.set_effect(scene, step), code)
        self.assertEqual(output.expected_properties(scene, 0), {"output": {}, "preview": {}})

    def test_guides_are_replaced_by_name_and_an_empty_call_is_refused(self) -> None:
        scene = model.new_scene()
        self.assertEqual(output.guides_effect(scene, {}), "invalid_params/nothing_to_set")
        self.assertEqual(
            output.guides_effect(scene, {"field": {}}), "invalid_params/nothing_to_set"
        )
        output.guides_effect(scene, {"field": {"size": 4}, "markers": {"interval": 6}})
        output.guides_effect(scene, {"field": {"aspect_ratio": 1.5}})
        ruler = {"vertical_lines_x_in": [1.0, -2.0], "horizontal_lines_y_in": []}
        output.guides_effect(scene, {"ruler": ruler})
        record = output.expected_properties(scene, 0)
        self.assertEqual(record["field_guide"], {"size": 4, "aspect_ratio": 1.5})
        self.assertEqual(record["markers"], {"interval": 6})
        self.assertEqual(record["ruler_guides"], ruler)

    def test_projection_keeps_only_the_expected_entries(self) -> None:
        seen = {"output": {"step": 1, "bpp": 32}, "x": 1}
        self.assertEqual(output.project(seen, {"output": {"step": 0}}), {"output": {"step": 1}})


class RegistrationTest(unittest.TestCase):
    TOOLS = {
        "column.move", "column.duplicate", "camera.list", "camera.create", "camera.duplicate",
        "camera.select", "camera.remove", "scene.output.set", "scene.guides.set",
        "scene.properties.inspect", "object.duplicate", "object.rename", "object.delete",
        "object.group", "object.group.rename", "object.ungroup", "rig.dependents",
        "object.groups",
    }  # fmt: skip

    def everything(self) -> Any:
        tools = {*model.TOOLS, *self.TOOLS}
        stack = mock.patch.dict(model.OPERATIONS)
        patched = mock.patch.object(model, "TOOLS", tools)
        stack.start()
        patched.start()
        self.addCleanup(stack.stop)
        self.addCleanup(patched.stop)
        model._register((rig, cams, output))

    def test_an_operation_waits_for_its_tools(self) -> None:
        without = mock.patch.object(model, "TOOLS", model.TOOLS - self.TOOLS)
        with mock.patch.dict(model.OPERATIONS, clear=True), without:
            model._register((rig, cams, output))
            self.assertNotIn("camera_create", model.OPERATIONS)
        self.everything()
        for name in ("camera_create", "column_move", "group_create", "output_set", "object_delete"):
            self.assertIn(name, model.OPERATIONS)

    def test_every_generated_step_applies_to_the_model_and_builds_a_request(self) -> None:
        self.everything()
        handles: Any = collections.defaultdict(lambda: collections.defaultdict(lambda: "handle"))
        used: set[str] = set()
        for seed in range(1, 120):
            for step in model.generate(seed, 30):
                tool, params = model.OPERATIONS[step["op"]].request(step["args"], handles)
                used.add(step["op"])
                self.assertIsInstance(params, dict)
        new = {"column_insert", "column_move", "column_duplicate", "object_parent", "object_rename"}
        new |= {
            "object_duplicate",
            "object_delete",
            "group_create",
            "group_rename",
            "group_ungroup",
        }
        new |= {"camera_create", "camera_duplicate", "camera_select", "camera_remove"}
        new |= {"output_set", "guides_set"}
        self.assertEqual(new - used, set())

    def test_the_expected_observation_names_what_the_new_tools_read(self) -> None:
        self.everything()
        record = model.expected(model.new_scene())
        for key in ("cameras", "properties", "parents", "origins", "groups"):
            self.assertIn(key, record)


if __name__ == "__main__":
    unittest.main()
