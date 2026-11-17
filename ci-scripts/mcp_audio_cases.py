"""Exercise detached audio import, clips, volume, packages and exact mixdown on the worker."""

from __future__ import annotations

import hashlib
import json
import struct
from pathlib import Path
from typing import TYPE_CHECKING, Any

import mcp_movie_cases
from mcp_audio_model import (
    Clip,
    Column,
    Mix,
    Source,
    aiff_file,
    constant,
    offline_mix,
    pcm_sha256,
    tone,
    wav_file,
)

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

FIXTURE_RATES = ((12.5, 22050), (23.976, 44100), (24, 48000), (25, 44100), (29.97, 48000))


def _scene(case: NativeAcceptance, fps: float = 24) -> str:
    return case.mutate(
        "scene.create", None, name="Audio", width_px=32, height_px=32, frame_rate=fps
    )["document_id"]


def _read(case: NativeAcceptance, method: str, document: str, **params: Any) -> dict[str, Any]:
    return case.success(
        method, {"document_id": document, "expected_revision": case.revisions[document], **params}
    )


def _inspect(case: NativeAcceptance, document: str) -> dict[str, Any]:
    return _read(case, "audio.inspect", document)


def _write(case: NativeAcceptance, name: str, data: bytes) -> str:
    path = case.inputs / name
    path.write_bytes(data)
    return str(path)


def _import(case: NativeAcceptance, document: str, name: str, data: bytes) -> dict[str, Any]:
    path = _write(case, name + ".audio", data)
    return case.mutate("audio.import", document, path=path, name=name)["source"]


def _column(case: NativeAcceptance, document: str, volume: int = 1000) -> str:
    column = case.mutate("audio.column.create", document, name="Sound", volume_permille=volume)
    return column["column"]["column_id"]


def _placement(clip: Clip) -> dict[str, int]:
    return {
        "timeline_start_frame": clip.timeline,
        "source_start_frame": clip.start,
        "frame_count": clip.count,
    }


def _place_params(column: str, level: str, clip: Clip) -> dict[str, Any]:
    return {
        "column_id": column,
        "level_id": level,
        "placement": _placement(clip),
        "overlap_policy": "reject",
    }


def _place(
    case: NativeAcceptance, document: str, column: str, level: str, clip: Clip
) -> dict[str, Any]:
    return case.mutate("audio.clip.place", document, **_place_params(column, level, clip))["clip"]


def _reject(
    case: NativeAcceptance, document: str, method: str, params: dict[str, Any], code: str
) -> dict[str, Any]:
    before = _inspect(case, document)
    summary = case.success("scene.summary", {"document_id": document})
    error = case.failure(method, case.write_params(document, params))
    case.assertEqual(error["code"], code, error)
    case.assertEqual(_inspect(case, document), before)
    case.assertEqual(case.success("scene.summary", {"document_id": document}), summary)
    return error


def _mix_params(start: int, end: int, output: Path) -> dict[str, Any]:
    return {
        "start_frame": start,
        "end_frame": end,
        "output_path": str(output),
        "format_policy": "native_mix",
    }


def _mixdown(case: NativeAcceptance, document: str, start: int, end: int) -> dict[str, Any]:
    case.operations += 1
    output = case.workspace / f"mix-{case.operations}.wav"
    result = case.mutate("audio.mixdown", document, **_mix_params(start, end, output))
    case.assertEqual(result["revision"], case.revisions[document])
    return result


def _assert_mix(case: NativeAcceptance, result: dict[str, Any], expected: Mix) -> None:
    data = Path(result["output_path"]).read_bytes()
    case.assertEqual(result["pcm_sha256"], expected.sha256())
    case.assertEqual(data, expected.wav())
    case.assertEqual(result["sha256"], hashlib.sha256(data).hexdigest())
    case.assertEqual(result["sample_count"], len(expected.frames))
    case.assertEqual(result["size_bytes"], len(data))
    case.assertEqual(result["format"]["codec"], expected.codec)
    case.assertEqual(result["full_scale_sample_count"], expected.full_scale_count())
    case.assertAlmostEqual(result["peak"], expected.peak(), places=9)
    case.assertEqual(result["silent"], expected.peak() == 0)


def _raw_wav(tag: int, channels: int, bits: int, data: bytes, align: int | None = None) -> bytes:
    block = channels * bits // 8 if align is None else align
    fmt = struct.pack("<HHIIHH", tag, channels, 8000, 8000 * block, block, bits)
    body = b"WAVEfmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", len(data))
    return b"RIFF" + struct.pack("<I", len(body + data)) + body + data


def _bad_sources() -> dict[str, tuple[bytes, str]]:
    oversized = _raw_wav(1, 1, 16, bytes(8))
    oversized = oversized[:40] + struct.pack("<I", 0x7FFFFFF0) + oversized[44:]
    return {
        "zero-align": (_raw_wav(1, 1, 16, bytes(8), align=0), "invalid_asset"),
        "oversized": (oversized, "invalid_asset"),
        "truncated": (_raw_wav(1, 1, 16, bytes(8))[:-3], "invalid_asset"),
        "empty": (_raw_wav(1, 1, 16, b""), "invalid_asset"),
        "partial": (_raw_wav(1, 1, 16, bytes(3)), "invalid_asset"),
        "nan": (wav_file(Source(8000, "pcm_f32", ((float("nan"),),))), "invalid_asset"),
        "extensible": (_raw_wav(0xFFFE, 1, 16, bytes(8)), "unsupported_format"),
        "three-channels": (_raw_wav(1, 3, 16, bytes(12)), "unsupported_format"),
        "int32": (_raw_wav(1, 1, 32, bytes(8)), "unsupported_format"),
        "aifc": (aiff_file(constant(8000, "pcm_s16", 4, 1), b"AIFC"), "unsupported_format"),
        "aiff24": (aiff_file(constant(8000, "pcm_s24", 4, 1)), "unsupported_format"),
        "low-rate": (wav_file(constant(4000, "pcm_s16", 8, 1)), "unsupported_format"),
        "mp3": (b"ID3" + bytes(64), "unsupported_format"),
    }


def _expected_source(data: bytes, source: Source, fps: float) -> dict[str, Any]:
    frames = source.frame_count(fps)
    per_frame = source.rate / fps
    return {
        "sample_count": len(source.frames),
        "pcm_sha256": pcm_sha256(source),
        "source_sha256": hashlib.sha256(data).hexdigest(),
        "size_bytes": len(data),
        "native_frame_count": frames,
        "samples_per_frame": per_frame,
        "frame_rate": fps,
        "clip_count": 0,
        "lossy_source": False,
        "unreachable_tail_samples": max(0, len(source.frames) - int(frames * per_frame)),
    }


def _check_import(
    case: NativeAcceptance, document: str, name: str, data: bytes, source: Source
) -> dict[str, Any]:
    imported = _import(case, document, name, data)
    expected = _expected_source(data, source, 25)
    case.assertEqual({key: imported[key] for key in expected}, expected)
    case.assertEqual(imported["format"]["codec"], source.codec)
    case.assertEqual(imported["format"]["channels"], source.channels)
    case.assertEqual(imported["format"]["sample_rate"], source.rate)
    return imported


def audio_import_inspect_and_bounded_parser(case: NativeAcceptance) -> None:
    """Admit WAV/AIFF PCM exactly and refuse every malformed or unadmitted file unchanged."""
    document = _scene(case, 25)
    sources = {
        "mono16": tone(48000, "pcm_s16", 4800),
        "stereo24": tone(44100, "pcm_s24", 4411, channels=2),
        "u8": tone(22050, "pcm_u8", 2206),
        "float": tone(32000, "pcm_f32", 3200, channels=2),
    }
    imported = [_check_import(case, document, n, wav_file(s), s) for n, s in sources.items()]
    case.assertEqual({item["decoded_by"] for item in imported}, {"native_wav"})
    aiff = tone(16000, "pcm_s16", 1601, channels=2)
    record = _check_import(case, document, "aiff16", aiff_file(aiff), aiff)
    case.assertEqual((record["container"], record["decoded_by"]), ("aiff", "native_aiff"))
    signed = tone(8000, "pcm_s8", 801)
    case.assertEqual(
        _check_import(case, document, "s8", aiff_file(signed), signed)["format"]["sample_type"],
        "signed_int",
    )
    inspected = _inspect(case, document)
    case.assertEqual([item["name"] for item in inspected["sources"]], [*sources, "aiff16", "s8"])
    case.assertEqual(inspected["mix"]["format"], None)
    case.assertTrue(inspected["frame_rate_native_stable"])
    for name, (data, code) in _bad_sources().items():
        with case.subTest(source=name):
            path = _write(case, name + ".bad", data)
            _reject(case, document, "audio.import", {"path": path, "name": name}, code)
    _reject(
        case,
        document,
        "audio.import",
        {"path": str(case.inputs / "missing.wav"), "name": "missing"},
        "invalid_asset",
    )
    link = case.inputs / "link.wav"
    link.symlink_to(case.inputs / "mono16.audio")
    _reject(case, document, "audio.import", {"path": str(link), "name": "link"}, "path_denied")
    level = case.success(
        "level.inspect", {"document_id": document, "level_id": imported[0]["level_id"]}
    )
    case.assertEqual((level["format"], level["drawings"]), ("sound", []))


def _clip_fixture(case: NativeAcceptance) -> tuple[str, str, str, Source]:
    document = _scene(case, 24)
    source = constant(8000, "pcm_s16", 8000, 100)
    level = _import(case, document, "Dialogue", wav_file(source))["level_id"]
    return document, level, _column(case, document, 600), source


def _clip_rejections(case: NativeAcceptance, document: str, column: str, level: str) -> None:
    source = constant(8000, "pcm_s16", 8000, 100)
    error = _reject(
        case,
        document,
        "audio.clip.place",
        _place_params(column, level, Clip(source, 12, 0, 2)),
        "audio_overlap",
    )
    case.assertEqual(
        error["details"]["conflicts"], [{"column_id": column, "timeline_start_frame": 10}]
    )
    joined = _reject(
        case,
        document,
        "audio.clip.place",
        _place_params(column, level, Clip(source, 14, 6, 2)),
        "invalid_params",
    )
    case.assertEqual(joined["details"]["reason"], "contiguous_clip")
    bounds = _reject(
        case,
        document,
        "audio.clip.place",
        _place_params(column, level, Clip(source, 30, 20, 5)),
        "invalid_params",
    )
    case.assertEqual(bounds["details"]["native_frame_count"], 24)
    other = case.mutate("column.create", document, name="Art")["column_id"]
    _reject(
        case,
        document,
        "audio.clip.place",
        _place_params(other, level, Clip(source, 0, 0, 1)),
        "incompatible_column",
    )
    case.mutate("column.update", document, column_id=column, locked=True)
    _reject(
        case,
        document,
        "audio.column.set_volume",
        {"column_id": column, "volume_permille": 1},
        "locked",
    )
    _reject(
        case,
        document,
        "audio.clip.remove",
        {"clip": {"column_id": column, "timeline_start_frame": 10}},
        "locked",
    )
    case.mutate("column.update", document, column_id=column, locked=False)


def _volume_and_receipts(case: NativeAcceptance, document: str, column: str) -> None:
    revision = case.revisions[document]
    same = case.mutate("audio.column.set_volume", document, column_id=column, volume_permille=600)
    case.assertEqual((same["changed"], same["revision"]), (False, revision))
    request = case.write_params(document, {"column_id": column, "volume_permille": 250})
    changed = case.success("audio.column.set_volume", request)
    case.revisions[document] = changed["revision"]
    case.assertEqual(case.success("audio.column.set_volume", request), changed)
    conflict = dict(request, volume_permille=251)
    case.assertEqual(
        case.failure("audio.column.set_volume", conflict)["code"], "operation_conflict"
    )
    stale = case.write_params(document, {"column_id": column, "volume_permille": 9})
    stale["expected_revision"] = revision
    case.assertEqual(case.failure("audio.column.set_volume", stale)["code"], "revision_conflict")
    read = {"document_id": document, "expected_revision": revision}
    case.assertEqual(case.failure("audio.inspect", read)["code"], "revision_conflict")


def audio_clip_edits_reject_before_mutation(case: NativeAcceptance) -> None:
    """Place, update and remove clips by key, refusing overlap, joins, bounds and locks."""
    document, level, column, source = _clip_fixture(case)
    placed = _place(case, document, column, level, Clip(source, 10, 2, 4))
    case.assertEqual(
        {
            k: placed[k]
            for k in (
                "native_start_frame",
                "timeline_end_frame",
                "source_sample_start",
                "source_sample_end_inclusive",
            )
        },
        {
            "native_start_frame": 8,
            "timeline_end_frame": 13,
            "source_sample_start": 666,
            "source_sample_end_inclusive": 2000,
        },
    )
    _clip_rejections(case, document, column, level)
    _volume_and_receipts(case, document, column)
    key = {"column_id": column, "timeline_start_frame": 10}
    same = case.mutate(
        "audio.clip.update",
        document,
        clip=key,
        overlap_policy="reject",
        placement=_placement(Clip(source, 10, 2, 4)),
    )
    case.assertFalse(same["changed"])
    moved = case.mutate(
        "audio.clip.update",
        document,
        clip=key,
        overlap_policy="reject",
        placement=_placement(Clip(source, 0, 3, 9)),
    )
    case.assertEqual(
        (moved["clip"]["timeline_start_frame"], moved["clip"]["native_start_frame"]), (0, -3)
    )
    _reject(case, document, "audio.clip.remove", {"clip": key}, "not_found")
    _reject(case, document, "scene.settings", {"frame_rate": 25}, "timing_conflict")
    removed = case.mutate(
        "audio.clip.remove", document, clip={"column_id": column, "timeline_start_frame": 0}
    )
    case.assertEqual(removed["removed"], moved["clip"])
    inspected = _inspect(case, document)
    case.assertEqual(inspected["columns"][0]["clips"], [])
    case.assertEqual(inspected["sources"][0]["clip_count"], 0)
    case.mutate("scene.settings", document, frame_rate=24000 / 1001)
    _reject(
        case,
        document,
        "audio.clip.place",
        _place_params(column, level, Clip(source, 0, 0, 1)),
        "timing_conflict",
    )
    case.mutate("scene.settings", document, frame_rate=24)
    for index in range(256):
        _place(case, document, column, level, Clip(source, index * 2, 0, 1))
    _reject(
        case,
        document,
        "audio.clip.place",
        _place_params(column, level, Clip(source, 600, 0, 1)),
        "resource_limit",
    )


def _model_scene(
    case: NativeAcceptance, fps: float, rate: int
) -> tuple[str, list[Column], list[str]]:
    document = _scene(case, fps)
    a = tone(rate, "pcm_s16", rate, step=97)
    b = tone(rate, "pcm_s16", 2 * rate, step=211)
    levels = {
        id(source): _import(case, document, name, wav_file(source))["level_id"]
        for name, source in (("A", a), ("B", b))
    }
    columns = [
        Column([Clip(a, 0, 1, 5), Clip(b, 7, 3, 4)], 1000),
        Column([Clip(b, 2, 0, 9)], 750),
        Column([Clip(a, 0, 0, 6)], 0),
        Column([Clip(a, 1, 0, 3)], 1000, audible=False),
        Column([], 500),
        Column([Clip(a, 4, 2, 1)], 1000),
    ]
    ids = []
    for column in columns:
        ids.append(_column(case, document, column.volume_permille))
        for clip in column.clips:
            _place(case, document, ids[-1], levels[id(clip.source)], clip)
        if not column.audible:
            case.mutate("column.update", document, column_id=ids[-1], visible=False)
    return document, columns, ids


def audio_mixdown_matches_independent_model(case: NativeAcceptance) -> None:
    """Match the native offline mix sample for sample at fractional and integer rates."""
    for fps, rate in FIXTURE_RATES:
        document, columns, ids = _model_scene(case, fps, rate)
        for start, end in ((1, 9), (5, 9), (1, 6), (0, 12), (3, 3)):
            with case.subTest(fps=fps, rate=rate, start=start, end=end):
                expected = offline_mix(columns, start, end, fps)
                result = _mixdown(case, document, start, end)
                _assert_mix(case, result, expected)
                case.assertEqual(result["included_columns"], [ids[i] for i in expected.included])
                case.assertEqual(
                    result["excluded_columns"],
                    [{"column_id": ids[i], "reason": r} for i, r in expected.excluded],
                )
        case.mutate("scene.discard", document)


def _format_case(
    case: NativeAcceptance, columns: list[Column], start: int, end: int, fps: float = 24
) -> None:
    document = _scene(case, fps)
    levels: dict[int, str] = {}
    for column in columns:
        identifier = _column(case, document, column.volume_permille)
        for clip in column.clips:
            if id(clip.source) not in levels:
                name = f"S{len(levels)}"
                data = (
                    aiff_file(clip.source)
                    if clip.source.codec == "pcm_s8"
                    else wav_file(clip.source)
                )
                levels[id(clip.source)] = _import(case, document, name, data)["level_id"]
            _place(case, document, identifier, levels[id(clip.source)], clip)
    _assert_mix(case, _mixdown(case, document, start, end), offline_mix(columns, start, end, fps))
    case.mutate("scene.discard", document)


def audio_mixdown_formats_and_saturation(case: NativeAcceptance) -> None:
    """Promote 8-bit mixes, saturate integers, keep float overs and convert channel counts."""
    unsigned = tone(22050, "pcm_u8", 22050, step=7)
    _format_case(
        case,
        [Column([Clip(unsigned, 1, 0, 8)], 1000), Column([Clip(unsigned, 3, 2, 8)], 500)],
        0,
        11,
    )
    signed = tone(8000, "pcm_s8", 8000, step=3)
    _format_case(case, [Column([Clip(signed, 0, 0, 6)], 750)], 0, 7)
    wide = tone(48000, "pcm_s24", 48000, channels=2, step=5001)
    narrow = tone(48000, "pcm_s24", 48000, step=7919)
    _format_case(
        case, [Column([Clip(wide, 0, 0, 10)], 1000), Column([Clip(narrow, 2, 1, 10)], 250)], 0, 12
    )
    loud = constant(44100, "pcm_f32", 44100, 0.75, channels=2)
    _format_case(
        case, [Column([Clip(loud, 0, 0, 5)], 1000), Column([Clip(loud, 1, 0, 5)], 1000)], 0, 6, 25
    )
    hot = constant(44100, "pcm_s16", 44100, 30000)
    _format_case(
        case, [Column([Clip(hot, 0, 0, 4)], 1000), Column([Clip(hot, 2, 0, 4)], 1000)], 0, 6, 29.97
    )


def _without_ids(audio: dict[str, Any]) -> dict[str, Any]:
    text = json.dumps(audio, sort_keys=True)
    result = json.loads(text)
    for key in ("document_id", "revision"):
        result.pop(key)
    for source in result["sources"]:
        source.pop("level_id")
    for column in result["columns"]:
        column.pop("column_id")
        column.pop("object_id")
        for clip in column["clips"]:
            clip.pop("column_id")
            clip.pop("level_id")
    result["mix"].pop("audible_columns")
    result["mix"].pop("excluded_columns")
    return result


def _package_scene(case: NativeAcceptance) -> tuple[str, list[str], str]:
    document = _scene(case, 23.976)
    dialogue = tone(44100, "pcm_s16", 44100, channels=2, step=331)
    music = constant(44100, "pcm_s16", 88200, -1200, channels=2)
    one = _import(case, document, "Dialogue", wav_file(dialogue))["level_id"]
    two = _import(case, document, "Music", wav_file(music))["level_id"]
    ids = [_column(case, document, 900), _column(case, document, 1000), _column(case, document)]
    _place(case, document, ids[0], one, Clip(dialogue, 0, 3, 12))
    _place(case, document, ids[0], two, Clip(music, 14, 0, 6))
    _place(case, document, ids[2], two, Clip(music, 2, 10, 30))
    case.mutate("column.update", document, column_id=ids[2], visible=False, name="Muted")
    return document, ids, one


def _scene_tool_changes(case: NativeAcceptance, document: str, ids: list[str], level: str) -> None:
    summary = case.success("scene.summary", {"document_id": document})
    case.assertEqual({column["kind"] for column in summary["columns"]}, {"sound"})
    case.assertEqual({item["format"] for item in summary["levels"]}, {"sound"})
    cells = {"document_id": document, "column_id": ids[0], "start_frame": 0, "length": 2}
    case.assertEqual(case.failure("column.cells", cells)["code"], "incompatible_column")
    _reject(
        case, document, "column.update", {"column_id": ids[0], "opacity": 0.5}, "invalid_params"
    )
    _reject(case, document, "scene.settings", {"frame_rate": 30}, "timing_conflict")
    art = case.mutate("column.create", document, name="Art")["column_id"]
    _reject(
        case,
        document,
        "column.expose",
        {
            "column_id": art,
            "level_id": level,
            "drawing_id": {"number": 1, "suffix": ""},
            "start_frame": 0,
            "length": 1,
        },
        "incompatible_column",
    )
    case.mutate("column.delete", document, column_id=art)


def _package_rejections(case: NativeAcceptance, document: str, ids: list[str]) -> None:
    case.mutate("column.update", document, column_id=ids[1], locked=True)
    target = {"directory": str(case.workspace / "refused")}
    _reject(case, document, "scene.save_copy", target, "unsupported_column")
    case.mutate("column.update", document, column_id=ids[1], locked=False)
    trailing = _column(case, document)
    level = _inspect(case, document)["sources"][1]["level_id"]
    _place(case, document, trailing, level, Clip(constant(44100, "pcm_s16", 2, 0), 40, 0, 1))
    _reject(case, document, "scene.save_copy", target, "unsupported_column")
    case.mutate("column.delete", document, column_id=trailing)


def audio_save_reopen_render_and_scene_tools(case: NativeAcceptance) -> None:
    """Round-trip sound through packages bit for bit and keep renders device-free."""
    document, ids, level = _package_scene(case)
    _scene_tool_changes(case, document, ids, level)
    _package_rejections(case, document, ids)
    before = _inspect(case, document)
    mix = _mixdown(case, document, 0, 21)
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / "package"))
    info = case.success("scene.package_info", {"path": saved["package_directory"]})
    case.assertEqual(info["frame_rate"], 23.976)
    case.assertEqual(
        [entry["path"] for entry in info["sounds"]], ["assets/sound-0.wav", "assets/sound-1.wav"]
    )
    case.assertEqual([column["position"] for column in info["sound_columns"]], [0, 1, 2])
    reopened = case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]
    case.assertEqual(_without_ids(_inspect(case, reopened)), _without_ids(before))
    again = _mixdown(case, reopened, 0, 21)
    case.assertEqual(Path(again["output_path"]).read_bytes(), Path(mix["output_path"]).read_bytes())
    preview = case.preview(document)
    case.assertTrue(preview.startswith(b"\x89PNG"))
    asset = Path(saved["package_directory"]) / "assets" / "sound-0.wav"
    asset.write_bytes(asset.read_bytes()[:-2] + b"\x01\x02")
    reopen = case.write_params(None, {"path": saved["package_directory"]})
    case.assertEqual(case.failure("scene.open", reopen)["code"], "invalid_package")
    case.assertEqual(_without_ids(_inspect(case, reopened)), _without_ids(before))
    case.mutate("scene.save_copy", reopened, directory=str(case.workspace / "resaved"))


def _mixdown_failures(case: NativeAcceptance, document: str) -> None:
    existing = case.workspace / "existing.wav"
    existing.write_bytes(b"keep")
    for params, code in (
        (_mix_params(5, 4, case.workspace / "a.wav"), "invalid_params"),
        (_mix_params(0, 1, existing), "already_exists"),
        (_mix_params(0, 1, case.workspace / "missing" / "a.wav"), "missing_parent"),
        (_mix_params(0, 1, case.workspace / "a.mp3"), "invalid_path"),
        (_mix_params(0, 1, case.inputs / "a.wav"), "path_denied"),
        (_mix_params(0, 14400, case.workspace / "long.wav"), "resource_limit"),
        (_mix_params(0, 5640, case.workspace / "big.wav"), "resource_limit"),
    ):
        with case.subTest(code=code, output=params["output_path"]):
            _reject(case, document, "audio.mixdown", params, code)
    case.assertEqual(existing.read_bytes(), b"keep")


def audio_mixdown_rejections_and_receipts(case: NativeAcceptance) -> None:
    """Refuse unsafe outputs and budgets before writing, and replay exact retries."""
    document = _scene(case, 24)
    source = tone(48000, "pcm_s24", 48000 * 4, channels=2)
    level = _import(case, document, "Wide", wav_file(source))["level_id"]
    column = _column(case, document)
    error = _reject(
        case,
        document,
        "audio.mixdown",
        _mix_params(0, 1, case.workspace / "none.wav"),
        "invalid_params",
    )
    case.assertEqual(error["details"]["audible_columns"], 0)
    _place(case, document, column, level, Clip(source, 0, 0, 90))
    _mixdown_failures(case, document)
    request = case.write_params(document, _mix_params(0, 3, case.workspace / "retry.wav"))
    first = case.success("audio.mixdown", request)
    case.assertEqual(case.success("audio.mixdown", request), first)
    changed = dict(request, end_frame=4)
    case.assertEqual(case.failure("audio.mixdown", changed)["code"], "operation_conflict")
    case.assertEqual(
        sorted(p.name for p in case.workspace.glob("*.wav")), ["existing.wav", "retry.wav"]
    )
    unstable = _scene(case, 30000 / 1001)
    _reject(
        case,
        unstable,
        "audio.mixdown",
        _mix_params(0, 1, case.workspace / "b.wav"),
        "timing_conflict",
    )


def audio_movie_render_stays_video_only(case: NativeAcceptance) -> None:
    """Render MP4 from a scene with audible sound without adding an audio stream."""
    mcp_movie_cases._start_configured_worker(case, mcp_movie_cases._media_directory(case))
    document, _effect = mcp_movie_cases._fixture(case)
    source = tone(48000, "pcm_s16", 48000 * 2, channels=2)
    level = _import(case, document, "Score", wav_file(source))["level_id"]
    column = _column(case, document)
    _place(case, document, column, level, Clip(source, 0, 0, 20))
    job = case.mutate("render.submit", document, **mcp_movie_cases._options())
    artifact = mcp_movie_cases._movie_artifact(case, job)
    path = mcp_movie_cases._check_artifact(case, artifact)
    mcp_movie_cases._probe(case, path, artifact)
    case.assertEqual(_inspect(case, document)["columns"][0]["audible"], True)
