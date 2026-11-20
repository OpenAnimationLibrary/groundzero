"""Exercise lip-sync timing import and application through the actual worker."""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from fractions import Fraction
from typing import TYPE_CHECKING, Any

from mcp_audio_model import constant, wav_file
from mcp_case import acceptance_case

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

EXTENDED = ("A", "B", "C", "D", "E", "F", "G", "H", "X")
PRESTON = ("ai", "e", "o", "u", "fv", "l", "mbp", "wq", "etc", "rest")


@dataclass(frozen=True)
class Cue:
    start: int
    shape: str
    end: int | None = None


def _drawing(number: int) -> dict[str, Any]:
    return {"number": number, "suffix": ""}


def _seconds(micro: int) -> str:
    return f"{micro // 1_000_000}.{micro % 1_000_000:06d}"


def dat_file(cues: list[Cue]) -> bytes:
    lines = ["MohoSwitch1", *(f"{cue.start} {cue.shape}" for cue in cues), ""]
    return "\r\n".join(lines).encode()


def tsv_file(cues: list[Cue]) -> bytes:
    return "".join(f"{_seconds(cue.start)}\t{cue.shape}\n" for cue in cues).encode()


def json_file(cues: list[Cue]) -> bytes:
    mouth = [
        {"start": cue.start / 1e6, "end": (cue.end or 0) / 1e6, "value": cue.shape} for cue in cues
    ]
    return json.dumps(
        {"metadata": {"soundFile": "line.wav", "duration": 2.5}, "mouthCues": mouth}
    ).encode()


@dataclass(frozen=True)
class Timing:
    start: int
    fps: Fraction
    frames: bool
    hold: int | None = None


def _frame(timing: Timing, value: int) -> int:
    if timing.frames:
        return timing.start + value - 1
    return timing.start + int(Fraction(value, 1_000_000) * timing.fps)


def oracle_spans(cues: list[Cue], timing: Timing) -> tuple[list[tuple[int, int, str]], list[int]]:
    """Return (first, last, shape) spans and dropped cue indices with exact rationals."""
    spans: list[tuple[int, int, str]] = []
    dropped: list[int] = []
    for index, cue in enumerate(cues):
        first = _frame(timing, cue.start)
        if index + 1 < len(cues):
            following = _frame(timing, cues[index + 1].start)
        elif timing.hold is not None:
            following = timing.hold + 1
        elif cue.end is not None and not timing.frames:
            following = _frame(timing, cue.end)
        else:
            following = first + 1
        if following > first:
            spans.append((first, following - 1, cue.shape))
        else:
            dropped.append(index)
    return spans, dropped


def oracle_runs(spans: list[tuple[int, int, str]], mapping: dict[str, int]) -> list[list[Any]]:
    runs: list[list[Any]] = []
    for first, last, shape in spans:
        number = mapping[shape]
        if runs and runs[-1][2] == number and runs[-1][1] + 1 == first:
            runs[-1][1] = last
        else:
            runs.append([first, last, number, ""])
    return runs


def _runs_sha256(runs: list[list[Any]]) -> str:
    return hashlib.sha256(json.dumps(runs, separators=(",", ":")).encode()).hexdigest()


def _drawing_scene(case: NativeAcceptance, fps: float, drawings: int) -> tuple[str, str, str]:
    document = case.mutate(
        "scene.create", None, name="Lipsync", width_px=32, height_px=32, frame_rate=fps
    )["document_id"]
    level = case.mutate("level.create", document, name="Mouth", format="vector")["level_id"]
    for number in range(1, drawings + 1):
        case.mutate(
            "drawing.create",
            document,
            level_id=level,
            drawing_id=_drawing(number),
            format="vector",
        )
    column = case.mutate("column.create", document, name="Lips")["column_id"]
    return document, level, column


def _import(case: NativeAcceptance, name: str, data: bytes, **params: Any) -> dict[str, Any]:
    path = case.inputs / name
    path.write_bytes(data)
    return case.mutate("lipsync.import", None, path=str(path), **params)


def _apply_params(
    level: str, column: str, analysis: str, fps: float, start: int, mapping: dict[str, int]
) -> dict[str, Any]:
    return {
        "analysis_id": analysis,
        "column_id": column,
        "level_id": level,
        "start_frame": start,
        "expected_frame_rate": fps,
        "mapping": [{"shape": s, "drawing_id": _drawing(n)} for s, n in mapping.items()],
        "unmapped_policy": {"kind": "reject"},
        "end_policy": {"kind": "last_cue_end"},
        "overwrite": "replace_range",
    }


def _cell_runs(case: NativeAcceptance, document: str, column: str, first: int, last: int) -> list:
    cells = case.success(
        "column.cells",
        {
            "document_id": document,
            "column_id": column,
            "start_frame": first,
            "length": last - first + 1,
        },
    )["cells"]
    runs: list[list[Any]] = []
    for cell in cells:
        number = None if cell["empty"] else cell["drawing_id"]["number"]
        if runs and runs[-1][2] == number and runs[-1][1] + 1 == cell["frame"]:
            runs[-1][1] = cell["frame"]
        else:
            runs.append([cell["frame"], cell["frame"], number, ""])
    return runs


def _check_apply(
    case: NativeAcceptance,
    document: str,
    column: str,
    result: dict[str, Any],
    runs: list[list[Any]],
) -> None:
    case.assertEqual(result["runs_sha256"], _runs_sha256(runs))
    case.assertEqual(result["runs_count"], len(runs))
    case.assertEqual((result["first_frame"], result["last_frame"]), (runs[0][0], runs[-1][1]))
    padded = _cell_runs(case, document, column, runs[0][0] - 1, runs[-1][1] + 1)
    case.assertEqual(padded[1:-1], runs)
    case.assertIsNone(padded[0][2])
    case.assertIsNone(padded[-1][2])


DAT_CUES = [
    Cue(1, "rest"),
    Cue(4, "ai"),
    Cue(6, "e"),
    Cue(9, "other"),
    Cue(12, "MBP"),
    Cue(15, "rest"),
]
DAT_MAPPING = {"rest": 1, "ai": 2, "e": 3, "etc": 4, "mbp": 5}


@acceptance_case
def lipsync_dat_import_inspect_and_apply(case: NativeAcceptance) -> None:
    """Apply a Papagayo file from its first key's frame and keep it through a package."""
    document, level, column = _drawing_scene(case, 24, 5)
    imported = _import(
        case,
        "speech.dat",
        dat_file(DAT_CUES),
        format="papagayo_dat",
        shape_set="preston_blair",
        frame_rate=24,
    )
    case.assertEqual(imported["cue_count"], 6)
    case.assertEqual(imported["shapes_used"], ["ai", "e", "mbp", "etc", "rest"])
    case.assertEqual(
        imported["timebase"], {"kind": "frames", "frame_rate": 24, "first_frame_number": 1}
    )
    case.assertIsNone(imported["linked_source"])
    page = case.success(
        "lipsync.inspect", {"analysis_id": imported["analysis_id"], "offset": 2, "limit": 3}
    )
    case.assertEqual([cue["shape"] for cue in page["cues"]], ["e", "etc", "mbp"])
    case.assertEqual(page["next_offset"], 5)
    canonical = [
        Cue(c.start, {"other": "etc", "MBP": "mbp"}.get(c.shape, c.shape)) for c in DAT_CUES
    ]
    spans, dropped = oracle_spans(canonical, Timing(10, Fraction(24), True))
    runs = oracle_runs(spans, DAT_MAPPING)
    request = case.write_params(
        document, _apply_params(level, column, imported["analysis_id"], 24, 10, DAT_MAPPING)
    )
    applied = case.success("lipsync.apply", request)
    case.revisions[document] = applied["revision"]
    case.assertEqual((applied["dropped_cue_count"], dropped), (0, []))
    _check_apply(case, document, column, applied, runs)
    case.assertEqual(case.success("lipsync.apply", request), applied)
    conflict = dict(request, start_frame=11)
    case.assertEqual(case.failure("lipsync.apply", conflict)["code"], "operation_conflict")
    again = case.mutate(
        "lipsync.apply",
        document,
        **_apply_params(level, column, imported["analysis_id"], 24, 10, DAT_MAPPING),
    )
    case.assertEqual((again["changed"], again["revision"]), (False, applied["revision"]))
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "lips"))
    reopened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    case.assertEqual(
        _cell_runs(
            case,
            reopened,
            case.success("scene.summary", {"document_id": reopened})["columns"][0]["column_id"],
            10,
            24,
        ),
        runs,
    )


def _boundary_cues(fps: Fraction, frames: int) -> list[Cue]:
    starts: list[int] = [0]
    for frame in range(1, frames):
        boundary = Fraction(frame, 1) / fps * 1_000_000
        exact = int(boundary)
        for micro in (exact - 1, exact, exact + 1):
            if micro > starts[-1]:
                starts.append(micro)
    return [Cue(micro, EXTENDED[index % len(EXTENDED)]) for index, micro in enumerate(starts)]


def _linked_source(case: NativeAcceptance) -> tuple[str, dict[str, Any]]:
    owner = case.mutate("scene.create", None, name="Voice", frame_rate=24)["document_id"]
    path = case.inputs / "voice.wav"
    path.write_bytes(wav_file(constant(16000, "pcm_s16", 16000, 900)))
    source = case.mutate("audio.import", owner, path=str(path), name="Voice")["source"]
    link = {
        "document_id": owner,
        "expected_revision": case.revisions[owner],
        "level_id": source["level_id"],
    }
    return owner, {"link": link, "pcm": source["pcm_sha256"]}


def _rational_case(case: NativeAcceptance, text: str, link: dict[str, Any]) -> None:
    fps = Fraction(text)
    document, level, column = _drawing_scene(case, float(text), len(EXTENDED))
    mapping = {shape: index + 1 for index, shape in enumerate(EXTENDED)}
    # A cue exactly on a frame boundary that binary floating point misplaces.
    exact = {"23.976": 125_000_000, "29.97": 100_000_000, "12.5": 2_320_000}[text]
    cues = [*_boundary_cues(fps, 8), Cue(exact - 1, "G"), Cue(exact, "H")]
    tsv = _import(
        case,
        f"line-{text}.tsv",
        tsv_file(cues),
        format="rhubarb_tsv",
        shape_set="rhubarb_extended",
        source_link=link["link"],
    )
    case.assertEqual(tsv["linked_source"]["pcm_sha256"], link["pcm"])
    spans, dropped = oracle_spans(cues, Timing(3, fps, False))
    applied = case.mutate(
        "lipsync.apply",
        document,
        **_apply_params(level, column, tsv["analysis_id"], float(text), 3, mapping),
    )
    case.assertEqual(applied["dropped_cue_count"], len(dropped))
    case.assertEqual([cue["index"] for cue in applied["dropped_cues"]], dropped)
    _check_apply(case, document, column, applied, oracle_runs(spans, mapping))
    ends = [Cue(c.start, c.shape, n.start) for c, n in zip(cues, cues[1:], strict=False)]
    data = json_file(ends)
    analysis = _import(
        case, f"line-{text}.json", data, format="rhubarb_json", shape_set="rhubarb_extended"
    )["analysis_id"]
    start = 3200
    hold = _frame(Timing(start, fps, False), ends[-1].start) + 5
    params = _apply_params(level, column, analysis, float(text), start, mapping)
    params["end_policy"] = {"kind": "hold_until_frame", "frame": hold}
    held = case.mutate("lipsync.apply", document, **params)
    spans, _dropped = oracle_spans(ends, Timing(start, fps, False, hold))
    _check_apply(case, document, column, held, oracle_runs(spans, mapping))


@acceptance_case
def lipsync_rhubarb_rational_timing(case: NativeAcceptance) -> None:
    """Map microsecond cues to frames with exact rationals at 23.976, 29.97 and 12.5 fps."""
    owner, link = _linked_source(case)
    for text in ("23.976", "29.97", "12.5"):
        with case.subTest(frame_rate=text):
            _rational_case(case, text, link)
    analysis = _import(
        case,
        "stale.tsv",
        tsv_file([Cue(0, "A")]),
        format="rhubarb_tsv",
        shape_set="rhubarb_basic",
        source_link=link["link"],
    )["analysis_id"]
    document, level, column = _drawing_scene(case, 24, 1)
    case.mutate("scene.discard", owner)
    params = _apply_params(level, column, analysis, 24, 0, {"A": 1})
    error = case.failure("lipsync.apply", case.write_params(document, params))
    case.assertEqual(error["code"], "stale_analysis")


MALFORMED: tuple[tuple[str, bytes, dict[str, Any], str], ...] = (
    ("header.dat", b"1 rest\n", {"format": "papagayo_dat", "frame_rate": 24}, "invalid_asset"),
    (
        "order.dat",
        b"MohoSwitch1\n3 ai\n2 e\n",
        {"format": "papagayo_dat", "frame_rate": 24},
        "invalid_asset",
    ),
    (
        "noise.dat",
        b"MohoSwitch1\n1 ai\n# comment\n",
        {"format": "papagayo_dat", "frame_rate": 24},
        "invalid_asset",
    ),
    ("rate.dat", b"MohoSwitch1\n1 ai\n", {"format": "papagayo_dat"}, "invalid_params"),
    ("digits.tsv", b"0.1234567\tA\n", {"format": "rhubarb_tsv"}, "invalid_asset"),
    ("shape.tsv", b"0.00\tX\n", {"format": "rhubarb_tsv"}, "invalid_asset"),
    (
        "gap.json",
        json_file([Cue(0, "A", 100000), Cue(200000, "B", 300000)]),
        {"format": "rhubarb_json"},
        "invalid_asset",
    ),
    ("large.tsv", b"0" * (1024 * 1024 + 1), {"format": "rhubarb_tsv"}, "resource_limit"),
)


def _apply_rejections(
    case: NativeAcceptance, document: str, level: str, column: str, analysis: str
) -> None:
    sound_column = case.mutate("audio.column.create", document, name="Voice", volume_permille=1000)[
        "column"
    ]["column_id"]
    path = case.inputs / "own.wav"
    path.write_bytes(wav_file(constant(16000, "pcm_s16", 16000, 1)))
    sound = case.mutate("audio.import", document, path=str(path), name="Own")["source"]
    base = _apply_params(level, column, analysis, 24, 0, {"ai": 1, "etc": 2})
    for change, code in (
        ({"expected_frame_rate": 25}, "timing_conflict"),
        ({"mapping": [{"shape": "ai", "drawing_id": _drawing(1)}]}, "invalid_params"),
        (
            {
                "mapping": [
                    {"shape": "ai", "drawing_id": _drawing(1)},
                    {"shape": "etc", "drawing_id": _drawing(7)},
                ]
            },
            "missing_drawing",
        ),
        ({"column_id": sound_column}, "incompatible_column"),
        ({"level_id": sound["level_id"]}, "unsupported_level"),
        ({"analysis_id": "analysis_missing"}, "invalid_handle"),
        ({"end_policy": {"kind": "hold_until_frame", "frame": 0}}, "invalid_params"),
    ):
        with case.subTest(code=code, change=change):
            before = _cell_runs(case, document, column, 0, 8)
            error = case.failure("lipsync.apply", case.write_params(document, {**base, **change}))
            case.assertEqual(error["code"], code, error)
            case.assertEqual(_cell_runs(case, document, column, 0, 8), before)
            if code == "invalid_params" and "mapping" in change:
                case.assertEqual(error["details"]["unmapped_shapes"], ["etc"])
    case.mutate("column.update", document, column_id=column, locked=True)
    error = case.failure("lipsync.apply", case.write_params(document, base))
    case.assertEqual(error["code"], "locked")


@acceptance_case
def lipsync_rejections_preserve_cells(case: NativeAcceptance) -> None:
    """Refuse malformed files and invalid applies before any record or cell changes."""
    for name, data, params, code in MALFORMED:
        with case.subTest(file=name):
            path = case.inputs / name
            path.write_bytes(data)
            shape_set = "preston_blair" if name.endswith(".dat") else "rhubarb_basic"
            error = case.failure(
                "lipsync.import",
                case.write_params(None, {"path": str(path), "shape_set": shape_set, **params}),
            )
            case.assertEqual(error["code"], code, error)
            if code == "invalid_asset" and not name.endswith(".json"):
                case.assertIn("line", error["details"])
    document, level, column = _drawing_scene(case, 24, 2)
    analysis = _import(
        case,
        "short.dat",
        dat_file([Cue(1, "ai"), Cue(3, "etc")]),
        format="papagayo_dat",
        shape_set="preston_blair",
        frame_rate=24,
    )
    late = _import(
        case,
        "late.dat",
        dat_file([Cue(1, "ai")]),
        format="papagayo_dat",
        shape_set="preston_blair",
        frame_rate=25,
    )
    params = _apply_params(level, column, late["analysis_id"], 24, 0, {"ai": 1})
    error = case.failure("lipsync.apply", case.write_params(document, params))
    case.assertEqual(error["code"], "timing_conflict")
    _apply_rejections(case, document, level, column, analysis["analysis_id"])
