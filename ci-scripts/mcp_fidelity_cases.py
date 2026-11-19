"""Acceptance cases for render output fidelity: deep profiles, pipelines, conversions, omissions.

Each EXR artifact is decoded by `mcp_exr.py`, a standard-library reader that shares no code with
OpenEXR, and the worker's frames are compared with what the native batch renderer `tcomposer`
writes for the same configured scene. Comparisons are exact: the profiles are lossless and both
renderers run the same native pipeline, so no tolerance is stated for any channel, alpha
included.
"""

from __future__ import annotations

import base64
import hashlib
import shutil
import struct
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_exr import ExrImage, decode_exr_file
from mcp_native_oracle import OracleError, compare_files, find_tcomposer, render_with_tcomposer
from mcp_png import decode_png
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

RED_HALF = (255, 0, 0, 128)
SIZE = {"width": 24, "height": 16}
INTERMEDIATE = "fx_integer_intermediate"


def _half(value: float) -> float:
    return struct.unpack("<e", struct.pack("<e", value))[0]


def _single(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _card(case: NativeAcceptance, name: str) -> tuple[str, str]:
    document = case.create(name)
    effect = case.mutate("fx.create", document, type="colorCardFx", frame_count=2)
    case.mutate(
        "fx.parameter.set",
        document,
        fx_id=effect["fx_id"],
        parameter="color",
        value=list(RED_HALF),
    )
    case.mutate("fx.terminal.set", document, fx_id=effect["fx_id"], enabled=True)
    return document, effect["fx_id"]


def _completed(case: NativeAcceptance, job: dict[str, Any]) -> list[dict[str, Any]]:
    status = case.wait_job(job["job_id"])
    case.assertEqual(status["status"], "completed", status)
    return case.success("job.result", {"job_id": job["job_id"]})["artifacts"]


def _kinds(conversions: list[dict[str, Any]], avoidable: bool) -> list[str]:
    wanted = "avoidable" if avoidable else "inherent"
    return [row["kind"] for row in conversions if row["class"] == wanted]


def _exr(case: NativeAcceptance, artifact: dict[str, Any]) -> ExrImage:
    path = case.workspace / artifact["workspace_relative_path"]
    data = path.read_bytes()
    case.assertEqual(len(data), artifact["size_bytes"])
    case.assertEqual(hashlib.sha256(data).hexdigest(), artifact["sha256"])
    image = decode_exr_file(path)
    case.assertEqual((image.width, image.height), (artifact["width"], artifact["height"]))
    case.assertEqual([channel.name for channel in image.channels], ["A", "B", "G", "R"])
    case.assertEqual({channel.sample for channel in image.channels}, {artifact["sample"]})
    case.assertEqual(image.compression, "zip")
    return image


def _check_red(case: NativeAcceptance, image: ExrImage, rounded: Any) -> None:
    """Premultiplied linear red: straight red 1.0 stays 1.0, so R equals A."""
    alpha = rounded(128 / 255)
    expected = (rounded(alpha), 0.0, 0.0, alpha)
    values = {image.pixel(x, y) for y in range(image.height) for x in range(image.width)}
    case.assertEqual(values, {expected})


@acceptance_case
def exr_profiles_decode_and_report_conversions(case: NativeAcceptance) -> None:
    """Render half and float EXR: avoidable conversions refused, then accepted and recorded."""
    document, effect = _card(case, "EXR card")
    sequence = {"start_frame": 0, "end_frame": 1, **SIZE}
    report = case.success(
        "render.preflight",
        {
            "document_id": document,
            "expected_revision": case.revisions[document],
            "kind": "sequence",
            "profile": "rgba-half-exr",
            **sequence,
        },
    )
    case.assertEqual((report["ready"], report["first_failure"]), (False, "conversions"))
    effective = report["effective"]
    case.assertEqual(_kinds(effective["conversions"], False), ["half_float_range"])
    intermediate = [row for row in effective["conversions"] if row["kind"] == INTERMEDIATE]
    case.assertEqual([row["fx_id"] for row in intermediate], [effect])
    case.assertEqual(effective["color"]["pipeline"]["bpp"], 128)
    case.assertEqual(effective["color"]["output"]["transfer"], "linear")
    case.assertEqual(effective["alpha"], "premultiplied")
    error = case.failure(
        "render.submit", case.write_params(document, {"profile": "rgba-half-exr", **sequence})
    )
    case.assertEqual(error["code"], "unaccepted_conversion", error)
    case.assertEqual(error["details"]["conversions"], [INTERMEDIATE])
    case.assertFalse((case.workspace / "renders").exists())
    for profile, sample, rounded in (
        ("rgba-half-exr", "half", _half),
        ("rgba-float-exr", "float", _single),
    ):
        job = case.mutate(
            "render.submit",
            document,
            profile=profile,
            accept_conversions=[INTERMEDIATE],
            **sequence,
        )
        accepted = [row for row in job["effective"]["conversions"] if row["kind"] == INTERMEDIATE]
        case.assertEqual([row["accepted"] for row in accepted], [True])
        artifacts = _completed(case, job)
        case.assertEqual([row["kind"] for row in artifacts], ["exr", "exr"])
        case.assertEqual({row["sample"] for row in artifacts}, {sample})
        for artifact in artifacts:
            _check_red(case, _exr(case, artifact), rounded)
        refused = case.failure("artifact.read", {"artifact_id": artifacts[0]["artifact_id"]})
        case.assertEqual(refused["code"], "unsupported_artifact_kind")
        case.assertEqual(refused["details"]["hint"], "job.result")


@acceptance_case
def scene_pipeline_conversions_and_omissions(case: NativeAcceptance) -> None:
    """Stored output settings are listed; under the scene pipeline each one must be accepted."""
    document, _effect = _card(case, "Scene pipeline")
    case.mutate(
        "scene.output.set", document, apply_to=["output"], settings={"bpp": 64, "shrink": 2}
    )
    sequence = {"start_frame": 0, "end_frame": 0, **SIZE}
    default = case.mutate("render.submit", document, **sequence)
    effective = default["effective"]
    case.assertEqual(effective["omissions"], [{"setting": "shrink", "scene": 2}])
    case.assertEqual(_kinds(effective["conversions"], True), [])
    case.assertEqual(effective["color"]["scene_settings"]["bpp"], 64)
    case.assertEqual(effective["color"]["pipeline"]["bpp"], 32)
    baseline = _completed(case, default)[0]
    params = case.write_params(document, {"pipeline": "scene", **sequence})
    error = case.failure("render.submit", params)
    case.assertEqual(error["code"], "unaccepted_conversion", error)
    case.assertEqual(
        error["details"]["conversions"], ["quantize_to_8bit", "output_setting_ignored"]
    )
    accepted = ["quantize_to_8bit", "output_setting_ignored"]
    job = case.mutate(
        "render.submit", document, pipeline="scene", accept_conversions=accepted, **sequence
    )
    case.assertEqual(job["effective"]["color"]["pipeline"]["channel_bits"], 16)
    ignored = [
        row["setting"]
        for row in job["effective"]["conversions"]
        if row["kind"] == "output_setting_ignored"
    ]
    case.assertEqual(ignored, ["shrink"])
    deep = _completed(case, job)[0]
    first = case.success("artifact.read", {"artifact_id": baseline["artifact_id"]})
    second = case.success("artifact.read", {"artifact_id": deep["artifact_id"]})
    pixels = [
        decode_png(base64.b64decode(result["image"]["data"]))[2] for result in (first, second)
    ]
    case.assertEqual(pixels[0], pixels[1], "a 16-bit pipeline changed an 8-bit card's pixels")


def _oracle_worker(case: NativeAcceptance) -> None:
    case.worker.close()
    case.workspace = case.directory / "oracle-workspace"
    case.workspace.mkdir()
    case.worker = Worker(
        case.executable,
        case.workspace,
        case.inputs,
        case.directory / "oracle.stderr.log",
        test_arguments=("--test-render-fault", "keep-oracle-scene"),
    )
    case.addCleanup(case.worker.close)
    case.revisions.clear()


PROJECT = (
    "<project>\n  <version>\n    70 1 \n  </version>\n  <folders>\n"
    + "".join(
        f'    <folder name="{name}" path="{name}"/>\n'
        for name in ("inputs", "drawings", "scenes", "extras", "outputs")
    )
    + "  </folders>\n</project>\n"
)


def _as_project(snapshot: Path, destination: Path) -> Path:
    """The worker's configured scene as a project the batch renderer can open."""
    destination.mkdir(parents=True)
    (destination / "scenes").mkdir()
    if (snapshot / "assets").is_dir():
        shutil.copytree(snapshot / "assets", destination / "drawings")
    (destination / "oracle_otprj.xml").write_text(PROJECT, encoding="utf-8")
    (destination / "scenes" / "scenes.xml").write_text(
        '<parentProject type="projectFolder">\n  ".."\n</parentProject>\n', encoding="utf-8"
    )
    text = (snapshot / "oracle.tnz").read_text(encoding="utf-8")
    scene = destination / "scenes" / "oracle.tnz"
    scene.write_text(text.replace("$scenefolder/assets/", "+drawings/"), encoding="utf-8")
    return scene


def _native_frames(
    case: NativeAcceptance, tcomposer: Path, job: dict[str, Any], name: str, extension: str
) -> list[Path]:
    snapshot = case.workspace / "renders" / job["job_id"] / "snapshot"
    scene = _as_project(snapshot, case.directory / "batch" / name)
    output = case.directory / "batch" / f"{name}-frames"
    output.mkdir()
    try:
        return render_with_tcomposer(
            tcomposer,
            scene,
            output,
            range(job["start_frame"], job["end_frame"] + 1),
            stuff=case.directory / "batch" / "stuff",
            extension=extension,
        )
    except OracleError as error:
        case.fail(str(error))
        raise


@acceptance_case(
    worker_only="needs --test-render-fault keep-oracle-scene, which the adapter never passes"
)
def deep_profiles_match_the_native_batch_renderer(case: NativeAcceptance) -> None:
    """Worker EXR and PNG frames equal tcomposer's for the same configured scene, exactly."""
    tcomposer = find_tcomposer(case.native_executable.parent)
    if tcomposer is None:
        case.skipTest("no tcomposer next to the application: build the tcomposer target")
        return
    _oracle_worker(case)
    document, _effect = _card(case, "Oracle card")
    sequence = {"start_frame": 0, "end_frame": 1, **SIZE}
    for profile, extension in (
        ("rgba-half-exr", "exr"),
        ("rgba-float-exr", "exr"),
        ("rgba8-png", "png"),
    ):
        with case.subTest(profile=profile):
            accept = [INTERMEDIATE] if extension == "exr" else []
            extra: dict[str, Any] = {"accept_conversions": accept} if accept else {}
            job = case.mutate("render.submit", document, profile=profile, **sequence, **extra)
            artifacts = _completed(case, job)
            native = _native_frames(case, tcomposer, job, profile, extension)
            case.assertEqual(len(native), len(artifacts))
            frames = case.workspace / "renders" / job["job_id"] / "frames"
            for artifact, expected in zip(artifacts, native, strict=True):
                if extension == "png":
                    difference = compare_files(expected, frames / expected.name)
                    case.assertTrue(difference.equal, difference.problems)
                    continue
                theirs = decode_exr_file(expected)
                mine = _exr(case, artifact)
                case.assertEqual(mine.samples, theirs.samples, profile)
