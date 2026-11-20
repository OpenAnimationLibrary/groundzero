"""Qualify drawing analysis jobs on read-only documents, run in their scene hosts."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_scene_access_cases import corpus, open_scene, wait
from mcp_studio_palette_cases import call

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

DRAWING = {"number": 1, "suffix": ""}


def _level(case: NativeAcceptance, document: str, name: str) -> str:
    levels = call(case, case.worker, "scene.summary", {"document_id": document})["levels"]
    return next(level["level_id"] for level in levels if level["name"] == name)


def _rows(case: NativeAcceptance, document: str, revision: str, name: str) -> list[Any]:
    params = {
        "document_id": document,
        "expected_revision": revision,
        "operation_id": f"analysis-{name}",
        "level_id": _level(case, document, "Ink"),
        "check": "roles",
        "drawing_ids": [DRAWING],
    }
    job = call(case, case.worker, "drawing.analysis.submit", params)
    case.assertEqual(wait(case, case.worker, job["job_id"])["status"], "completed")
    result = call(case, case.worker, "job.result", {"job_id": job["job_id"], "limit": 256})
    case.assertIs(result["complete"], True, result)
    return [
        {key: value for key, value in row.items() if not key.endswith("_id")}
        for row in result["rows"]
    ]


@acceptance_case
def read_only_drawing_analysis_runs_in_the_host(case: NativeAcceptance) -> None:
    """A read-only document's drawing analysis runs in its scene host, bound to its
    sources, and gives the rows the same drawing gives in an editable document."""
    scenes = corpus(case)
    document = open_scene(case, case.worker, scenes["B"], "standalone")["document_id"]
    probe = {
        "document_id": document,
        "expected_revision": "0",
        "operation_id": "analysis-probe",
        "level_id": _level(case, document, "Ink"),
        "check": "roles",
        "drawing_ids": [DRAWING],
    }
    job = call(case, case.worker, "drawing.analysis.submit", probe)
    case.assertEqual(job["executor"], "scene_host")
    case.assertTrue(job["inputs"]["source_fingerprint"].startswith("source:"))
    wait(case, case.worker, job["job_id"])
    read_only = _rows(case, document, "0", "read-only")
    opened = case.mutate("scene.open", None, path=str(scenes["base"]))["document_id"]
    editable = _rows(case, opened, case.revisions[opened], "editable")
    case.assertTrue(read_only)
    case.assertEqual(read_only, editable)
