"""Verify detached native FX macros through graph ownership, receipts, and rendered pixels."""

from __future__ import annotations

import base64
import hashlib
from typing import TYPE_CHECKING, Any

import mcp_fx_authoring_cases as fx
from mcp_case import acceptance_case
from mcp_png import decode_png

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


FRAMES = (0, 6, 12)


def _chain(case: NativeAcceptance) -> tuple[str, str, str, str]:
    document, artwork, blur = fx._blur_fixture(case)
    glow = case.mutate("fx.create", document, type="STD_glowFx", name="Root glow")["fx_id"]
    case.mutate("fx.connect", document, fx_id=glow, port="Source", source_fx_id=blur)
    case.mutate("fx.terminal.set", document, fx_id=glow, enabled=True)
    fx._output(case, document, glow)
    for frame, value in ((0, 0), (12, 8)):
        case.mutate(
            "fx.parameter.set_key",
            document,
            fx_id=blur,
            parameter="value",
            frame=frame,
            value=value,
        )
    return document, artwork, blur, glow


def _macro(case: NativeAcceptance, document: str, members: list[str]) -> dict[str, Any]:
    return case.mutate("fx.macro.create", document, fx_ids=members, name="Grouped 光")


def _reopen(case: NativeAcceptance, document: str, directory: str) -> str:
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / directory))
    case.mutate("scene.discard", document)
    return case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]


def _member_shape(node: dict[str, Any]) -> list[dict[str, Any]]:
    members = []
    for member in node["macro"]["members"]:
        copy = {**member, "ports": []}
        for port in member["ports"]:
            copy["ports"].append({**port, "source_fx_id": bool(port["source_fx_id"])})
        members.append(copy)
    return members


def _assert_frames(case: NativeAcceptance, document: str, expected: dict[int, Any]) -> None:
    for frame, pixels in expected.items():
        case.assertEqual(fx._pixels(case, document, frame), pixels, frame)


def _assert_snapshot_frames(
    case: NativeAcceptance, document: str, expected: dict[int, Any]
) -> None:
    revision = case.revisions[document]
    job = case.mutate(
        "render.submit", document, start_frame=0, end_frame=12, step=6, width=32, height=32
    )
    status = case.wait_job(job["job_id"])
    case.assertEqual(status["status"], "completed", status)
    case.assertTrue(status["writers_closed"])
    artifacts = case.success("job.result", {"job_id": job["job_id"]})["artifacts"]
    case.assertEqual([artifact["frame"] for artifact in artifacts], list(FRAMES))
    for artifact in artifacts:
        result = case.success("artifact.read", {"artifact_id": artifact["artifact_id"]})
        case.assertEqual(result["source_revision"], revision)
        data = base64.b64decode(result["image"]["data"], validate=True)
        case.assertEqual(hashlib.sha256(data).hexdigest(), artifact["sha256"])
        width, height, pixels = decode_png(data)
        case.assertEqual((width, height), (32, 32))
        case.assertEqual(pixels, expected[artifact["frame"]])


def _assert_retired(case: NativeAcceptance, document: str, handles: list[str]) -> None:
    graph = fx._graph(case, document)
    live = {node["fx_id"] for node in graph["effects"]}
    case.assertTrue(live.isdisjoint(handles))
    for handle in handles:
        failure = case.failure("fx.parameter.get", {"document_id": document, "fx_id": handle})
        case.assertEqual(failure["code"], "invalid_handle")
    case.assertEqual(fx._graph(case, document), graph)


@acceptance_case
def macro_roundtrip_preserves_pixels_and_mapping(case: NativeAcceptance) -> None:
    """Keep keyed rendering, boundaries, identities and terminal routing through native reload.

    Args:
        case: A fresh actual native-worker acceptance fixture.
    """
    document, artwork, blur, glow = _chain(case)
    before = {frame: fx._pixels(case, document, frame) for frame in FRAMES}
    case.assertEqual(len({tuple(pixels) for pixels in before.values()}), len(FRAMES))
    grouped = _macro(case, document, [blur, glow])
    node = grouped["node"]
    case.assertTrue(node["terminal"])
    case.assertEqual(grouped["retired_fx_ids"], [blur, glow])
    mapping = {item["source_fx_id"]: item["member_key"] for item in grouped["member_mapping"]}
    case.assertEqual(node["macro"]["root_member_key"], mapping[glow])
    external = [item for item in node["macro"]["input_mapping"] if item["source_fx_id"]]
    case.assertEqual([item["source_fx_id"] for item in external], [artwork])
    _assert_retired(case, document, [blur, glow])
    _assert_frames(case, document, before)
    _assert_snapshot_frames(case, document, before)
    _roundtrip_group(case, document, node, before)


def _roundtrip_group(
    case: NativeAcceptance, document: str, node: dict[str, Any], before: dict[int, Any]
) -> None:
    original_members = _member_shape(node)
    macro_id = node["fx_id"]
    root_key = node["macro"]["root_member_key"]
    restored = _reopen(case, document, "grouped-scene")
    reopened = next(
        item for item in fx._graph(case, restored)["effects"] if item["type"] == "macroFx"
    )
    case.assertNotEqual(reopened["fx_id"], macro_id)
    case.assertEqual(_member_shape(reopened), original_members)
    case.assertEqual(reopened["macro"]["root_member_key"], root_key)
    _assert_frames(case, restored, before)
    expanded = case.mutate("fx.macro.expand", restored, fx_id=reopened["fx_id"])
    case.assertEqual(expanded["retired_macro_fx_id"], reopened["fx_id"])
    expanded_mapping = {item["member_key"]: item["fx_id"] for item in expanded["member_mapping"]}
    case.assertEqual(expanded["root_fx_id"], expanded_mapping[root_key])
    case.assertTrue(fx._node(case, restored, expanded["root_fx_id"])["terminal"])
    _assert_retired(case, restored, [reopened["fx_id"]])
    _assert_frames(case, restored, before)
    final = _reopen(case, restored, "expanded-scene")
    case.assertFalse(any(item["type"] == "macroFx" for item in fx._graph(case, final)["effects"]))
    _assert_frames(case, final, before)


@acceptance_case
def macro_retries_retire_handles_without_reapplying(case: NativeAcceptance) -> None:
    """Replay original create/expand receipts even after their published handles retire.

    Args:
        case: A fresh actual native-worker acceptance fixture.
    """
    document, _, blur, glow = _chain(case)
    params = case.write_params(document, {"fx_ids": [glow, blur], "name": "Receipt macro"})
    grouped = case.success("fx.macro.create", params)
    case.revisions[document] = grouped["revision"]
    case.assertEqual(grouped["retired_fx_ids"], [glow, blur])
    case.assertEqual([item["source_fx_id"] for item in grouped["member_mapping"]], [glow, blur])
    graph = fx._graph(case, document)
    case.assertEqual(case.success("fx.macro.create", params), grouped)
    case.assertEqual(fx._graph(case, document), graph)
    expand_params = case.write_params(document, {"fx_id": grouped["node"]["fx_id"]})
    expanded = case.success("fx.macro.expand", expand_params)
    case.revisions[document] = expanded["revision"]
    graph = fx._graph(case, document)
    case.assertEqual(case.success("fx.macro.expand", expand_params), expanded)
    case.assertEqual(case.success("fx.macro.create", params), grouped)
    case.assertEqual(fx._graph(case, document), graph)
    _assert_retired(case, document, [blur, glow, grouped["node"]["fx_id"]])
    case.assertEqual(
        case.success("scene.summary", {"document_id": document})["revision"], expanded["revision"]
    )


def _reject(
    case: NativeAcceptance,
    document: str,
    method: str,
    params: dict[str, Any],
    code: str,
) -> None:
    before = fx._graph(case, document)
    revision = case.revisions[document]
    error = case.failure(method, case.write_params(document, params))
    case.assertEqual(error["code"], code)
    case.assertEqual(fx._graph(case, document), before)
    case.assertEqual(case.success("scene.summary", {"document_id": document})["revision"], revision)


@acceptance_case
def macro_preflight_and_boundary_edits_preserve_scene(case: NativeAcceptance) -> None:
    """Reject invalid selections and cycles while allowing explicit edits to macro boundaries.

    Args:
        case: A fresh actual native-worker acceptance fixture.
    """
    document, artwork, blur, glow = _chain(case)
    stale = case.write_params(document, {"fx_ids": [blur, glow], "name": "Stale"})
    case.mutate("fx.rename", document, fx_id=blur, name="Renamed blur")
    case.assertEqual(case.failure("fx.macro.create", stale)["code"], "revision_conflict")
    other = case.create("Foreign owner")
    _reject(
        case,
        other,
        "fx.macro.create",
        {"fx_ids": [blur, glow], "name": "Foreign"},
        "invalid_handle",
    )
    disconnected = case.mutate("fx.create", document, type="STD_blurFx", name="Disconnected")[
        "fx_id"
    ]
    for selection in ([blur, blur], [blur, disconnected], [artwork, blur]):
        _reject(
            case,
            document,
            "fx.macro.create",
            {"fx_ids": selection, "name": "Invalid"},
            "INVALID_MACRO",
        )
    _reject(case, document, "fx.macro.expand", {"fx_id": glow}, "INVALID_MACRO")
    case.mutate("fx.connect", document, fx_id=disconnected, port="Source", source_fx_id=blur)
    _reject(
        case,
        document,
        "fx.macro.create",
        {"fx_ids": [blur, glow], "name": "Side exit"},
        "INVALID_MACRO",
    )
    case.mutate("fx.disconnect", document, fx_id=disconnected, port="Source")
    node = _macro(case, document, [blur, glow])["node"]
    macro_id = node["fx_id"]
    boundary = next(
        item for item in node["macro"]["input_mapping"] if item["source_fx_id"] == artwork
    )
    renamed = case.mutate("fx.rename", document, fx_id=macro_id, name="Renamed macro")
    case.assertEqual((renamed["name"], renamed["changed"]), ("Renamed macro", True))
    for method, options in (
        ("fx.enabled.set", {"enabled": False}),
        ("fx.duplicate", {}),
        ("fx.delete", {}),
        ("fx.parameter.set", {"parameter": "value", "value": 1}),
    ):
        _reject(case, document, method, {"fx_id": macro_id, **options}, "UNSUPPORTED_FX")
    aggregation = case.failure("fx.parameter.get", {"document_id": document, "fx_id": macro_id})
    case.assertEqual(aggregation["code"], "UNSUPPORTED_PARAMETER")
    _reject(
        case,
        document,
        "fx.connect",
        {"fx_id": macro_id, "port": boundary["port"], "source_fx_id": macro_id},
        "GRAPH_CYCLE",
    )
    before = fx._pixels(case, document, 6)
    case.mutate("fx.disconnect", document, fx_id=macro_id, port=boundary["port"])
    case.assertNotEqual(fx._pixels(case, document, 6), before)
    case.mutate("fx.connect", document, fx_id=macro_id, port=boundary["port"], source_fx_id=artwork)
    case.assertEqual(fx._pixels(case, document, 6), before)
    case.mutate("fx.terminal.set", document, fx_id=macro_id, enabled=False)
    case.assertFalse(fx._node(case, document, macro_id)["terminal"])
    case.mutate("fx.terminal.set", document, fx_id=macro_id, enabled=True)
    case.assertTrue(fx._node(case, document, macro_id)["terminal"])
