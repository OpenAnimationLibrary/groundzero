"""Scene output settings and guides: explicit settings that make two shots identical,
guides that change no pixel, blocks that change only when named, and a setting scope per
row, each saved and reopened through both loaders."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_sheet_record_cases import save_and_reopen

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

BACKGROUND = {"r": 10, "g": 20, "b": 30, "a": 255}


def properties(case: NativeAcceptance, document: str) -> dict[str, Any]:
    """Return scene.properties.inspect at the document's current revision."""
    return case.success(
        "scene.properties.inspect",
        {"document_id": document, "expected_revision": case.revisions[document]},
    )


def _reopened(case: NativeAcceptance, document: str, name: str) -> str:
    before = properties(case, document)
    _saved, reopened = save_and_reopen(case, document, name)
    case.assertEqual(properties(case, reopened), before)
    return reopened


def _shot(case: NativeAcceptance, name: str) -> dict[str, Any]:
    document = case.mutate(
        "scene.create",
        None,
        name=name,
        width_px=32,
        height_px=32,
        width_in=32 / 120,
        height_in=32 / 120,
        frame_rate=24,
        background_color=BACKGROUND,
    )["document_id"]
    case.mutate(
        "scene.output.set",
        document,
        apply_to=["output", "preview"],
        settings={
            "range": {"kind": "frames", "start_frame": 0, "end_frame": 119},
            "step": 1,
            "shrink": 1,
            "bpp": 32,
            "aspect_export": {"mode": "none"},
        },
        column_filters_on_render=False,
    )
    return {k: v for k, v in properties(case, document).items() if k != "frame_count"}


@acceptance_case
def explicit_settings_make_two_shots_identical(case: NativeAcceptance) -> None:
    """Two scenes given the same explicit settings report the same properties, which a
    reopen keeps (SCN-09 #1)."""
    first, second = _shot(case, "Shot A"), _shot(case, "Shot B")
    case.assertEqual(first, second)
    case.assertEqual(first["background_color"], BACKGROUND)
    case.assertEqual(
        first["output"]["range"], {"kind": "frames", "start_frame": 0, "end_frame": 119}
    )
    document = case.create("Background")
    case.mutate("scene.settings", document, background_color=BACKGROUND)
    case.assertEqual(properties(case, document)["background_color"], BACKGROUND)
    _reopened(case, document, "background")


@acceptance_case
def output_blocks_change_only_when_named(case: NativeAcceptance) -> None:
    """Setting the preview block leaves the output block alone; 128 bpp is not settable and
    an empty request is refused (SCN-09 F2, F4)."""
    document = case.create("Blocks")
    before = properties(case, document)["output"]
    result = case.mutate(
        "scene.output.set",
        document,
        apply_to=["preview"],
        settings={"shrink": 2, "aspect_export": {"mode": "crop", "ratio": 2.39}},
        sync_color_settings=False,
    )
    case.assertEqual(result["output"], before)
    case.assertEqual(result["changed_fields"], ["preview"])
    case.assertEqual(result["preview"]["aspect_export"]["mode"], "crop")
    revision = case.revisions[document]
    case.mutate("scene.output.set", document, apply_to=["preview"], settings={"shrink": 2})
    case.assertEqual(case.revisions[document], revision)
    case.schema_failure(
        "scene.output.set",
        case.write_params(document, {"apply_to": ["output"], "settings": {"bpp": 128}}),
    )
    error = case.handler_failure(
        "scene.output.set",
        case.write_params(document, {"apply_to": ["output"], "sync_color_settings": True}),
        "invalid_params",
    )
    case.assertEqual(error["details"]["reason"], "sync_color_settings")
    _reopened(case, document, "blocks")


def _near(case: NativeAcceptance, values: list[float], expected: list[float]) -> None:
    case.assertEqual(len(values), len(expected))
    for value, wanted in zip(values, expected, strict=True):
        case.assertAlmostEqual(value, wanted, delta=1e-9)


@acceptance_case
def guides_change_no_pixel_and_reopen(case: NativeAcceptance) -> None:
    """Ruler guides are added, moved, duplicated and removed by replacing each list; a 2.39
    field guide changes no rendered pixel; view overlays are reported as user state, not
    scene data (SCN-09 #2, #3, #4, F3)."""
    document, _level, _column = case.populated_scene()
    pixels = case.preview(document)
    guides = {"vertical_lines_x_in": [1 / 3, -0.5], "horizontal_lines_y_in": [0.25]}
    result = case.mutate(
        "scene.guides.set",
        document,
        ruler_guides=guides,
        field_guide={"size": 12, "aspect_ratio": 2.39},
        markers={"interval": 8, "start_frame": 2},
    )
    _near(case, result["ruler_guides"]["vertical_lines_x_in"], [1 / 3, -0.5])
    case.assertLessEqual(result["max_canonicalization_error_in"], 1e-9)
    case.assertEqual(case.preview(document), pixels)
    moved = {"vertical_lines_x_in": [1 / 3, 1 / 3, 0.75], "horizontal_lines_y_in": []}
    result = case.mutate("scene.guides.set", document, ruler_guides=moved)
    case.assertEqual(result["changed_fields"], ["ruler_guides"])
    case.assertEqual(result["ruler_guides"]["horizontal_lines_y_in"], [])
    scope = {row["setting"]: row for row in properties(case, document)["settings_scope"]}
    case.assertEqual(
        (scope["safe_area"]["scope"], scope["safe_area"]["stored_in_scene"]), ("user_view", False)
    )
    case.assertEqual(scope["ruler_guides"]["editable_tool"], "scene.guides.set")
    reopened = _reopened(case, document, "guides")
    _near(
        case,
        properties(case, reopened)["ruler_guides"]["vertical_lines_x_in"],
        [1 / 3, 1 / 3, 0.75],
    )
