"""Exercise the shipped native worker with real scenes, assets, and rendered pixels."""

from __future__ import annotations

import argparse
import base64
import functools
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

import mcp_animation_cases
import mcp_artwork_cases
import mcp_assistance_cases
import mcp_audio_cases
import mcp_census_cases
import mcp_color_composite_cases
import mcp_curve_cases
import mcp_drawing_cases
import mcp_edit_limit_cases
import mcp_failure_cases
import mcp_finishing_cases
import mcp_fx_authoring_cases
import mcp_fx_cases
import mcp_group_cases
import mcp_library_cases
import mcp_library_limit_cases
import mcp_limit_cases
import mcp_lipsync_cases
import mcp_macro_cases
import mcp_media_cases
import mcp_motion_path_cases
import mcp_movie_cases
import mcp_movie_eof
import mcp_palette_cases
import mcp_production_cases
import mcp_project_cases
import mcp_protocol_cases
import mcp_receipt_cases
import mcp_recipe_cases
import mcp_render_cases
import mcp_renumber_cases
import mcp_revision_cases
import mcp_rig_cases
import mcp_scene_access_cases
import mcp_scene_cases
import mcp_storyboard_cases
import mcp_studio_palette_cases
import mcp_text_cases
import mcp_vector_opacity_cases
import mcp_vector_transform_cases
import mcp_worker_cases
from mcp_png import decode_png, fixture_pixel, write_fixture
from mcp_worker import Worker
from run_harness_tests import Tee, annotation

REQUIRED_TOOLS = {
    "automation.capabilities",
    "automation.list_documents",
    "scene.create",
    "scene.summary",
    "scene.settings",
    "scene.save_copy",
    "scene.open",
    "scene.discard",
    "level.import",
    "column.create",
    "column.expose",
    "animation.set_keys",
    "animation.sample",
    "camera.inspect",
    "camera.update",
    "render.preview",
    "render.submit",
    "render.status",
    "render.cancel",
    "render.artifacts",
    "artifact.read",
}
SDK_CLIENT = "native-client.mjs"
SCHEMA_REJECTION = "INVALID_ARGUMENTS"
CaseMethod = Callable[["NativeAcceptance"], None]


def worker_only(reason: str) -> Callable[[CaseMethod], CaseMethod]:
    """Mark a case that qualifies the native worker process itself rather than its tools.

    No MCP client can express such a case, so through the SDK client it is skipped and the
    skip records the reason.
    """

    def mark(test: CaseMethod) -> CaseMethod:
        @functools.wraps(test)
        def run(case: NativeAcceptance) -> None:
            if case.through_sdk:
                case.skipTest(f"worker-only: {reason}")
            test(case)

        return run

    return mark


class NativeAcceptance(unittest.TestCase):
    """Qualify each automation milestone against the native executable."""

    executable: Path
    output: Path
    media_tools: Path | None = None

    def test_animation_channel_descriptors_and_fractional_sampling(self) -> None:
        mcp_animation_cases.animation_channel_descriptors_and_fractional_sampling(self)

    def test_rich_animation_round_trips_and_renders(self) -> None:
        mcp_animation_cases.rich_animation_round_trips_and_renders(self)

    def test_rich_animation_rejections_preserve_scene(self) -> None:
        mcp_animation_cases.rich_animation_rejections_preserve_scene(self)

    def test_rich_animation_retries_and_noops(self) -> None:
        mcp_animation_cases.rich_animation_retries_and_noops(self)

    def test_rich_animation_delete_keys_obeys_control_kinds(self) -> None:
        mcp_animation_cases.rich_animation_delete_keys_obeys_control_kinds(self)

    def test_vector_groups_preserve_paint_and_roundtrip(self) -> None:
        mcp_group_cases.vector_groups_preserve_paint_and_roundtrip(self)

    def test_vector_groups_reject_partial_boundaries(self) -> None:
        mcp_group_cases.vector_groups_reject_partial_boundaries(self)

    def test_vector_order_preserves_groups_and_stacking(self) -> None:
        mcp_group_cases.vector_order_preserves_groups_and_stacking(self)

    def test_vector_group_noops_and_rejections(self) -> None:
        mcp_group_cases.vector_group_noops_and_rejections(self)

    def test_fx_duplicate_and_enable_preserve_native_roundtrip(self) -> None:
        mcp_fx_authoring_cases.fx_duplicate_and_enable_preserve_native_roundtrip(self)

    def test_fx_presets_preserve_keys_and_target_graph(self) -> None:
        mcp_fx_authoring_cases.fx_presets_preserve_keys_and_target_graph(self)

    def test_fx_presets_cover_each_admitted_native_type(self) -> None:
        mcp_fx_authoring_cases.fx_presets_cover_each_admitted_native_type(self)

    def test_fx_authoring_rejections_preserve_document(self) -> None:
        mcp_fx_authoring_cases.fx_authoring_rejections_preserve_document(self)

    def test_fx_preset_integrity_and_file_receipts(self) -> None:
        mcp_fx_authoring_cases.fx_preset_integrity_and_file_receipts(self)

    def test_fx_preset_capacity_keeps_exact_replay(self) -> None:
        mcp_fx_authoring_cases.fx_preset_capacity_keeps_exact_replay(self)

    def test_color_composite_bounds_reject_before_mutation(self) -> None:
        mcp_color_composite_cases.color_composite_bounds_reject_before_mutation(self)

    def test_color_composite_blend_endpoints_select_sources(self) -> None:
        mcp_color_composite_cases.color_composite_blend_endpoints_select_sources(self)

    def test_color_presets_cover_native_types(self) -> None:
        mcp_color_composite_cases.color_presets_cover_native_types(self)

    def test_composite_presets_cover_native_types(self) -> None:
        mcp_color_composite_cases.composite_presets_cover_native_types(self)

    def test_color_composite_keys_survive_copy_and_reopen(self) -> None:
        mcp_color_composite_cases.color_composite_keys_survive_copy_and_reopen(self)

    def test_finishing_bounds_reject_before_mutation(self) -> None:
        mcp_finishing_cases.finishing_bounds_reject_before_mutation(self)

    def test_finishing_gradient_separation_rejects_before_mutation(self) -> None:
        mcp_finishing_cases.finishing_gradient_separation_rejects_before_mutation(self)

    def test_finishing_preflight_bounds_render_extent(self) -> None:
        mcp_finishing_cases.finishing_preflight_bounds_render_extent(self)

    def test_finishing_preflight_requires_unmirrored_erosion(self) -> None:
        mcp_finishing_cases.finishing_preflight_requires_unmirrored_erosion(self)

    def test_finishing_required_inputs_are_diagnosed(self) -> None:
        mcp_finishing_cases.finishing_required_inputs_are_diagnosed(self)

    @worker_only("needs the --test-render-deadline-ms test hook, which the adapter never passes")
    def test_finishing_render_deadline_stops_slow_job(self) -> None:
        mcp_finishing_cases.finishing_render_deadline_stops_slow_job(self)

    def test_light_presets_cover_native_types(self) -> None:
        mcp_finishing_cases.light_presets_cover_native_types(self)

    def test_stylize_presets_cover_native_types(self) -> None:
        mcp_finishing_cases.stylize_presets_cover_native_types(self)

    def test_grade_presets_cover_native_types(self) -> None:
        mcp_finishing_cases.grade_presets_cover_native_types(self)

    def test_recipe_light_wrap_composite_runs_literally(self) -> None:
        mcp_recipe_cases.recipe_light_wrap_composite_runs_literally(self)

    def test_recipe_backlit_rim_light_runs_literally(self) -> None:
        mcp_recipe_cases.recipe_backlit_rim_light_runs_literally(self)

    def test_recipe_exposure_grade_runs_literally(self) -> None:
        mcp_recipe_cases.recipe_exposure_grade_runs_literally(self)

    def test_recipe_cross_dissolve_runs_literally(self) -> None:
        mcp_recipe_cases.recipe_cross_dissolve_runs_literally(self)

    def test_recipe_iris_wipe_runs_literally(self) -> None:
        mcp_recipe_cases.recipe_iris_wipe_runs_literally(self)

    def test_macro_roundtrip_preserves_pixels_and_mapping(self) -> None:
        mcp_macro_cases.macro_roundtrip_preserves_pixels_and_mapping(self)

    def test_macro_retries_retire_handles_without_reapplying(self) -> None:
        mcp_macro_cases.macro_retries_retire_handles_without_reapplying(self)

    def test_macro_preflight_and_boundary_edits_preserve_scene(self) -> None:
        mcp_macro_cases.macro_preflight_and_boundary_edits_preserve_scene(self)

    def test_whole_curve_copy_preserves_independence_and_receipts(self) -> None:
        mcp_curve_cases.whole_curve_copy_preserves_independence_and_receipts(self)

    def test_whole_curve_retime_preserves_samples_rendering_and_reopen(self) -> None:
        mcp_curve_cases.whole_curve_retime_preserves_samples_rendering_and_reopen(self)

    def test_whole_curve_rejections_and_quantized_steps(self) -> None:
        mcp_curve_cases.whole_curve_rejections_and_quantized_steps(self)

    def test_motion_path_lifecycle_handles_and_receipts(self) -> None:
        mcp_motion_path_cases.motion_path_lifecycle_handles_and_receipts(self)

    def test_motion_path_animation_render_and_reopen(self) -> None:
        mcp_motion_path_cases.motion_path_animation_render_and_reopen(self)

    def test_motion_path_unused_geometry_and_name_roundtrip(self) -> None:
        mcp_motion_path_cases.motion_path_unused_geometry_and_name_roundtrip(self)

    def test_motion_path_geometry_percentage_and_revision_rejections(self) -> None:
        mcp_motion_path_cases.motion_path_geometry_percentage_and_revision_rejections(self)

    def test_audio_import_inspect_and_bounded_parser(self) -> None:
        mcp_audio_cases.audio_import_inspect_and_bounded_parser(self)

    def test_audio_clip_edits_reject_before_mutation(self) -> None:
        mcp_audio_cases.audio_clip_edits_reject_before_mutation(self)

    def test_audio_mixdown_matches_independent_model(self) -> None:
        mcp_audio_cases.audio_mixdown_matches_independent_model(self)

    def test_audio_aiff24_round_trips_every_sample(self) -> None:
        mcp_audio_cases.audio_aiff24_round_trips_every_sample(self)

    def test_audio_mixdown_formats_and_saturation(self) -> None:
        mcp_audio_cases.audio_mixdown_formats_and_saturation(self)

    def test_audio_save_reopen_render_and_scene_tools(self) -> None:
        mcp_audio_cases.audio_save_reopen_render_and_scene_tools(self)

    def test_audio_mixdown_rejections_and_receipts(self) -> None:
        mcp_audio_cases.audio_mixdown_rejections_and_receipts(self)

    def test_audio_movie_render_stays_video_only(self) -> None:
        mcp_audio_cases.audio_movie_render_stays_video_only(self)

    def test_rig_hook_attach_renders_and_reopens(self) -> None:
        mcp_rig_cases.rig_hook_attach_renders_and_reopens(self)

    def test_rig_pass_hook_matches_render(self) -> None:
        mcp_rig_cases.rig_pass_hook_matches_render(self)

    def test_rig_additive_scale_renders_and_reopens(self) -> None:
        mcp_rig_cases.rig_additive_scale_renders_and_reopens(self)

    def test_rig_handles_and_pivot_report_world_changes(self) -> None:
        mcp_rig_cases.rig_handles_and_pivot_report_world_changes(self)

    def test_rig_hook_lifecycle_and_drawing_edits(self) -> None:
        mcp_rig_cases.rig_hook_lifecycle_and_drawing_edits(self)

    def test_rig_layer_lifecycle_and_animation_keys(self) -> None:
        mcp_rig_cases.rig_layer_lifecycle_and_animation_keys(self)

    def test_rig_parent_policy_and_refusals(self) -> None:
        mcp_rig_cases.rig_parent_policy_and_refusals(self)

    def test_storyboard_panels_spans_and_thumbnails(self) -> None:
        mcp_storyboard_cases.storyboard_panels_spans_and_thumbnails(self)

    def test_storyboard_text_round_trips_and_receipts(self) -> None:
        mcp_storyboard_cases.storyboard_text_round_trips_and_receipts(self)

    def test_storyboard_rejections_preserve_panels(self) -> None:
        mcp_storyboard_cases.storyboard_rejections_preserve_panels(self)

    def test_lipsync_dat_import_inspect_and_apply(self) -> None:
        mcp_lipsync_cases.lipsync_dat_import_inspect_and_apply(self)

    def test_lipsync_rhubarb_rational_timing(self) -> None:
        mcp_lipsync_cases.lipsync_rhubarb_rational_timing(self)

    def test_lipsync_rejections_preserve_cells(self) -> None:
        mcp_lipsync_cases.lipsync_rejections_preserve_cells(self)

    def test_vector_opacity_round_trip(self) -> None:
        mcp_vector_opacity_cases.vector_opacity_round_trip(self)

    def test_vector_selection_and_rejection_purity(self) -> None:
        mcp_vector_opacity_cases.vector_selection_and_rejection_purity(self)

    def test_shared_root_flags_are_explicit_and_read_only(self) -> None:
        mcp_studio_palette_cases.shared_root_flags_are_explicit_and_read_only(self)

    def test_studio_palettes_list_inspect_find_preserve_roots(self) -> None:
        mcp_studio_palette_cases.studio_palettes_list_inspect_find_preserve_roots(self)

    def test_library_items_report_identity_dependencies_and_thumbnails(self) -> None:
        mcp_library_cases.library_items_report_identity_dependencies_and_thumbnails(self)

    def test_shared_parsers_refuse_damaged_files(self) -> None:
        mcp_library_cases.shared_parsers_refuse_damaged_files(self)

    @worker_only("needs the --test-child-fault test hook, which the adapter never passes")
    def test_shared_parser_failures_leave_worker_serving(self) -> None:
        mcp_library_cases.shared_parser_failures_leave_worker_serving(self)

    def test_read_only_journey_renders_and_preserves_sources(self) -> None:
        mcp_scene_access_cases.read_only_journey_renders_and_preserves_sources(self)

    def test_read_only_render_admission_refuses_unsafe_content(self) -> None:
        mcp_scene_access_cases.read_only_render_admission_refuses_unsafe_content(self)

    def test_read_only_rejections_and_limits(self) -> None:
        mcp_scene_access_cases.read_only_rejections_and_limits(self)

    def test_read_only_documents_refuse_mutations(self) -> None:
        mcp_scene_access_cases.read_only_documents_refuse_mutations(self)

    @worker_only(
        "sends every catalog tool only its document handle; the adapter checks those"
        " arguments against the tool's schema before the worker can refuse"
    )
    def test_read_only_documents_refuse_every_other_tool(self) -> None:
        mcp_scene_access_cases.read_only_documents_refuse_every_other_tool(self)

    def test_read_only_damaged_scenes_are_refused(self) -> None:
        mcp_scene_access_cases.read_only_damaged_scenes_are_refused(self)

    def test_read_only_content_rows_cover_columns_levels_and_styles(self) -> None:
        mcp_scene_access_cases.read_only_content_rows_cover_columns_levels_and_styles(self)

    def test_read_only_structure_limits_hold_at_their_bounds(self) -> None:
        mcp_scene_access_cases.read_only_structure_limits_hold_at_their_bounds(self)

    def test_read_only_file_and_report_limits_hold_at_their_bounds(self) -> None:
        mcp_scene_access_cases.read_only_file_and_report_limits_hold_at_their_bounds(self)

    def test_read_only_receipts_sources_and_lost_hosts(self) -> None:
        mcp_scene_access_cases.read_only_receipts_sources_and_lost_hosts(self)

    @worker_only("needs the --test-child-fault test hook, which the adapter never passes")
    def test_read_only_hosts_contain_native_failures(self) -> None:
        mcp_scene_access_cases.read_only_hosts_contain_native_failures(self)

    def test_read_only_projects_bind_their_own_folders(self) -> None:
        mcp_scene_access_cases.read_only_projects_bind_their_own_folders(self)

    def test_read_only_level_prechecks_refuse_before_native_load(self) -> None:
        mcp_scene_access_cases.read_only_level_prechecks_refuse_before_native_load(self)

    def test_text_outline_round_trip_and_edit(self) -> None:
        mcp_text_cases.text_outline_round_trip_and_edit(self)

    def test_text_rejections_preserve_drawing(self) -> None:
        mcp_text_cases.text_rejections_preserve_drawing(self)

    def test_passive_inspection_preserves_state(self) -> None:
        mcp_assistance_cases.passive_inspection_preserves_state(self)

    def test_compound_artwork_edits_preserve_native_roundtrip(self) -> None:
        mcp_artwork_cases.artwork_transform_renumber_palette_roundtrip(self)

    def test_cast_paging_preserves_native_state(self) -> None:
        mcp_production_cases.cast_paging_preserves_native_state(self)

    def test_scene_usage_and_dependencies(self) -> None:
        mcp_production_cases.scene_usage_and_dependencies(self)

    def test_scene_cell_limit_suppresses_unused_claims(self) -> None:
        mcp_production_cases.scene_cell_limit_suppresses_unused_claims(self)

    def test_vector_renumber_preserves_exposures_and_native_roundtrip(self) -> None:
        mcp_renumber_cases.check_renumber_roundtrip(self, "vector")

    def test_rgba_renumber_preserves_exposures_and_native_roundtrip(self) -> None:
        mcp_renumber_cases.check_renumber_roundtrip(self, "rgba8")

    def test_toonz_renumber_preserves_exposures_and_native_roundtrip(self) -> None:
        mcp_renumber_cases.check_renumber_roundtrip(self, "toonzRaster")

    def test_drawing_renumber_rejections_preserve_scene(self) -> None:
        mcp_renumber_cases.check_renumber_rejections(self)

    def test_drawing_renumber_preserves_imported_source(self) -> None:
        mcp_renumber_cases.check_renumber_import_preserves_source(self)

    def test_vector_palette_lifecycle_preserves_native_roundtrip(self) -> None:
        mcp_palette_cases.check_vector_palette_lifecycle(self)

    def test_toonz_palette_lifecycle_preserves_native_roundtrip(self) -> None:
        mcp_palette_cases.check_toonz_palette_lifecycle(self)

    def test_palette_page_rejections_preserve_palette(self) -> None:
        mcp_palette_cases.check_palette_page_rejections(self)

    def test_palette_page_capacity_preserves_native_roundtrip(self) -> None:
        mcp_palette_cases.check_palette_page_capacity(self)

    def test_vector_affine_preserves_paint_and_native_roundtrip(self) -> None:
        mcp_vector_transform_cases.vector_affine_preserves_paint_and_roundtrip(self)

    def test_vector_transform_rejections_and_noops_preserve_revision(self) -> None:
        mcp_vector_transform_cases.vector_transform_rejections_and_noops(self)

    def test_vector_transform_scales_radii_explicitly(self) -> None:
        mcp_vector_transform_cases.vector_transform_scales_radii_explicitly(self)

    def test_vector_transform_respects_fractional_pli_precision(self) -> None:
        mcp_vector_transform_cases.vector_transform_fractional_pli_precision(self)

    def test_media_directories_require_explicit_canonical_arguments(self) -> None:
        mcp_media_cases.media_directories_require_explicit_canonical_arguments(self)

    def test_media_tools_require_both_regular_unlinked_helpers(self) -> None:
        mcp_media_cases.media_tools_require_both_regular_unlinked_helpers(self)

    def test_media_tools_require_posix_execute_permission(self) -> None:
        mcp_media_cases.media_tools_require_posix_execute_permission(self)

    def test_media_tools_on_path_do_not_enable_movies(self) -> None:
        mcp_media_cases.media_tools_on_path_do_not_enable_movies(self)

    def test_movie_snapshot_metadata_and_pixels(self) -> None:
        mcp_movie_cases.movie_snapshot_metadata_and_pixels(self)

    def test_movie_without_media_tools_is_rejected(self) -> None:
        mcp_movie_cases.movie_without_media_tools_is_rejected(self)

    def test_movie_options_reject_unsupported_values(self) -> None:
        mcp_movie_cases.movie_options_reject_unsupported_values(self)

    def test_movie_cancellation_publishes_no_artifact(self) -> None:
        mcp_movie_cases.movie_cancellation_publishes_no_artifact(self)

    def test_movie_writer_failure_publishes_no_artifact(self) -> None:
        mcp_movie_cases.movie_writer_failure_publishes_no_artifact(self)

    def test_movie_eof_interrupts_encoder_finalization(self) -> None:
        mcp_movie_eof.movie_eof_interrupts_encoder_finalization(self)

    def test_receipt_capacity_preserves_bounded_cancellation(self) -> None:
        mcp_receipt_cases.receipt_capacity_preserves_bounded_cancellation(self)

    def test_parenting_preserves_local_keys_and_changes_rendered_placement(self) -> None:
        mcp_scene_cases.parenting_preserves_local_keys_and_changes_rendered_placement(self)

    def test_parenting_rejections_preserve_every_object(self) -> None:
        mcp_scene_cases.parenting_rejections_preserve_every_object(self)

    def test_locked_columns_and_deleted_handles_remain_consistent(self) -> None:
        mcp_scene_cases.locked_columns_and_deleted_handles_remain_consistent(self)

    def test_animation_precision_and_batch_errors_preserve_existing_keys(self) -> None:
        mcp_scene_cases.animation_precision_and_batch_errors_preserve_existing_keys(self)

    def test_custom_dpi_survives_reopen_and_remains_independent_of_camera(self) -> None:
        mcp_scene_cases.custom_dpi_survives_reopen_and_remains_independent_of_camera(self)

    def test_png_import_dimension_bounds_preserve_scene(self) -> None:
        mcp_scene_cases.png_import_dimension_bounds_preserve_scene(self)

    def test_level_capacity_rejection_preserves_scene(self) -> None:
        mcp_scene_cases.level_capacity_rejection_preserves_scene(self)

    def test_unicode_names_preserve_data_and_reject_unpaired_surrogates(self) -> None:
        mcp_worker_cases.unicode_names_preserve_data_and_reject_unpaired_surrogates(self)

    @worker_only(
        "launches the binary by relative path from other directories; the adapter never does"
    )
    def test_private_bootstrap_ignores_launch_configuration(self) -> None:
        mcp_worker_cases.private_bootstrap_ignores_launch_configuration(self)

    @worker_only("closes the worker's protocol output to check its own SIGPIPE handling")
    def test_closed_protocol_output_has_controlled_exit(self) -> None:
        mcp_worker_cases.closed_output_has_controlled_exit(self)

    @worker_only("gives the worker a directory as protocol input to check its read-error exit")
    def test_unreadable_protocol_input_is_not_clean_eof(self) -> None:
        mcp_worker_cases.unreadable_input_is_not_clean_eof(self)

    def test_unicode_workspace_and_asset_paths(self) -> None:
        mcp_worker_cases.unicode_roots_admit_png(self)

    def test_vector_drawing_data_survives_native_roundtrip(self) -> None:
        mcp_drawing_cases.check_vector_roundtrip(self)

    def test_rgba_drawing_data_survives_native_roundtrip(self) -> None:
        mcp_drawing_cases.check_rgba_roundtrip(self)

    def test_toonz_raster_data_survives_native_roundtrip(self) -> None:
        mcp_drawing_cases.check_toonz_raster_roundtrip(self)

    def test_lzo_helpers_preserve_tiny_and_incompressible_data(self) -> None:
        mcp_drawing_cases.check_lzo_roundtrip(self)

    def test_drawings_respect_native_format_number_limits(self) -> None:
        mcp_drawing_cases.check_drawing_number_limits(self)

    def test_palettes_respect_native_serialization_limits(self) -> None:
        mcp_drawing_cases.check_palette_serialization_limits(self)

    def test_fx_selected_output_survives_reopen(self) -> None:
        mcp_fx_cases.selected_output_survives_reopen(self)

    def test_fx_color_keys_survive_render_and_reopen(self) -> None:
        mcp_fx_cases.color_keys_survive_render_and_reopen(self)

    def test_blank_render_dimensions_and_alpha(self) -> None:
        mcp_render_cases.blank_dimensions_and_alpha(self)

    def test_preview_fit_preserves_camera(self) -> None:
        mcp_render_cases.preview_fit_preserves_camera(self)

    def test_generator_render_preserves_straight_alpha(self) -> None:
        mcp_render_cases.generator_straight_alpha(self)

    def test_render_snapshot_survives_edits_and_discard(self) -> None:
        mcp_render_cases.snapshot_survives_edits_and_discard(self)

    def test_changed_render_artifact_is_rejected(self) -> None:
        mcp_render_cases.changed_artifact_is_rejected(self)

    def test_eof_cancels_render_without_forced_kill(self) -> None:
        mcp_render_cases.eof_cancels_without_forced_kill(self)

    def test_client_results_match_native_serializer(self) -> None:
        mcp_protocol_cases.client_results_match_native_serializer(self)

    def test_mcp_cancellation_leaves_render_running(self) -> None:
        mcp_protocol_cases.mcp_cancellation_leaves_render_running(self)

    def test_document_ids_do_not_alias_after_close_and_reopen(self) -> None:
        mcp_protocol_cases.document_ids_do_not_alias_after_close_and_reopen(self)

    def test_integers_beyond_exact_precision_are_refused(self) -> None:
        mcp_limit_cases.integers_beyond_exact_precision_are_refused(self)

    @worker_only("writes NaN and overflowing number literals, which no SDK client can encode")
    def test_non_finite_numbers_are_refused_before_dispatch(self) -> None:
        mcp_limit_cases.non_finite_numbers_are_refused_before_dispatch(self)

    def test_wrong_value_kinds_are_refused_by_schema(self) -> None:
        mcp_limit_cases.wrong_value_kinds_are_refused_by_schema(self)

    def test_nested_values_are_refused_by_schema(self) -> None:
        mcp_limit_cases.nested_values_are_refused_by_schema(self)

    @worker_only("writes request lines nested beyond the worker's own JSON parser limit")
    def test_deep_nesting_is_refused_by_the_parser(self) -> None:
        mcp_limit_cases.deep_nesting_is_refused_by_the_parser(self)

    @worker_only("writes request lines that repeat a key, which no SDK client can encode")
    def test_duplicate_keys_are_refused_before_dispatch(self) -> None:
        mcp_limit_cases.duplicate_keys_are_refused_before_dispatch(self)

    def test_published_patterns_are_enforced_with_whole_string_semantics(self) -> None:
        mcp_limit_cases.published_patterns_are_enforced_with_whole_string_semantics(self)

    def test_non_lowercase_drawing_suffixes_are_refused(self) -> None:
        mcp_limit_cases.non_lowercase_drawing_suffixes_are_refused(self)

    def test_enum_parameters_accept_only_native_integers(self) -> None:
        mcp_limit_cases.enum_parameters_accept_only_native_integers(self)

    def test_hidden_fx_parameters_are_neither_listed_nor_writable(self) -> None:
        mcp_limit_cases.hidden_fx_parameters_are_neither_listed_nor_writable(self)

    def test_unknown_style_rejects_the_whole_vector_batch(self) -> None:
        mcp_edit_limit_cases.unknown_style_rejects_the_whole_vector_batch(self)

    def test_oversized_vector_selection_is_refused(self) -> None:
        mcp_edit_limit_cases.oversized_vector_selection_is_refused(self)

    def test_render_frame_ranges_are_refused_before_a_job_exists(self) -> None:
        mcp_edit_limit_cases.render_frame_ranges_are_refused_before_a_job_exists(self)

    def test_exact_render_retry_returns_the_same_job(self) -> None:
        mcp_edit_limit_cases.exact_render_retry_returns_the_same_job(self)

    def test_library_listing_pages_are_capped(self) -> None:
        mcp_library_limit_cases.library_listing_pages_are_capped(self)

    def test_library_thumbnails_are_bounded(self) -> None:
        mcp_library_limit_cases.library_thumbnails_are_bounded(self)

    def test_library_pose_thumbnails_are_bounded(self) -> None:
        mcp_library_limit_cases.library_pose_thumbnails_are_bounded(self)

    def test_library_listing_revision_tracks_real_changes(self) -> None:
        mcp_library_limit_cases.library_listing_revision_tracks_real_changes(self)

    def test_library_case_fold_collisions_are_reported(self) -> None:
        mcp_library_limit_cases.library_case_fold_collisions_are_reported(self)

    def test_library_unreadable_dependency_is_refused_not_hidden(self) -> None:
        mcp_library_limit_cases.library_unreadable_dependency_is_refused_not_hidden(self)

    def test_render_cancel_leaves_no_output_and_no_process(self) -> None:
        mcp_failure_cases.render_cancel_leaves_no_output_and_no_process(self)

    @worker_only("needs --test-render-fault hold-launch, which the adapter never passes")
    def test_render_cancel_before_launch_leaves_nothing(self) -> None:
        mcp_failure_cases.render_cancel_before_launch_leaves_nothing(self)

    @worker_only("needs --test-render-fault hold-dispatch, which the adapter never passes")
    def test_render_cancel_racing_dispatch_leaves_nothing(self) -> None:
        mcp_failure_cases.render_cancel_racing_dispatch_leaves_nothing(self)

    @worker_only("needs --test-render-fault hold-publication, which the adapter never passes")
    def test_render_cancel_racing_publication_leaves_nothing(self) -> None:
        mcp_failure_cases.render_cancel_racing_publication_leaves_nothing(self)

    @worker_only("needs --test-render-fault fail-after-final-frame, which the adapter never passes")
    def test_render_failure_after_final_frame_publishes_nothing(self) -> None:
        mcp_failure_cases.render_failure_after_final_frame_publishes_nothing(self)

    @worker_only("needs --test-render-fault encoder-failure, which the adapter never passes")
    def test_render_encoder_failure_publishes_nothing(self) -> None:
        mcp_failure_cases.render_encoder_failure_publishes_nothing(self)

    @worker_only("needs --test-render-fault stray-callbacks, which the adapter never passes")
    def test_render_stray_callbacks_do_not_change_the_outcome(self) -> None:
        mcp_failure_cases.render_stray_callbacks_do_not_change_the_outcome(self)

    @worker_only("kills the worker process and needs --test-render-fault hold-publication")
    def test_render_worker_killed_mid_preview_then_new_worker(self) -> None:
        mcp_failure_cases.render_worker_killed_mid_preview_then_new_worker(self)

    def test_render_storage_budget_refuses_before_creating_anything(self) -> None:
        mcp_failure_cases.render_storage_budget_refuses_before_creating_anything(self)

    def test_render_unwritable_output_directory_is_refused_cleanly(self) -> None:
        mcp_failure_cases.render_unwritable_output_directory_is_refused_cleanly(self)

    def test_render_disk_full_fails_cleanly_and_recovers(self) -> None:
        mcp_failure_cases.render_disk_full_fails_cleanly_and_recovers(self)

    def test_package_write_refusals_leave_nothing_behind(self) -> None:
        mcp_failure_cases.package_write_refusals_leave_nothing_behind(self)

    @worker_only("needs --test-write-fault publish, which the adapter never passes")
    def test_package_publish_failure_reports_what_is_on_disk(self) -> None:
        mcp_failure_cases.package_publish_failure_reports_what_is_on_disk(self)

    @worker_only("needs --test-write-fault publish, which the adapter never passes")
    def test_mixdown_publish_failure_reports_what_is_on_disk(self) -> None:
        mcp_failure_cases.mixdown_publish_failure_reports_what_is_on_disk(self)

    def test_read_only_render_cancel_and_host_loss_leave_nothing(self) -> None:
        mcp_failure_cases.read_only_render_cancel_and_host_loss_leave_nothing(self)

    def test_live_worker_and_host_files_survive_another_workers_start(self) -> None:
        mcp_failure_cases.live_worker_and_host_files_survive_another_workers_start(self)

    def test_dead_workers_directories_are_swept_and_live_ones_are_not(self) -> None:
        mcp_failure_cases.dead_workers_directories_are_swept_and_live_ones_are_not(self)

    def test_private_files_end_with_their_document_and_worker(self) -> None:
        mcp_failure_cases.private_files_end_with_their_document_and_worker(self)

    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix=self._testMethodName + "-", dir=self.output))
        self.workspace = self.directory / "workspace"
        self.inputs = self.directory / "inputs"
        self.workspace.mkdir()
        self.inputs.mkdir()
        self.fixture = self.inputs / "quadrants.png"
        write_fixture(self.fixture)
        self.worker = Worker(
            self.executable, self.workspace, self.inputs, self.directory / "worker.stderr.log"
        )
        self.addCleanup(self.worker.close)
        self.revisions: dict[str, str] = {}
        self.operations = 0

    def success(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        """Require a successful, object-shaped domain response."""
        response = self.worker.call(method, params)
        self.assertNotIn("error", response, f"{method}: {response}")
        result = response["result"]
        self.assertIsInstance(result, dict)
        return result

    def failure(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        """Require a structured actionable failure, never process termination."""
        response = self.worker.call(method, params)
        self.assertNotIn("result", response, f"{method} unexpectedly succeeded: {response}")
        error = response["error"]
        self.assertIsInstance(error.get("code"), str)
        self.assertTrue(error.get("message"), response)
        self.assertIsInstance(error.get("details"), dict)
        return error

    @property
    def through_sdk(self) -> bool:
        """Whether each worker is the MCP SDK client fixture driving the adapter."""
        return self.executable.name == SDK_CLIENT

    @property
    def native_executable(self) -> Path:
        """The OpenToonz executable that serves every call, directly or behind the adapter."""
        if not self.through_sdk:
            return self.executable
        return Path(os.environ["GROUNDZERO_NATIVE_EXECUTABLE"])

    def schema_code(self, native_code: str = "invalid_params") -> str:
        """Return the code for arguments outside a tool's advertised input schema.

        The adapter validates every call against the advertised schema and never forwards a
        violation, so through the SDK client the code is SCHEMA_REJECTION; the raw worker
        reports the violation with its own native_code.
        """
        return SCHEMA_REJECTION if self.through_sdk else native_code

    def schema_failure(
        self,
        method: str,
        params: dict[str, Any],
        native_code: str = "invalid_params",
        worker: Worker | None = None,
    ) -> dict[str, Any]:
        """Require the schema rejection of the client in use and an unchanged document.

        Args:
            method: The tool to call.
            params: Arguments that violate the tool's advertised input schema.
            native_code: The code the raw worker reports for this violation.
            worker: The worker to call instead of the case's own.
        Returns:
            The structured error.
        """
        worker = worker or self.worker
        document = params.get("document_id")
        tracked = isinstance(document, str) and document in self.revisions
        before = self._summary(worker, document) if tracked else None
        response = worker.call(method, params)
        self.assertNotIn("result", response, f"{method} unexpectedly succeeded: {response}")
        error = response["error"]
        self.assertEqual(error["code"], self.schema_code(native_code), error)
        self.assertTrue(error["message"], error)
        self.assertIsInstance(error["details"], dict)
        if self.through_sdk:
            self.assertTrue(error["details"]["issues"], error)
        if before is not None:
            self.assertEqual(self._summary(worker, document), before)
        return error

    def _summary(self, worker: Worker, document: Any) -> dict[str, Any]:
        response = worker.call("scene.summary", {"document_id": document})
        self.assertNotIn("error", response, response)
        return response["result"]

    def launch_refused(self, grants: list[str], name: str) -> str:
        """Require a launch with invalid grants to stop before the protocol starts.

        The raw worker reports `automation-worker: invalid_argument` and exits 2. Through the
        SDK client the adapter refuses the same grants itself and the client exits 1.

        Args:
            grants: Arguments after the workspace grant.
            name: Distinct name for the retained diagnostics.
        Returns:
            The diagnostics, for checks on how the refusal names its argument.
        """
        workspace = ["--workspace", str(self.workspace)]
        result = subprocess.run(
            [str(self.executable), "--automation-worker", *workspace, *grants],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=60,
            check=False,
        )
        (self.directory / f"{name}.stderr.log").write_bytes(result.stderr)
        diagnostics = result.stderr.decode("utf-8", errors="replace")
        code, marker = (
            (1, "Groundzero MCP startup failed:")
            if self.through_sdk
            else (2, "automation-worker: invalid_argument:")
        )
        self.assertEqual(result.returncode, code, (grants, diagnostics))
        self.assertEqual(result.stdout, b"", "A refused launch must not start the protocol")
        self.assertIn(marker, diagnostics)
        return diagnostics

    def write_params(self, document: str | None, params: dict[str, Any]) -> dict[str, Any]:
        """Attach the current revision and a unique mutation receipt identifier."""
        self.operations += 1
        result = {**params, "operation_id": f"operation-{self.operations}"}
        if document is not None:
            result.update(document_id=document, expected_revision=self.revisions[document])
        return result

    def mutate(self, method: str, document: str | None, **params: Any) -> dict[str, Any]:
        """Issue a mutation and retain the revision returned by the real host."""
        result = self.success(method, self.write_params(document, params))
        if "document_id" in result:
            self.assertIsInstance(result["revision"], str)
            self.revisions[result["document_id"]] = result["revision"]
        return result

    def create(self, name: str = "Acceptance") -> str:
        """Create a small scene with one source pixel per rendered camera pixel."""
        result = self.mutate(
            "scene.create",
            None,
            name=name,
            width_px=32,
            height_px=32,
            width_in=32 / 120,
            height_in=32 / 120,
            frame_rate=24,
        )
        return result["document_id"]

    def populated_scene(self) -> tuple[str, dict[str, Any], dict[str, Any]]:
        """Import and expose an actual PNG in a real Xsheet column."""
        document = self.create()
        level = self.mutate(
            "level.import", document, path=str(self.fixture), name="Quadrants", dpi=120
        )
        column = self.mutate("column.create", document, name="Artwork")
        self.mutate(
            "column.expose",
            document,
            column_id=column["column_id"],
            level_id=level["level_id"],
            drawing_id={"number": 1, "suffix": ""},
            start_frame=0,
            length=25,
        )
        return document, level, column

    def wait_job(self, job: str) -> dict[str, Any]:
        """Wait for an explicit terminal job state within a fixed CI deadline."""
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            result = self.success("render.status", {"job_id": job})
            if result["status"] in {"completed", "failed", "canceled", "deadline_exceeded"}:
                return result
            self.assertIn(
                result["status"],
                {
                    "queued",
                    "preparing",
                    "running",
                    "cancellation_requested",
                    "finalizing",
                },
            )
            time.sleep(0.05)
        self.fail(f"Render job {job} did not reach a terminal state within 90 seconds")

    def preview(self, document: str) -> bytes:
        """Render and retrieve a verified artifact through its opaque identifier."""
        revision = self.revisions[document]
        job = self.mutate("render.preview", document, frame=0, width=32, height=32)
        self.assertEqual(job["source_revision"], revision)
        self.assertEqual(self.revisions[document], revision)
        status = self.wait_job(job["job_id"])
        self.assertEqual(status["status"], "completed", status)
        artifacts = self.success("render.artifacts", {"job_id": job["job_id"]})["artifacts"]
        self.assertEqual(len(artifacts), 1, artifacts)
        artifact = artifacts[0]
        result = self.success("artifact.read", {"artifact_id": artifact["artifact_id"]})
        self.assertEqual(result["image"]["mimeType"], "image/png")
        data = base64.b64decode(result["image"]["data"], validate=True)
        self.assertEqual(hashlib.sha256(data).hexdigest(), artifact["sha256"])
        self.assertEqual(len(data), artifact["size_bytes"])
        self.assertEqual(result["source_revision"], revision)
        return data

    def test_revision_matrix_animation(self) -> None:
        mcp_revision_cases.check_group(self, "animation")

    def test_revision_matrix_drawing(self) -> None:
        mcp_revision_cases.check_group(self, "drawing")

    def test_revision_matrix_fx(self) -> None:
        mcp_revision_cases.check_group(self, "fx")

    def test_revision_matrix_lifecycle(self) -> None:
        mcp_revision_cases.check_group(self, "lifecycle")

    def test_revision_matrix_production(self) -> None:
        mcp_revision_cases.check_group(self, "production")

    def test_revision_matrix_render(self) -> None:
        mcp_revision_cases.check_group(self, "render")

    def test_revision_matrix_rigging(self) -> None:
        mcp_revision_cases.check_group(self, "rigging")

    def test_revision_matrix_scene(self) -> None:
        mcp_revision_cases.check_group(self, "scene")

    def test_revision_matrix_covers_the_catalog(self) -> None:
        mcp_revision_cases.check_catalog(self)

    def test_project_inspect_reports_the_private_workspace(self) -> None:
        mcp_project_cases.project_inspect_reports_the_private_workspace(self)

    def test_project_inspect_is_bounded_and_read_only(self) -> None:
        mcp_project_cases.project_inspect_is_bounded_and_read_only(self)

    def test_project_inspect_refuses_arguments(self) -> None:
        mcp_project_cases.project_inspect_refuses_arguments(self)

    def test_census_matches_live_catalog(self) -> None:
        mcp_census_cases.census_matches_live_catalog(self)

    def test_capabilities_are_strict_and_implemented(self) -> None:
        capabilities = self.success("automation.capabilities", {})
        self.assertEqual(capabilities["protocol_version"], 2)
        tools = capabilities["tools"]
        names = [tool["name"] for tool in tools]
        self.assertEqual(len(names), len(set(names)), "Duplicate advertised tool names")
        self.assertTrue(REQUIRED_TOOLS.issubset(names), REQUIRED_TOOLS.difference(names))
        listed = [name for toolset in capabilities["toolsets"] for name in toolset["tools"]]
        self.assertEqual(sorted(listed), sorted(names), "Toolsets must partition the catalog")
        described = {
            toolset["name"] for toolset in capabilities["toolsets"] if toolset["description"]
        }
        for tool in tools:
            with self.subTest(tool=tool["name"]):
                self.assertTrue(tool["description"])
                self.assertIn(tool["toolset"], described)
                schema = tool["inputSchema"]
                self.assertEqual(schema["type"], "object")
                self.assertIs(schema["additionalProperties"], False)
                readonly = tool["annotations"]["readOnlyHint"]
                self.assertIsInstance(readonly, bool)
                if not readonly:
                    self.assertIn("operation_id", schema["required"])
                    if "document_id" in schema["properties"]:
                        self.assertIn("expected_revision", schema["required"])
                self.failure(tool["name"], {"unrecognized_acceptance_argument": True})

    @worker_only("runs the native domain tests, which only the worker's --test-domains flag starts")
    def test_native_domain_regressions(self) -> None:
        domain_workspace = self.directory / "domain-workspace"
        domain_workspace.mkdir()
        with (self.directory / "native-domain-tests.log").open("wb") as log:
            result = subprocess.run(
                [
                    str(self.executable),
                    "--automation-worker",
                    "--workspace",
                    str(domain_workspace),
                    "--input-root",
                    str(self.inputs),
                    "--test-domains",
                ],
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=300,
                check=False,
            )
        diagnostics = (self.directory / "native-domain-tests.log").read_bytes()[-4096:]
        self.assertEqual(result.returncode, 0, diagnostics.decode("utf-8", errors="replace"))
        reports = json.loads((domain_workspace / "media-verifier-report.json").read_text())
        self.assertTrue(reports, "Native movie verifier cases did not run")
        self.assertIn("production-deadline", {report["case"] for report in reports})
        for report in reports:
            with self.subTest(native_case=report["case"]):
                self.assertEqual(report["status"], "passed", report)

    def test_vector_edits_validate_the_whole_batch(self) -> None:
        document = self.create()
        level = self.mutate("level.create", document, name="Vector", format="vector")
        target = {"level_id": level["level_id"], "drawing_id": {"number": 1, "suffix": "a"}}
        self.mutate("drawing.create", document, **target, format="vector")
        points = [{"x": x, "y": 0, "thickness": 1} for x in (-10, 0, 10)]
        edit = {"action": "add", "style_id": 1, "closed": False, "points": points}
        edits = {**target, "coordinate_space": "drawing_native", "sharing": "all_exposures"}
        self.mutate("vector.edit", document, **edits, edits=[edit])
        inspected = self.success("vector.inspect", {"document_id": document, **target})
        self.assertEqual(inspected["total"], 1)
        self.assertEqual(inspected["strokes"][0]["points"], points)
        revision = self.revisions[document]
        self.failure(
            "vector.edit",
            self.write_params(
                document,
                {
                    **edits,
                    "edits": [edit, {"action": "delete", "index": 99}],
                },
            ),
        )
        self.assertEqual(
            self.success("vector.inspect", {"document_id": document, **target}), inspected
        )
        self.assertEqual(
            self.success("scene.summary", {"document_id": document})["revision"], revision
        )
        points[1]["y"] = 8
        self.mutate(
            "vector.edit",
            document,
            **edits,
            edits=[{**edit, "action": "update", "index": 0, "points": points}],
        )
        updated = self.success("vector.inspect", {"document_id": document, **target})
        self.assertEqual(updated["strokes"][0]["points"], points)
        self.mutate("vector.edit", document, **edits, edits=[{"action": "delete", "index": 0}])
        self.assertEqual(
            self.success("vector.inspect", {"document_id": document, **target})["total"], 0
        )

    def test_palette_updates_preserve_other_animation_keys(self) -> None:
        document = self.create()
        level = self.mutate("level.create", document, name="Palette", format="vector")
        target = {"level_id": level["level_id"]}
        red = {"r": 255, "g": 0, "b": 0, "a": 255}
        blue = {"r": 0, "g": 0, "b": 255, "a": 255}
        green = {"r": 0, "g": 255, "b": 0, "a": 255}
        style = self.mutate("palette.create_style", document, **target, name="Animated", color=red)
        target["style_id"] = style["style_id"]
        self.mutate("palette.update_style", document, **target, color=red, keyframe=0)
        before = self.mutate("palette.update_style", document, **target, color=blue, keyframe=24)
        after = self.mutate("palette.update_style", document, **target, color=green, keyframe=0)
        self.assertEqual(after["keyframes"], [0, 24])
        self.assertEqual(after["animation"][0]["color"], green)
        self.assertEqual(after["animation"][1], before["animation"][1])
        self.failure("palette.update_style", self.write_params(document, {**target, "color": red}))
        palette = self.success(
            "palette.inspect",
            {
                "document_id": document,
                "level_id": level["level_id"],
            },
        )
        current = next(item for item in palette["styles"] if item["style_id"] == style["style_id"])
        self.assertEqual(current["animation"], after["animation"])

    def test_vector_fill_and_quadratic_geometry_validation(self) -> None:
        document = self.create()
        level = self.mutate("level.create", document, name="Closed", format="vector")
        target = {"level_id": level["level_id"], "drawing_id": {"number": 1, "suffix": ""}}
        self.mutate("drawing.create", document, **target, format="vector")
        edits = {**target, "coordinate_space": "drawing_native", "sharing": "all_exposures"}
        corners = ((0, 0), (5, 0), (10, 0), (10, 5), (10, 10), (5, 10), (0, 10), (0, 5), (0, 0))
        points = [{"x": x, "y": y, "thickness": 0.5} for x, y in corners]
        stroke = {"action": "add", "style_id": 1, "closed": True, "points": points}
        self.failure(
            "vector.edit",
            self.write_params(
                document,
                {
                    **edits,
                    "edits": [{**stroke, "points": points[:-1]}],
                },
            ),
        )
        self.failure(
            "vector.edit",
            self.write_params(
                document,
                {
                    **edits,
                    "sharing": "unacknowledged",
                    "edits": [stroke],
                },
            ),
        )
        self.mutate("vector.edit", document, **edits, edits=[stroke])
        filled = self.mutate("vector.fill", document, **edits, x=5, y=5, style_id=1)
        self.assertEqual(filled["style_id"], 1)
        self.assertGreaterEqual(filled["previous_style_id"], 0)
        self.failure(
            "vector.fill", self.write_params(document, {**edits, "x": 50, "y": 50, "style_id": 1})
        )

    def test_raster_png_roundtrip_and_source_precision_admission(self) -> None:
        document, level, _column = self.populated_scene()
        target = {"level_id": level["level_id"], "drawing_id": {"number": 1, "suffix": ""}}
        exported = self.mutate(
            "raster.export_png", document, **target, path=str(self.workspace / "exported.png")
        )
        self.assertEqual(
            decode_png(Path(exported["path"]).read_bytes()), decode_png(self.fixture.read_bytes())
        )
        copied = self.inputs / "roundtrip.png"
        shutil.copyfile(exported["path"], copied)
        destination = {"level_id": level["level_id"], "drawing_id": {"number": 2, "suffix": "a"}}
        self.mutate("raster.import_png", document, **destination, path=str(copied), dpi=120)
        rectangle = {
            "coordinate_space": "raster_pixels_lower_left",
            "x": 0,
            "y": 0,
            "width": 32,
            "height": 32,
        }
        self.assertEqual(
            self.success(
                "raster.read_pixels",
                {
                    "document_id": document,
                    **destination,
                    **rectangle,
                },
            ),
            self.success("raster.read_pixels", {"document_id": document, **target, **rectangle}),
        )
        precise = self.inputs / "sixteen-bit.png"
        write_fixture(precise, depth=16)
        self.failure(
            "raster.import_png",
            self.write_params(
                document,
                {
                    **destination,
                    "drawing_id": {"number": 3, "suffix": ""},
                    "path": str(precise),
                    "dpi": 120,
                },
            ),
        )
        self.failure(
            "level.import",
            self.write_params(
                document,
                {
                    "name": "Sixteen bit",
                    "path": str(precise),
                    "dpi": 120,
                },
            ),
        )

    def test_glow_without_light_preserves_source_pixels(self) -> None:
        document, _level, _column = self.populated_scene()
        baseline = decode_png(self.preview(document))
        graph = self.success("fx.graph", {"document_id": document})
        glow = self.mutate("fx.create", document, type="STD_glowFx")
        self.mutate(
            "fx.connect",
            document,
            fx_id=glow["fx_id"],
            port="Source",
            source_fx_id=graph["xsheet_fx_id"],
        )
        self.mutate(
            "fx.connect",
            document,
            fx_id=graph["current_output_fx_id"],
            port="source",
            source_fx_id=glow["fx_id"],
        )
        graph = self.success("fx.graph", {"document_id": document})
        node = next(effect for effect in graph["effects"] if effect["fx_id"] == glow["fx_id"])
        self.assertIn("GLOW_WITHOUT_LIGHT", [item["code"] for item in node["diagnostics"]])
        self.assertEqual(decode_png(self.preview(document)), baseline)

    def test_native_raster_pixels_and_atomic_patch_rejection(self) -> None:
        for image_format in ("rgba8", "toonzRaster"):
            with self.subTest(format=image_format):
                document = self.create(image_format)
                level = self.mutate(
                    "level.create", document, name=image_format, format=image_format
                )
                target = {"level_id": level["level_id"], "drawing_id": {"number": 1, "suffix": ""}}
                self.mutate(
                    "drawing.create",
                    document,
                    **target,
                    format=image_format,
                    width=2,
                    height=2,
                    dpi=120,
                )
                rectangle = {
                    **target,
                    "coordinate_space": "raster_pixels_lower_left",
                    "x": 0,
                    "y": 0,
                    "width": 2,
                    "height": 1,
                }
                if image_format == "rgba8":
                    pixels = [
                        {"r": 64, "g": 32, "b": 0, "a": 128},
                        {"r": 0, "g": 0, "b": 0, "a": 0},
                    ]
                    invalid = {"r": 255, "g": 0, "b": 0, "a": 1}
                else:
                    pixels = [
                        {"ink": 1, "paint": 0, "tone": 20},
                        {"ink": 0, "paint": 1, "tone": 255},
                    ]
                    invalid = {"ink": 4095, "paint": 0, "tone": 0}
                self.mutate(
                    "raster.patch", document, **rectangle, sharing="all_exposures", pixels=pixels
                )
                observed = self.success(
                    "raster.read_pixels", {"document_id": document, **rectangle}
                )
                self.assertEqual(observed["pixels"], pixels)
                self.assertEqual(observed["row_order"], "bottom_to_top")
                self.failure(
                    "raster.patch",
                    self.write_params(
                        document,
                        {
                            **rectangle,
                            "sharing": "all_exposures",
                            "pixels": [pixels[1], invalid],
                        },
                    ),
                )
                self.assertEqual(
                    self.success(
                        "raster.read_pixels",
                        {
                            "document_id": document,
                            **rectangle,
                        },
                    ),
                    observed,
                )

    def test_held_drawing_clone_is_independent_and_preserves_suffix(self) -> None:
        document, level, _column = self.populated_scene()
        target = {"level_id": level["level_id"], "drawing_id": {"number": 1, "suffix": ""}}
        self.failure("drawing.remove", self.write_params(document, target))
        cloned = self.mutate(
            "drawing.clone", document, **target, destination_drawing_id={"number": 1, "suffix": "b"}
        )
        self.assertEqual(cloned["drawing_id"], {"number": 1, "suffix": "b"})
        rectangle = {
            **target,
            "coordinate_space": "raster_pixels_lower_left",
            "x": 0,
            "y": 0,
            "width": 1,
            "height": 1,
        }
        original = self.success("raster.read_pixels", {"document_id": document, **rectangle})
        destination = {**rectangle, "drawing_id": cloned["drawing_id"]}
        self.mutate(
            "raster.patch",
            document,
            **destination,
            sharing="all_exposures",
            pixels=[{"r": 255, "g": 0, "b": 255, "a": 255}],
        )
        self.assertEqual(
            self.success("raster.read_pixels", {"document_id": document, **rectangle}), original
        )
        self.mutate(
            "drawing.remove", document, level_id=level["level_id"], drawing_id=cloned["drawing_id"]
        )
        self.failure(
            "drawing.inspect",
            {
                "document_id": document,
                "level_id": level["level_id"],
                "drawing_id": cloned["drawing_id"],
            },
        )

    @worker_only("writes malformed and oversize frames to the worker's own NDJSON protocol")
    def test_protocol_rejects_malformed_and_oversize_requests(self) -> None:
        self.assertEqual(self.worker.exchange(b"{malformed json")["error"]["code"], "parse_error")
        invalid_utf8 = b'{"id":"invalid-\xff","method":"automation.capabilities","params":{}}'
        self.assertIn("error", self.worker.exchange(invalid_utf8))
        concatenated = b'{"id":"first","method":"automation.capabilities","params":{}}{}'
        self.assertIn("error", self.worker.exchange(concatenated))
        missing_id = self.worker.exchange(b'{"method":"automation.capabilities","params":{}}')
        self.assertIn("error", missing_id)
        self.assertIsNone(missing_id["id"])
        self.assertEqual(self.success("automation.capabilities", {})["protocol_version"], 2)
        self.assertIn(
            "error",
            self.worker.exchange(
                json.dumps(
                    {
                        "id": "extra",
                        "method": "automation.capabilities",
                        "params": {},
                        "extra": 1,
                    }
                ).encode()
            ),
        )
        self.failure("acceptance.nonexistent", {})
        self.failure("scene.create", {"name": "Missing receipt"})
        self.failure(
            "scene.create",
            self.write_params(
                None,
                {
                    "name": "Unknown argument",
                    "unrecognized_acceptance_argument": True,
                },
            ),
        )
        self.failure("scene.create", self.write_params(None, {"name": "Bad width", "width_px": -1}))
        self.assertEqual(self.success("automation.list_documents", {})["documents"], [])
        oversized = b'{"id":"oversized","method":"automation.capabilities","params":{"pad":"'
        oversized += b"x" * (8 * 1024 * 1024) + b'"}}'
        self.assertEqual(self.worker.exchange(oversized)["error"]["code"], "request_too_large")
        self.assertEqual(self.worker.process.wait(timeout=30), 2)

    def test_revision_conflicts_noops_and_receipt_replay(self) -> None:
        document = self.create()
        original = self.revisions[document]
        params = self.write_params(document, {"width_px": 64})
        first = self.success("scene.settings", params)
        self.assertNotEqual(first["revision"], original)
        self.assertEqual(self.success("scene.settings", params), first)
        self.failure("scene.settings", {**params, "width_px": 128})
        self.revisions[document] = first["revision"]
        noop = self.mutate("scene.settings", document, width_px=64)
        self.assertEqual(noop["revision"], first["revision"])
        stale = self.write_params(document, {"width_px": 128})
        self.failure("scene.settings", {**stale, "expected_revision": original})
        self.assertEqual(
            self.success("scene.summary", {"document_id": document})["revision"], first["revision"]
        )

    def test_import_admission_rejects_missing_external_and_symlink_assets(self) -> None:
        document = self.create()
        external = self.directory / "unapproved.png"
        shutil.copyfile(self.fixture, external)
        link = self.inputs / "escaped.png"
        link.symlink_to(external)
        for path in (self.inputs / "missing.png", external, link):
            with self.subTest(path=path.name):
                self.failure(
                    "level.import",
                    self.write_params(
                        document,
                        {
                            "path": str(path),
                            "name": "Rejected",
                            "dpi": 120,
                        },
                    ),
                )
        summary = self.success("scene.summary", {"document_id": document})
        self.assertEqual(summary["revision"], self.revisions[document])
        self.assertEqual(summary["levels"], [])
        self.failure("scene.open", self.write_params(None, {"path": str(self.fixture)}))

    def test_native_decoder_failure_leaves_document_usable(self) -> None:
        document = self.create()
        level = self.mutate("level.create", document, name="Broken input", format="rgba8")
        truncated = self.inputs / "truncated.png"
        truncated.write_bytes(self.fixture.read_bytes()[:33])
        before = self.success("scene.summary", {"document_id": document})
        self.failure(
            "raster.import_png",
            self.write_params(
                document,
                {
                    "level_id": level["level_id"],
                    "drawing_id": {"number": 1, "suffix": ""},
                    "path": str(truncated),
                    "dpi": 120,
                },
            ),
        )
        self.assertEqual(self.success("scene.summary", {"document_id": document}), before)
        self.failure(
            "level.import",
            self.write_params(
                document,
                {
                    "name": "Invalid decoded asset",
                    "path": str(truncated),
                    "dpi": 120,
                },
            ),
        )
        self.assertEqual(self.success("scene.summary", {"document_id": document}), before)
        self.assertEqual(self.success("automation.capabilities", {})["protocol_version"], 2)

    def test_exposures_animation_and_document_isolation(self) -> None:
        document, level, column = self.populated_scene()
        other = self.create("Isolated")
        before = self.revisions[document]
        self.failure(
            "column.expose",
            self.write_params(
                other,
                {
                    "column_id": column["column_id"],
                    "level_id": level["level_id"],
                    "drawing_id": {"number": 1, "suffix": ""},
                    "start_frame": 0,
                    "length": 1,
                },
            ),
        )
        self.mutate(
            "animation.set_keys",
            document,
            object_id=column["object_id"],
            channel="x",
            unit="inch",
            keys=[
                {"frame": 0, "value": 0, "interpolation": "linear"},
                {"frame": 24, "value": 2, "interpolation": "linear"},
            ],
        )
        samples = self.success(
            "animation.sample",
            {
                "document_id": document,
                "object_id": column["object_id"],
                "channel": "x",
                "unit": "inch",
                "frames": [0, 12, 24],
            },
        )
        self.assertEqual([sample["value"] for sample in samples["values"]], [0, 1, 2])
        self.assertNotEqual(self.revisions[document], before)
        self.assertEqual(self.success("scene.summary", {"document_id": other})["levels"], [])

    def test_scene_edit_failures_leave_native_state_unchanged(self) -> None:
        document, level, column = self.populated_scene()
        self.mutate("column.update", document, column_id=column["column_id"], locked=True)
        before = self.success("scene.summary", {"document_id": document})
        self.failure(
            "column.expose",
            self.write_params(
                document,
                {
                    "column_id": column["column_id"],
                    "level_id": level["level_id"],
                    "drawing_id": {"number": 1, "suffix": ""},
                    "start_frame": 40,
                    "length": 1,
                },
            ),
        )
        self.assertEqual(self.success("scene.summary", {"document_id": document}), before)
        animation = {"object_id": column["object_id"], "channel": "x", "unit": "inch"}
        keys_before = self.success("animation.keys", {"document_id": document, **animation})
        self.failure(
            "animation.set_keys",
            self.write_params(
                document,
                {
                    **animation,
                    "keys": [
                        {"frame": 0, "value": 5, "interpolation": "linear"},
                        {"frame": 24, "value": 10, "interpolation": "invalid"},
                    ],
                },
            ),
        )
        self.assertEqual(
            self.success("animation.keys", {"document_id": document, **animation}), keys_before
        )
        first = self.mutate("object.create", document, name="First", kind="pegbar")
        second = self.mutate("object.create", document, name="Second", kind="pegbar")
        self.mutate(
            "object.parent",
            document,
            object_id=first["object_id"],
            parent_id=second["object_id"],
            placement_policy="preserve_local",
        )
        before = self.success("scene.summary", {"document_id": document})
        self.failure(
            "object.parent",
            self.write_params(
                document,
                {
                    "object_id": second["object_id"],
                    "parent_id": first["object_id"],
                    "placement_policy": "preserve_local",
                },
            ),
        )
        self.assertEqual(self.success("scene.summary", {"document_id": document}), before)

    def test_save_reopen_and_native_preview_pixels(self) -> None:
        document, _level, _column = self.populated_scene()
        preview = self.preview(document)
        width, height, pixels = decode_png(preview)
        self.assertEqual((width, height), (32, 32))
        for x, y in ((8, 8), (24, 8), (8, 24), (24, 24)):
            self.assertEqual(pixels[y * width + x], fixture_pixel(x, y, width), (x, y))
        saved = self.mutate("scene.save_copy", document, directory=str(self.workspace / "saved"))
        self.assertTrue(Path(saved["scene_path"]).is_file())
        self.assertTrue(Path(saved["manifest_path"]).is_file())
        self.assertTrue(saved["verified_drawings"])
        reopened = self.mutate("scene.open", None, path=saved["package_directory"])
        self.assertNotEqual(reopened["document_id"], document)
        self.assertEqual(decode_png(self.preview(reopened["document_id"])), (width, height, pixels))
        self.mutate("scene.discard", document)
        self.failure("scene.summary", {"document_id": document})
        self.assertEqual(decode_png(self.preview(reopened["document_id"])), (width, height, pixels))

    def test_missing_saved_assets_are_not_admitted(self) -> None:
        document, _level, _column = self.populated_scene()
        saved = self.mutate("scene.save_copy", document, directory=str(self.workspace / "missing"))
        package = Path(saved["package_directory"])
        assets = list(package.rglob("*.png"))
        self.assertTrue(assets, "Saved package must contain the imported raster drawing")
        assets[0].unlink()
        before = self.success("automation.list_documents", {})
        self.failure("scene.open", self.write_params(None, {"path": str(package)}))
        self.assertEqual(self.success("automation.list_documents", {}), before)

    def test_multiletter_drawing_suffix_survives_native_save_and_exposure(self) -> None:
        for image_format in ("vector", "rgba8", "toonzRaster"):
            with self.subTest(format=image_format):
                document = self.create(image_format)
                level = self.mutate("level.create", document, name="Suffix", format=image_format)
                drawing = {"number": 1, "suffix": "abcdefgh"}
                target = {"level_id": level["level_id"], "drawing_id": drawing}
                dimensions = (
                    {} if image_format == "vector" else {"width": 2, "height": 2, "dpi": 120}
                )
                self.mutate("drawing.create", document, **target, format=image_format, **dimensions)
                other_drawing = {"number": 1, "suffix": "z"}
                self.mutate(
                    "drawing.create",
                    document,
                    level_id=level["level_id"],
                    drawing_id=other_drawing,
                    format=image_format,
                    **dimensions,
                )
                self.failure(
                    "drawing.create",
                    self.write_params(
                        document,
                        {
                            **target,
                            "drawing_id": {"number": 2, "suffix": "abcdefghi"},
                            "format": image_format,
                            **dimensions,
                        },
                    ),
                )
                column = self.mutate("column.create", document, name="Suffix")
                self.mutate(
                    "column.expose",
                    document,
                    **target,
                    column_id=column["column_id"],
                    start_frame=0,
                    length=3,
                )
                self.mutate(
                    "column.expose",
                    document,
                    level_id=level["level_id"],
                    drawing_id=other_drawing,
                    column_id=column["column_id"],
                    start_frame=3,
                    length=3,
                )
                saved = self.mutate(
                    "scene.save_copy",
                    document,
                    directory=str(self.workspace / f"suffix-{image_format}"),
                )
                reopened = self.mutate("scene.open", None, path=saved["package_directory"])
                other = reopened["document_id"]
                summary = self.success("scene.summary", {"document_id": other})
                inspected = self.success(
                    "level.inspect",
                    {
                        "document_id": other,
                        "level_id": summary["levels"][0]["level_id"],
                    },
                )
                self.assertIn(drawing, inspected["drawings"])
                self.assertIn(other_drawing, inspected["drawings"])
                cells = self.success(
                    "column.cells",
                    {
                        "document_id": other,
                        "column_id": summary["columns"][0]["column_id"],
                        "start_frame": 0,
                        "length": 6,
                    },
                )
                self.assertEqual(
                    [cell["drawing_id"] for cell in cells["cells"]],
                    [drawing] * 3 + [other_drawing] * 3,
                )
                self.mutate(
                    "drawing.create",
                    other,
                    level_id=summary["levels"][0]["level_id"],
                    drawing_id={"number": 2, "suffix": "a"},
                    format=image_format,
                    **dimensions,
                )

    def test_unregistered_and_altered_packages_are_not_admitted(self) -> None:
        document, _level, _column = self.populated_scene()
        saved = self.mutate("scene.save_copy", document, directory=str(self.workspace / "original"))
        copy = self.workspace / "unregistered"
        shutil.copytree(saved["package_directory"], copy)
        before = self.success("automation.list_documents", {})
        self.failure("scene.open", self.write_params(None, {"path": str(copy)}))
        manifest = Path(saved["manifest_path"])
        manifest.write_bytes(manifest.read_bytes() + b"\n")
        self.failure("scene.open", self.write_params(None, {"path": saved["package_directory"]}))
        self.assertEqual(self.success("automation.list_documents", {}), before)

    def test_cancellation_and_worker_termination_are_bounded(self) -> None:
        document, _level, _column = self.populated_scene()
        job = self.mutate(
            "render.submit", document, start_frame=0, end_frame=119, step=1, width=256, height=256
        )
        canceled = self.success("render.cancel", self.write_params(None, {"job_id": job["job_id"]}))
        self.assertIn(canceled["status"], {"cancellation_requested", "canceled"})
        self.assertEqual(self.wait_job(job["job_id"])["status"], "canceled")
        self.failure("render.artifacts", {"job_id": job["job_id"]})
        self.mutate(
            "render.submit", document, start_frame=0, end_frame=119, step=1, width=256, height=256
        )
        self.worker.process.terminate()
        self.worker.process.wait(timeout=10)
        self.assertIsNotNone(self.worker.process.returncode)
        self.worker = Worker(
            self.executable, self.workspace, self.inputs, self.directory / "replacement.stderr.log"
        )
        self.addCleanup(self.worker.close)
        self.assertEqual(self.success("automation.list_documents", {})["documents"], [])
        self.failure("scene.summary", {"document_id": document})


def main() -> int:
    """Run the real-worker suite and retain both text output and native diagnostics."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--media-tools", type=Path)
    args = parser.parse_args()
    NativeAcceptance.executable = args.executable.resolve(strict=True)
    NativeAcceptance.output = args.output.resolve()
    NativeAcceptance.media_tools = (
        args.media_tools.resolve(strict=True) if args.media_tools else None
    )
    NativeAcceptance.output.mkdir(parents=True, exist_ok=True)
    report_path = NativeAcceptance.output / "acceptance.txt"
    with report_path.open("w", encoding="utf-8") as report:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(NativeAcceptance)
        result = unittest.TextTestRunner(stream=Tee(sys.stdout, report), verbosity=2).run(suite)
    for test, traceback in result.errors + result.failures:
        annotation(test.id(), traceback)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
