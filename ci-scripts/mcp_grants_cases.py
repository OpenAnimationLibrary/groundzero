"""Hold every tool to the grants its effect declaration derives, under every grant set.

A worker starts with fixed grant classes (`--grants`) and toolsets (`--domains`). The cases
read the live catalog, so a new tool is covered by its declaration alone: every tool whose
derived grants a session lacks is refused with `grant_denied` before anything is recorded,
and with exactly its grants it passes the check.
"""

from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_readonly_calls import sample
from mcp_readonly_tree import snapshot
from mcp_scene_access_cases import corpus, open_scene
from mcp_worker import Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

CLASSES = ("draft", "file", "job")
DENIED = "grant-denied"
# Schema patterns the generic sampler cannot satisfy with a one-character string.
SAMPLES = {"studio_palette.inspect": {"path": "x.tpl"}}


def _catalog(case: NativeAcceptance) -> list[dict[str, Any]]:
    return case.success("automation.capabilities", {})["tools"]


def _arguments(tool: dict[str, Any], document: str, revision: str, operation: str) -> dict:
    schema = tool["inputSchema"]
    fixed = SAMPLES.get(tool["name"], {})
    required = [name for name in schema.get("required", []) if name not in fixed]
    arguments = {**sample({**schema, "required": required}), **fixed}
    targets = {"document_id": document, "expected_revision": revision, "operation_id": operation}
    for name, value in targets.items():
        if name in schema["properties"]:
            arguments[name] = value
    return arguments


def _missing(tool: dict[str, Any], granted: set[str]) -> list[str]:
    """The classes the tool lacks; discarding a read-only document needs no draft."""
    effects = tool["effects"]
    needed = [grant for grant in effects["grants"] if grant not in granted]
    if effects["document"] == "discards":
        needed = [grant for grant in needed if grant != "draft"]
    return needed


def _revision(worker: Worker, document: str) -> str:
    listed = worker.call("automation.list_documents", {})["result"]["documents"]
    return next(row["revision"] for row in listed if row["document_id"] == document)


def _session(case: NativeAcceptance, grants: tuple[str, ...]) -> tuple[Worker, Path]:
    label = "-".join(grants) or "inspect"
    workspace = case.directory / f"grants-{label}"
    workspace.mkdir()
    worker = Worker(
        case.executable,
        workspace,
        case.inputs,
        case.directory / f"grants-{label}.stderr.log",
        grants=",".join(("inspect", *grants)),
    )
    case.addCleanup(worker.close)
    return worker, workspace


def _require_denied(case: NativeAcceptance, response: dict, tool: dict, missing: list) -> None:
    case.assertIn("error", response, f"{tool['name']} ran without {missing}: {response}")
    error = response["error"]
    case.assertEqual(error["code"], "grant_denied", error)
    case.assertEqual(error["details"]["tool"], tool["name"], error)
    case.assertEqual(error["details"]["missing"], missing, error)
    case.assertEqual(error["details"]["domain"], tool["toolset"], error)
    case.assertTrue(all(grant in error["message"] for grant in missing), error)


def _require_admitted(case: NativeAcceptance, worker: Worker, tool: dict, params: dict) -> None:
    """Require a call to pass the grant check; its other outcomes are other cases' concern."""
    payload = json.dumps({"id": "admitted", "method": tool["name"], "params": params}).encode()
    response = worker.exchange(payload, timeout=60)
    case.assertNotEqual(response.get("error", {}).get("code"), "grant_denied", response)


def _grant_set(case: NativeAcceptance, catalog: list[dict], scene: Path, grants: tuple) -> None:
    granted = {"inspect", *grants}
    worker, workspace = _session(case, grants)
    document = open_scene(case, worker, scene, "standalone")["document_id"]
    witness = open_scene(case, worker, scene, "standalone")["document_id"]
    revision = _revision(worker, document)
    before = snapshot({"workspace": workspace, "inputs": case.inputs})
    documents = worker.call("automation.list_documents", {})["result"]
    denied = [tool for tool in catalog if _missing(tool, granted)]
    for tool in denied:
        with case.subTest(grants=sorted(granted), denied=tool["name"]):
            params = _arguments(tool, document, revision, DENIED)
            response = worker.call(tool["name"], params)
            _require_denied(case, response, tool, _missing(tool, granted))
            case.assertEqual(response["error"]["details"]["granted"], sorted(granted))
    case.assertEqual(snapshot({"workspace": workspace, "inputs": case.inputs}), before)
    case.assertEqual(worker.call("automation.list_documents", {})["result"], documents)
    for index, tool in enumerate(catalog):
        if tool in denied or tool["effects"]["document"] == "discards":
            continue
        with case.subTest(grants=sorted(granted), admitted=tool["name"]):
            params = _arguments(tool, document, revision, f"admitted-{index}")
            _require_admitted(case, worker, tool, params)
    discard = {"document_id": witness, "operation_id": DENIED}
    discard["expected_revision"] = _revision(worker, witness)
    case.assertIn("result", worker.call("scene.discard", discard), "A denial kept a receipt")


def _grant_sets(case: NativeAcceptance, sets: list[tuple[str, ...]]) -> None:
    scene = corpus(case)["B"]
    catalog = _catalog(case)
    for grants in sets:
        _grant_set(case, catalog, scene, grants)


def inspect_only_session(case: NativeAcceptance) -> None:
    """Refuse every drafting, file and job tool and admit reads, read-only opens and job reads."""
    _grant_sets(case, [()])


def draft_grant_gates_documents(case: NativeAcceptance) -> None:
    """Refuse document changes without draft, and admit them with it."""
    _grant_sets(case, [("draft",), ("file", "job")])


def file_grant_gates_caller_paths(case: NativeAcceptance) -> None:
    """Refuse writes to caller-named paths without file, and admit them with it."""
    _grant_sets(case, [("file",), ("draft", "job")])


def job_grant_gates_jobs(case: NativeAcceptance) -> None:
    """Refuse starting and cancelling jobs without job, and admit them with it."""
    _grant_sets(case, [("job",), ("draft", "file")])


def full_grants_admit_every_tool(case: NativeAcceptance) -> None:
    """Admit every tool when the session holds every grant class."""
    _grant_sets(case, [CLASSES])


def _toolsets(case: NativeAcceptance, worker: Worker) -> dict[str, dict[str, Any]]:
    listed = worker.call("toolsets.list", {})["result"]["toolsets"]
    return {row["name"]: row for row in listed}


def _adapter_domains(case: NativeAcceptance, worker: Worker) -> None:
    rows = _toolsets(case, worker)
    case.assertTrue(rows["audio"]["granted"] and rows["audio"]["enabled"], rows)
    case.assertFalse(rows["rigging"]["granted"] or rows["rigging"]["enabled"], rows)
    refused = worker.call("toolsets.enable", {"names": ["rigging"]})
    case.assertEqual(refused["error"]["code"], "GRANT_REQUIRED", refused)
    case.assertEqual(refused["error"]["details"]["toolsets"], ["rigging"], refused)
    case.assertEqual(_toolsets(case, worker), rows)


def domain_grant_gates_toolsets(case: NativeAcceptance) -> None:
    """Refuse tools of an ungranted toolset in the worker; the adapter cannot enable them."""
    catalog = _catalog(case)
    workspace = case.directory / "grants-domains"
    workspace.mkdir()
    worker = Worker(
        case.executable,
        workspace,
        case.inputs,
        case.directory / "domains.stderr.log",
        domains="audio",
    )
    case.addCleanup(worker.close)
    if case.through_sdk:
        _adapter_domains(case, worker)
    for index, tool in enumerate(catalog):
        if tool["toolset"] == "core" or (case.through_sdk and tool["toolset"] != "audio"):
            continue
        with case.subTest(tool=tool["name"]):
            params = _arguments(tool, "doc_missing", "0", f"domain-{index}")
            if tool["toolset"] == "audio":
                _require_admitted(case, worker, tool, params)
                continue
            response = worker.call(tool["name"], params)
            _require_denied(case, response, tool, [])
            case.assertEqual(response["error"]["details"]["granted_domains"], ["audio", "core"])


def _canonical(report: dict[str, Any]) -> str:
    fields = {key: value for key, value in report.items() if key != "digest"}
    text = json.dumps(fields, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(text.encode()).hexdigest()


def authority_reports_the_launch(case: NativeAcceptance) -> None:
    """Report the launch authority and a digest that changes when the grants change."""
    full = case.success("automation.capabilities", {})["authority"]
    case.assertEqual(full["grants"], ["draft", "file", "inspect", "job"])
    case.assertIn("core", full["domains"])
    case.assertEqual(full["workspace"], str(case.workspace.resolve()))
    case.assertEqual(full["input_roots"], [str(case.inputs.resolve())])
    case.assertEqual(full["digest"], _canonical(full))
    worker, _ = _session(case, ("job",))
    narrowed = worker.call("automation.capabilities", {})["result"]["authority"]
    case.assertEqual(
        (narrowed["grants"], narrowed["domains"]), (["inspect", "job"], full["domains"])
    )
    case.assertEqual(narrowed["digest"], _canonical(narrowed))
    case.assertNotEqual(narrowed["digest"], full["digest"])


def _launch(case: NativeAcceptance, options: list[str]) -> tuple[int, str]:
    workspace = ["--workspace", str(case.workspace), "--input-root", str(case.inputs)]
    result = subprocess.run(
        [str(case.executable), "--automation-worker", *workspace, *options],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=60,
        check=False,
    )
    return result.returncode, result.stderr.decode("utf-8", errors="replace")


REFUSED_LAUNCHES = {
    "unknown class": (["--grants", "inspect,live", "--domains", "all"], "live"),
    "no inspect": (["--grants", "draft", "--domains", "all"], "inspect"),
    "repeated class": (["--grants", "inspect,inspect", "--domains", "all"], "--grants"),
    "two grant lists": (
        ["--grants", "inspect", "--grants", "inspect", "--domains", "all"],
        "--grants",
    ),
    "all with names": (["--grants", "inspect", "--domains", "all,core"], "--domains"),
    "two domain lists": (
        ["--grants", "inspect", "--domains", "core", "--domains", "core"],
        "--domains",
    ),
    "unknown toolset": (["--grants", "inspect", "--domains", "sound"], "sound"),
}
WORKER_ONLY_LAUNCHES = {
    "no grants": (["--domains", "all"], "--grants"),
    "no domains": (["--grants", "inspect"], "--domains"),
}


def grant_options_are_explicit_and_valid(case: NativeAcceptance) -> None:
    """Refuse unknown, repeated, contradictory and, for the worker, missing grant options.

    The adapter defaults to today's full authority, so only the raw worker refuses a launch
    without them.
    """
    launches = dict(REFUSED_LAUNCHES)
    if not case.through_sdk:
        launches.update(WORKER_ONLY_LAUNCHES)
    for label, (options, mention) in launches.items():
        with case.subTest(launch=label):
            code, diagnostics = _launch(case, options)
            case.assertEqual(code, 1 if case.through_sdk else 2, (label, diagnostics))
            case.assertIn(mention, diagnostics, label)
            if not case.through_sdk:
                case.assertIn("automation-worker: invalid_argument:", diagnostics)
