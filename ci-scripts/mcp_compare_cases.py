"""Acceptance cases for the image review step: review.compare.submit (contract w3-compare).

Every metric, grid cell, component, difference image and pixel digest the worker returns is
recomputed from the PNG bytes by `mcp_compare_oracle.py` (standard library) and must be equal.
"""

from __future__ import annotations

import base64
import hashlib
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_compare_oracle import Request, compare, difference_pixels, pixel_sha256
from mcp_png import Pixel, decode_png

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

SIZE = 32


def _render(case: NativeAcceptance, document: str, **extra: Any) -> dict[str, Any]:
    params = {"start_frame": 0, "end_frame": 1, "width": SIZE, "height": SIZE, **extra}
    job = case.mutate("render.submit", document, **params)
    status = case.wait_job(job["job_id"])
    case.assertEqual(status["status"], "completed", status)
    return job


def _patch(case: NativeAcceptance, document: str, level: dict[str, Any]) -> None:
    """Paint a 5 x 3 white block, partly transparent, into the drawing every frame shows."""
    case.mutate(
        "raster.patch",
        document,
        level_id=level["level_id"],
        drawing_id={"number": 1, "suffix": ""},
        coordinate_space="raster_pixels_lower_left",
        x=3,
        y=20,
        width=5,
        height=3,
        sharing="all_exposures",
        pixels=[{"r": 160, "g": 160, "b": 160, "a": 160}] * 15,
    )


def _image(case: NativeAcceptance, artifact_id: str) -> tuple[dict[str, Any], list[Pixel]]:
    result = case.success("artifact.read", {"artifact_id": artifact_id})
    data = base64.b64decode(result["image"]["data"], validate=True)
    case.assertEqual(hashlib.sha256(data).hexdigest(), result["sha256"])
    width, height, pixels = decode_png(data)
    case.assertEqual((width, height), (result["width"], result["height"]))
    return result, pixels


def _rows(case: NativeAcceptance, job: str) -> list[dict[str, Any]]:
    status = case.wait_job(job)
    case.assertEqual(status["status"], "completed", status)
    rows: list[dict[str, Any]] = []
    offset: int | None = 0
    while offset is not None:
        page = case.success("job.result", {"job_id": job, "offset": offset, "limit": 4})
        rows.extend(page["rows"])
        offset = page["next_offset"]
    return rows


def _review(case: NativeAcceptance, document: str, **params: Any) -> dict[str, Any]:
    return case.mutate("review.compare.submit", document, **params)


def _check_pair(case: NativeAcceptance, row: dict[str, Any], request: Request) -> None:
    _, left = _image(case, row["reference"]["artifact_id"])
    _, right = _image(case, row["candidate"]["artifact_id"])
    case.assertEqual(row["reference"]["pixel_sha256"], pixel_sha256(left, SIZE, SIZE))
    case.assertEqual(row["candidate"]["pixel_sha256"], pixel_sha256(right, SIZE, SIZE))
    expected = compare(left, right, SIZE, request)
    case.assertEqual(row["metrics"], expected["metrics"])
    case.assertEqual(row["regions"], expected["regions"])
    image = row["difference_image"]
    case.assertEqual(image["status"], "written", image)
    result, pixels = _image(case, image["artifact_id"])
    case.assertEqual(result["kind"], "review_difference")
    wanted = difference_pixels(expected["difference"], request.threshold)
    case.assertEqual(pixels, wanted)
    case.assertEqual(image["pixel_sha256"], pixel_sha256(wanted, SIZE, SIZE))


@acceptance_case
def review_returns_images_and_metrics(case: NativeAcceptance) -> None:
    """Compare two frozen renders: metrics, regions, difference images and a sheet, all exact."""
    document, level, _column = case.populated_scene()
    before = _render(case, document)
    _patch(case, document, level)
    after = _render(case, document)
    revision = case.revisions[document]
    job = _review(
        case,
        document,
        range={"reference_job_id": before["job_id"], "candidate_job_id": after["job_id"]},
        grid={"columns": 3, "rows": 5},
        sheet=True,
    )
    case.assertEqual(job["kind"], "image_review")
    rows = _rows(case, job["job_id"])
    case.assertEqual(case.revisions[document], revision, "a review changed the document")
    pairs, summary = rows[:-1], rows[-1]
    case.assertEqual([row["reference"]["frame"] for row in pairs], [0, 1])
    request = Request(region=(0, 0, SIZE, SIZE), grid=(3, 5))
    for row in pairs:
        case.assertEqual(row["outcome"], "compared")
        case.assertEqual(row["conditions_differ"][0]["field"], "source_revision")
        case.assertEqual(
            {entry["field"] for entry in row["conditions_differ"]}, {"source_revision"}
        )
        case.assertEqual(row["verdict"]["tolerance"]["profile"], "native_fx_regression")
        case.assertFalse(row["verdict"]["passed"])
        _check_pair(case, row, request)
    case.assertEqual(summary["kind"], "summary")
    case.assertEqual((summary["compared"], summary["failed"]), (2, 2))
    case.assertEqual(
        summary["evidence"],
        {"kind": "visual", "basis": "decoded_pixels", "structural_claims": "none"},
    )
    sheet = summary["sheet"]
    result, pixels = _image(case, sheet["artifact_id"])
    case.assertEqual(result["kind"], "review_sheet")
    case.assertEqual((sheet["width"], sheet["height"], sheet["factor"]), (128, 88, 1))
    case.assertEqual(len(sheet["tiles"]), 6)
    case.assertEqual(sheet["pixel_sha256"], pixel_sha256(pixels, 128, 88))
    _, reference = _image(case, pairs[0]["reference"]["artifact_id"])
    tile = sheet["tiles"][0]
    copied = [pixels[(tile["y"] + y) * 128 + tile["x"] + x] for y in range(32) for x in range(32)]
    case.assertEqual(copied, reference)


@acceptance_case
def review_identical_renders_pass_exactly(case: NativeAcceptance) -> None:
    """The same frame rendered twice is identical; the exact profile passes and writes nothing."""
    document, _level, _column = case.populated_scene()
    first = _render(case, document)
    second = _render(case, document)
    frames = [
        case.success("job.result", {"job_id": job["job_id"]})["artifacts"][0]
        for job in (first, second)
    ]
    job = _review(
        case,
        document,
        pairs=[
            {
                "reference": {"kind": "artifact", "artifact_id": frames[0]["artifact_id"]},
                "candidate": {"kind": "job_frame", "job_id": second["job_id"], "frame": 0},
                "label": "repeat",
            }
        ],
        tolerance="exact",
    )
    rows = _rows(case, job["job_id"])
    pair = rows[0]
    case.assertEqual(pair["label"], "repeat")
    case.assertTrue(pair["metrics"]["identical_straight"])
    case.assertEqual(pair["verdict"]["passed"], True)
    case.assertEqual(pair["verdict"]["tolerance"]["mode"], "straight")
    case.assertEqual(pair["difference_image"]["status"], "no_change")
    case.assertEqual(rows[1]["images_written"], 0)
    case.assertEqual(pair["conditions_differ"], [])


@acceptance_case
def review_conditions_and_refusals_are_disclosed(case: NativeAcceptance) -> None:
    """Different sizes are never resampled; mismatched frames and conflicts refuse before a job."""
    document, _level, _column = case.populated_scene()
    small = _render(case, document)
    large = _render(case, document, width=40, height=40, start_frame=0, end_frame=2)
    job = _review(
        case,
        document,
        pairs=[
            {
                "reference": {"kind": "job_frame", "job_id": small["job_id"], "frame": 0},
                "candidate": {"kind": "job_frame", "job_id": large["job_id"], "frame": 1},
            }
        ],
    )
    pair = _rows(case, job["job_id"])[0]
    case.assertEqual(pair["outcome"], "size_mismatch")
    case.assertIsNone(pair["metrics"])
    fields = {entry["field"] for entry in pair["conditions_differ"]}
    case.assertEqual(fields, {"width", "height", "frame"})
    reviews = sorted((case.workspace / "reviews").iterdir())
    ranged = {"reference_job_id": small["job_id"], "candidate_job_id": large["job_id"]}
    error = case.failure("review.compare.submit", case.write_params(document, {"range": ranged}))
    case.assertEqual(error["code"], "frame_mismatch", error)
    case.assertEqual(
        (error["details"]["reference_lacks"], error["details"]["candidate_lacks"]), ([2], [])
    )
    conflict = {"range": ranged, "mode": "premultiplied", "tolerance": "exact"}
    error = case.failure("review.compare.submit", case.write_params(document, conflict))
    case.assertEqual(error["code"], "profile_conflict", error)
    both = {
        "range": ranged,
        "pairs": [
            {
                "reference": {"kind": "artifact", "artifact_id": "x"},
                "candidate": {"kind": "artifact", "artifact_id": "x"},
            }
        ],
    }
    error = case.failure("review.compare.submit", case.write_params(document, both))
    case.assertEqual(error["code"], "invalid_params", error)
    outside = {
        "range": {**ranged, "frames": {"start": 0, "end": 1}},
        "region": {"x": 30, "y": 0, "width": 8, "height": 8},
    }
    error = case.failure("review.compare.submit", case.write_params(document, outside))
    case.assertEqual(error["details"]["reason"], "region_outside", error)
    case.assertEqual(sorted((case.workspace / "reviews").iterdir()), reviews)
    frame = case.workspace / "renders" / small["job_id"] / "frames" / "frame.0002.png"
    frame.write_bytes(frame.read_bytes()[:-1] + b"\0")
    side = {"kind": "job_frame", "job_id": small["job_id"], "frame": 1}
    changed = _review(case, document, pairs=[{"reference": side, "candidate": side}])
    pair = _rows(case, changed["job_id"])[0]
    case.assertEqual((pair["outcome"], pair["metrics"]), ("source_changed", None))
