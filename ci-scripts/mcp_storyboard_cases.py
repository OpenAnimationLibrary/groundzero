"""Exercise storyboard panels (root navigation tags) through the actual worker."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING, Any

from mcp_audio_model import constant, wav_file
from mcp_case import acceptance_case

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

NOTES = 'Shot "A" \\ café 日本 \U0001f3ac\n</tag> & #x;\n'


def _panels(case: NativeAcceptance, document: str) -> dict[str, Any]:
    return case.success(
        "storyboard.panels",
        {"document_id": document, "expected_revision": case.revisions[document]},
    )


def _create(
    case: NativeAcceptance, document: str, frame: int, caption: str, notes: str = ""
) -> dict[str, Any]:
    return case.mutate(
        "storyboard.panel.create",
        document,
        panel_frame=frame,
        caption=caption,
        notes=notes,
        color={"r": frame % 256, "g": 64, "b": 200},
    )


def _reject(
    case: NativeAcceptance, document: str, method: str, params: dict[str, Any], code: str
) -> None:
    before = _panels(case, document)
    error = case.failure(method, case.write_params(document, params))
    case.assertEqual(error["code"], code, error)
    case.assertEqual(_panels(case, document), before)


def _sound_above(case: NativeAcceptance, document: str) -> None:
    source = constant(48000, "pcm_s16", 48000, 300)
    path = case.inputs / "panel-sound.wav"
    path.write_bytes(wav_file(source))
    level = case.mutate("audio.import", document, path=str(path), name="Line")["source"]
    column = case.mutate("audio.column.create", document, name="Line", volume_permille=1000)
    case.mutate(
        "audio.clip.place",
        document,
        column_id=column["column"]["column_id"],
        level_id=level["level_id"],
        placement={"timeline_start_frame": 0, "source_start_frame": 0, "frame_count": 20},
        overlap_policy="reject",
    )


@acceptance_case
def storyboard_panels_spans_and_thumbnails(case: NativeAcceptance) -> None:
    """Report native spans and the drawing each panel shows, then render its frame."""
    document, level, _column = case.populated_scene()
    _sound_above(case, document)
    for frame in (10, 0, 30):
        _create(case, document, frame, f"Panel {frame}")
    listed = _panels(case, document)
    case.assertEqual(listed["frame_count"], 25)
    spans = [(p["panel_frame"], p["end_frame"], p["frame_count"]) for p in listed["panels"]]
    case.assertEqual(spans, [(0, 9, 10), (10, 29, 20), (30, 30, 1)])
    shown = listed["panels"][0]["thumbnail"]
    case.assertEqual(
        shown["exposed_drawing"],
        {"level_id": level["level_id"], "drawing_id": {"number": 1, "suffix": ""}},
    )
    case.assertEqual(
        (shown["render_with"], shown["source_revision"]),
        ("render.preview", case.revisions[document]),
    )
    case.assertIsNone(listed["panels"][2]["thumbnail"]["exposed_drawing"])
    case.assertTrue(case.preview(document).startswith(b"\x89PNG"))
    case.assertTrue(listed["older_reader_compatible"])
    case.assertFalse(listed["notes_present"])


def _comparable(panels: dict[str, Any]) -> dict[str, Any]:
    result = json.loads(json.dumps(panels))
    for key in ("document_id", "revision"):
        result.pop(key)
    for panel in result["panels"]:
        panel["thumbnail"].pop("source_revision")
    return result


@acceptance_case
def storyboard_text_round_trips_and_receipts(case: NativeAcceptance) -> None:
    """Keep Unicode captions and multi-line notes through packages and exact retries."""
    document = case.create("Storyboard")
    created = _create(case, document, 4, '日本 "one"', NOTES)
    case.assertFalse(created["older_reader_compatible"])
    case.assertEqual(created["panel"]["notes"], NOTES)
    request = case.write_params(
        document, {"panel_frame": 4, "new_panel_frame": 6, "color": {"r": 1, "g": 2, "b": 3}}
    )
    moved = case.success("storyboard.panel.update", request)
    case.revisions[document] = moved["revision"]
    case.assertEqual(case.success("storyboard.panel.update", request), moved)
    conflict = dict(request, new_panel_frame=7)
    case.assertEqual(
        case.failure("storyboard.panel.update", conflict)["code"], "operation_conflict"
    )
    revision = case.revisions[document]
    same = case.mutate("storyboard.panel.update", document, panel_frame=6, notes=NOTES)
    case.assertEqual((same["changed"], same["revision"]), (False, revision))
    stale = case.write_params(document, {"panel_frame": 6})
    stale["expected_revision"] = "0"
    case.assertEqual(case.failure("storyboard.panel.delete", stale)["code"], "revision_conflict")
    before = _panels(case, document)
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "panels"))
    info = case.success("scene.package_info", {"path": saved["package_directory"]})
    case.assertRegex(info["tags_sha256"], "^[0-9a-f]{64}$")
    reopened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    case.assertEqual(_comparable(_panels(case, reopened)), _comparable(before))
    deleted = case.mutate("storyboard.panel.delete", reopened, panel_frame=6)
    case.assertEqual((deleted["deleted"], deleted["panel_frame"]), (True, 6))
    case.assertEqual(_panels(case, reopened)["panels"], [])


@acceptance_case
def storyboard_rejections_preserve_panels(case: NativeAcceptance) -> None:
    """Refuse duplicates, missing panels, empty updates and control characters unchanged."""
    document = case.create("Storyboard")
    _create(case, document, 0, "First")
    _create(case, document, 8, "Second")
    color = {"r": 0, "g": 0, "b": 0}
    for method, params, code in (
        (
            "storyboard.panel.create",
            {"panel_frame": 8, "caption": "", "notes": "", "color": color},
            "already_exists",
        ),
        (
            "storyboard.panel.create",
            {"panel_frame": 2, "caption": "a\nb", "notes": "", "color": color},
            "invalid_params",
        ),
        (
            "storyboard.panel.create",
            {"panel_frame": 2, "caption": "", "notes": "a\rb", "color": color},
            "invalid_params",
        ),
        ("storyboard.panel.update", {"panel_frame": 0}, "invalid_params"),
        ("storyboard.panel.update", {"panel_frame": 3, "caption": "x"}, "not_found"),
        ("storyboard.panel.update", {"panel_frame": 0, "new_panel_frame": 8}, "already_exists"),
        ("storyboard.panel.delete", {"panel_frame": 5}, "not_found"),
    ):
        with case.subTest(method=method, code=code, params=params):
            _reject(case, document, method, params, code)
