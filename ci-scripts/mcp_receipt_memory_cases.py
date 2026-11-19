"""Receipts dropped from a worker's memory once the journal holds them."""

from __future__ import annotations

from typing import TYPE_CHECKING

from mcp_case import acceptance_case
from mcp_journal_cases import created_scene, restart

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

SMALL = ("--test-journal-limits", "small")
# --test-journal-limits small keeps 4 receipts in memory and 16 full receipts in the journal.
WRITES = 10


@acceptance_case(worker_only="needs --test-journal-limits, which the adapter never passes")
def dropped_receipts_are_answered_from_the_journal(case: NativeAcceptance) -> None:
    """Past the in-memory receipt limit no write is refused; an exact retry of an operation
    whose receipt left memory returns the recorded result unchanged, byte for byte, and
    changes nothing, in this worker and after a restart."""
    restart(case, "small-receipts", *SMALL)
    create, created = created_scene(case)
    document = created["document_id"]
    first = case.write_params(document, {"width_px": 33})
    recorded = case.success("scene.settings", first)
    case.revisions[document] = recorded["revision"]
    for index in range(WRITES):
        params = case.write_params(document, {"width_px": 34 + index % 2})
        case.revisions[document] = case.success("scene.settings", params)["revision"]
    before = case.success("scene.summary", {"document_id": document})
    case.assertEqual(case.success("scene.create", create), created)
    case.assertEqual(case.success("scene.settings", first), recorded)
    case.assertEqual(case.success("scene.summary", {"document_id": document}), before)
    status = case.success("operation.status", {"operation_id": first["operation_id"]})
    case.assertEqual(status["state"], "completed", status)
    restart(case, "small-receipts-after", *SMALL)
    replay = case.success("scene.settings", first)
    case.assertIs(replay.pop("receipt")["handles_expired"], True)
    case.assertEqual(replay, recorded)
