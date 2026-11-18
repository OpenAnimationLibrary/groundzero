"""Acceptance cases for the output schemas, effect declarations and error lists of every tool."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from mcp_worker import SchemaViolation, check_schema, require_declared_error

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

DESCRIPTOR_FIELDS = {
    "name",
    "description",
    "toolset",
    "inputSchema",
    "outputSchema",
    "annotations",
    "effects",
    "errors",
}
EVERY_TOOL_ERRORS = {
    "invalid_params",
    "method_not_found",
    "worker_busy",
    "native_error",
    "response_too_large",
}
ERROR_ENVELOPE = {
    "type": "object",
    "additionalProperties": False,
    "required": ["error"],
    "properties": {
        "error": {
            "type": "object",
            "additionalProperties": False,
            "required": ["code", "message", "details"],
            "properties": {
                "code": {"type": "string", "minLength": 1, "maxLength": 64},
                "message": {"type": "string", "minLength": 0, "maxLength": 2048},
                "details": {"type": "object"},
            },
        }
    },
}


def derived_grants(effects: dict[str, Any]) -> list[str]:
    """Return the grants the effect fields imply, in the order the worker publishes them."""
    grants = ["inspect"]
    if effects["document"] in {"writes", "creates", "discards"}:
        grants.append("draft")
    if {"writes_caller_path", "writes_shared_roots"} & set(effects["files"]):
        grants.append("file")
    if effects["job"] in {"starts", "controls"}:
        grants.append("job")
    return grants


def _check_effects(case: NativeAcceptance, tool: dict[str, Any]) -> None:
    effects = tool["effects"]
    read = effects["class"] == "read"
    writes = effects["class"] not in {"read", "session"}
    case.assertEqual(
        tool["annotations"],
        {
            "readOnlyHint": read,
            "destructiveHint": effects["overwrites"] and not read,
            "idempotentHint": True,
            "openWorldHint": False,
        },
    )
    case.assertEqual(effects["grants"], derived_grants(effects))
    case.assertEqual(effects["rollback"], "never_claimed")
    case.assertEqual(effects["retry"], "receipt_replay" if writes else "repeat_safe")
    case.assertEqual(
        effects["after_change_failure"], "partially_applied" if writes else "not_applicable"
    )
    if "writes_caller_path" in effects["files"]:
        case.assertFalse(effects["overwrites"], "Caller paths are never overwritten")


def _schema_keys(schema: Any) -> set[str]:
    if isinstance(schema, list):
        return set().union(*(_schema_keys(item) for item in schema))
    if not isinstance(schema, dict):
        return set()
    nested = schema.get("properties", {})
    keys = set(schema).union(*(_schema_keys(item) for item in nested.values()))
    return keys.union(*(_schema_keys(schema[key]) for key in ("items", "oneOf") if key in schema))


def _check_output_schema(case: NativeAcceptance, tool: dict[str, Any]) -> None:
    case.assertIn("outputSchema", tool, f"{tool['name']} publishes no outputSchema")
    schema = tool["outputSchema"]
    case.assertEqual(set(schema), {"type", "oneOf"})
    case.assertEqual(schema["type"], "object")
    success, envelope = schema["oneOf"]
    case.assertEqual(success["type"], "object")
    case.assertEqual(envelope, ERROR_ENVELOPE)
    case.assertNotIn("description", _schema_keys(success), "Output schemas carry no descriptions")


def descriptors_publish_declarations(case: NativeAcceptance, tools: list[dict[str, Any]]) -> None:
    """Require effects, errors and a well-formed outputSchema on every descriptor."""
    for tool in tools:
        with case.subTest(declared=tool["name"]):
            case.assertLessEqual(set(tool), DESCRIPTOR_FIELDS)
            case.assertTrue(EVERY_TOOL_ERRORS.issubset(tool["errors"]), tool["errors"])
            case.assertLessEqual(len(tool["errors"]), 64)
            case.assertEqual(len(tool["errors"]), len(set(tool["errors"])))
            _check_effects(case, tool)
            _check_output_schema(case, tool)


def _tools(case: NativeAcceptance) -> dict[str, dict[str, Any]]:
    capabilities = case.success("automation.capabilities", {})
    return {tool["name"]: tool for tool in capabilities["tools"]}


def errors_are_declared(case: NativeAcceptance) -> None:
    """Prove the suite-wide check: observed codes are declared and an undeclared one fails."""
    tools = _tools(case)
    error = case.failure("scene.summary", {"document_id": "doc_missing"})
    case.assertEqual(error["code"], "document_not_found")
    case.assertIn(error["code"], tools["scene.summary"]["errors"])
    require_declared_error(tools["scene.summary"], "document_not_found")
    with case.assertRaisesRegex(AssertionError, "undeclared error code 'not_a_code'"):
        require_declared_error(tools["scene.summary"], "not_a_code")
    with case.assertRaises(SchemaViolation):
        check_schema(
            {"documents": [], "stray": 1}, tools["automation.list_documents"]["outputSchema"]
        )
    with case.assertRaises(SchemaViolation):
        check_schema({"error": {"code": "", "message": "", "details": {}}}, ERROR_ENVELOPE)
    check_schema(
        case.success("automation.list_documents", {}),
        tools["automation.list_documents"]["outputSchema"],
    )


def _existing_target_is_refused(case: NativeAcceptance, tool: dict[str, Any]) -> None:
    document = case.create("Declared")
    directory = str(case.workspace / "declared-copy")
    case.mutate("scene.save_copy", document, directory=directory)
    params = case.write_params(document, {"directory": directory})
    error = case.failure("scene.save_copy", params)
    case.assertEqual(error["code"], "already_exists", error)
    case.assertIn(error["code"], tool["errors"])
    case.assertFalse(tool["effects"]["overwrites"])


def _no_op_keeps_revision(case: NativeAcceptance, tool: dict[str, Any]) -> None:
    case.assertEqual(tool["effects"]["no_op"], "revision_kept")
    document = case.create("No-op")
    first = case.mutate("scene.settings", document, width_px=64)
    noop = case.mutate("scene.settings", document, width_px=64)
    case.assertEqual(noop["revision"], first["revision"])


def _job_start_creates_one_job(case: NativeAcceptance, tool: dict[str, Any]) -> None:
    case.assertEqual((tool["effects"]["class"], tool["effects"]["job"]), ("job_start", "starts"))
    document = case.create("Job")
    params = case.write_params(document, {"frame": 0, "width": 32, "height": 32})
    job = case.success("render.preview", params)
    case.assertEqual(case.success("render.preview", params)["job_id"], job["job_id"])
    case.assertEqual(case.wait_job(job["job_id"])["status"], "completed")
    case.assertEqual(
        case.success("scene.summary", {"document_id": document})["revision"],
        params["expected_revision"],
    )


def effect_declarations_match_behavior(case: NativeAcceptance) -> None:
    """Hold representative tools to their declared no-op, overwrite and job effects."""
    tools = _tools(case)
    _no_op_keeps_revision(case, tools["scene.settings"])
    _existing_target_is_refused(case, tools["scene.save_copy"])
    _job_start_creates_one_job(case, tools["render.preview"])
