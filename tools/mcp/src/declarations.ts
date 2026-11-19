import { isDeepStrictEqual } from "node:util";
import { fromJsonSchema } from "@modelcontextprotocol/server";
import type { CallToolResult, JsonSchemaType, Tool } from "@modelcontextprotocol/server";
import { WorkerError, errorResult } from "#mcp/errors";

/** Error codes the adapter itself can return for any native tool. */
export const ADAPTER_ERRORS = [
  "INVALID_ARGUMENTS",
  "UNKNOWN_TOOL",
  "TOOLSET_DISABLED",
  "WORKER_UNAVAILABLE",
  "INVALID_RESULT",
  "RESULT_TOO_LARGE",
  "BUSY",
  "LIMIT_EXCEEDED",
];

const MAX_ISSUES = 16;

const string = (minLength: number, maxLength: number) => ({
  type: "string",
  minLength,
  maxLength,
});
const choice = (values: string[]) => ({ type: "string", enum: values });
const list = (items: object, minItems: number, maxItems: number) => ({
  type: "array",
  items,
  minItems,
  maxItems,
});

/** The structured content of every error result: the second branch of each output schema. */
export const ERROR_ENVELOPE = {
  type: "object",
  additionalProperties: false,
  required: ["error"],
  properties: {
    error: {
      type: "object",
      additionalProperties: false,
      required: ["code", "message", "details"],
      properties: {
        code: string(1, 64),
        message: string(0, 2048),
        details: { type: "object" },
      },
    },
  },
};

const EFFECT_FIELDS = [
  "class",
  "document",
  "files",
  "job",
  "processes",
  "overwrites",
  "undo",
  "no_op",
  "after_change_failure",
  "rollback",
  "retry",
  "grants",
];

/** What a tool does to documents, files, jobs and processes, as the worker declares it. */
export const EFFECTS_SCHEMA = {
  type: "object",
  additionalProperties: false,
  required: EFFECT_FIELDS,
  properties: {
    class: choice([
      "read",
      "document_write",
      "document_lifecycle",
      "job_start",
      "job_control",
      "session",
    ]),
    document: choice(["none", "reads", "writes", "creates", "opens_read_only", "discards"]),
    files: list(
      choice([
        "reads_roots",
        "reads_shared_roots",
        "reads_application",
        "reads_private",
        "writes_private",
        "writes_caller_path",
        "writes_shared_roots",
        "reads_output_roots",
        "writes_output_roots",
      ]),
      0,
      9,
    ),
    job: choice(["none", "starts", "controls", "reads"]),
    processes: list(choice(["parse_helper", "media_tools", "neural_helper", "scene_host"]), 0, 4),
    overwrites: { type: "boolean" },
    undo: choice(["none", "one_native_undo_entry"]),
    no_op: choice(["revision_kept", "not_applicable"]),
    after_change_failure: choice(["partially_applied", "not_applicable"]),
    rollback: { type: "string", const: "never_claimed" },
    retry: choice(["receipt_replay", "repeat_safe"]),
    grants: list(choice(["inspect", "draft", "file", "job", "live_inspect", "live_edit"]), 1, 4),
  },
};

/** The declared error codes of one tool. */
export const ERRORS_SCHEMA = list(string(1, 64), 1, 64);

/** An output schema: the success shape, or the error envelope every error result carries. */
export function outputSchema(success: object): NonNullable<Tool["outputSchema"]> {
  return { type: "object", oneOf: [success, ERROR_ENVELOPE] };
}

/** Effects of an adapter tool that changes only what this session exposes. */
export function sessionEffects(kind: "read" | "session"): Record<string, unknown> {
  return {
    class: kind,
    document: "none",
    files: [],
    job: "none",
    processes: [],
    overwrites: false,
    undo: "none",
    no_op: "not_applicable",
    after_change_failure: "not_applicable",
    rollback: "never_claimed",
    retry: "repeat_safe",
    grants: ["inspect"],
  };
}

export type Validator = ReturnType<typeof fromJsonSchema<Record<string, unknown>>>;

/** Compile the success branch of a tool's output schema.
 *
 * Args:
 *   tool: A descriptor whose optional outputSchema must be the success shape or the envelope.
 * Returns:
 *   The success validator, or undefined for a tool without an output schema.
 */
export function successValidator(tool: Tool): Validator | undefined {
  if (tool.outputSchema === undefined) return undefined;
  const success = successBranch(tool.outputSchema);
  if (success === undefined) {
    throw new WorkerError(
      "INVALID_CATALOG",
      `The output schema of ${tool.name} must be one of an object success shape and the error envelope`,
    );
  }
  // fromJsonSchema compiles the unchanged schema before any call.
  return fromJsonSchema<Record<string, unknown>>(success);
}

function successBranch(schema: Record<string, unknown>): JsonSchemaType | undefined {
  const branches = schema["oneOf"];
  if (schema["type"] !== "object" || Object.keys(schema).length !== 2) return undefined;
  if (!Array.isArray(branches) || branches.length !== 2) return undefined;
  if (!isDeepStrictEqual(branches[1], ERROR_ENVELOPE)) return undefined;
  const success = branches[0] as Record<string, unknown> | undefined;
  return success?.["type"] === "object" ? (success as JsonSchemaType) : undefined;
}

/** How the adapter checks one successful result against the tool's success shape. */
export interface ResultCheck {
  tool: Tool;
  validator: Validator | undefined;
  args: Record<string, unknown>;
  /** Refuse a nonconforming result instead of reporting it; the test fixture sets this. */
  strict: boolean;
}

const ISSUE_PATH = /(?:^|, )data((?:\/[^ ,]*)?) /gu;

// The SDK validator reports one issue whose message lists each failure as
// "data/<json pointer> must ...". Keep the pointers only: the values never reach a log.
function issuePaths(issues: ReadonlyArray<{ readonly message: string }>): string[] {
  const paths = new Set<string>();
  for (const issue of issues) {
    for (const match of issue.message.matchAll(ISSUE_PATH)) {
      const pointer = match[1] ?? "";
      paths.add(pointer === "" ? "(result)" : pointer.slice(1).replaceAll("/", "."));
    }
  }
  return paths.size > 0 ? [...paths].slice(0, MAX_ISSUES) : ["(result)"];
}

/** Check a successful result against its tool's success shape.
 *
 * A strict session returns INVALID_RESULT instead of a nonconforming result. Otherwise the
 * result, whose work may already have happened, is returned unchanged with the
 * `groundzero/output_mismatch` flag in `_meta`, and one stderr line names the tool and the
 * failing paths, never the values.
 *
 * Args:
 *   check: The tool, its compiled success shape, the call arguments and the mode.
 *   result: The converted native result.
 * Returns:
 *   The result, flagged or refused when it does not conform.
 */
export async function conformingResult(
  check: ResultCheck,
  result: CallToolResult,
): Promise<CallToolResult> {
  const { tool, validator, args, strict } = check;
  if (validator === undefined || result.isError === true) return result;
  const parsed = await validator["~standard"].validate(result.structuredContent);
  if (!parsed.issues) return result;
  const paths = issuePaths(parsed.issues);
  if (!strict) {
    process.stderr.write(
      `groundzero-mcp: ${tool.name} result does not match its output schema at ${paths.join(", ")}\n`,
    );
    return { ...result, _meta: { ...result._meta, "groundzero/output_mismatch": true } };
  }
  const readOnly = tool.annotations?.readOnlyHint === true;
  return errorResult(
    new WorkerError("INVALID_RESULT", `${tool.name} returned a result outside its output schema`, {
      tool: tool.name,
      outcome: readOnly ? "none" : "applied",
      operation_id: args["operation_id"] ?? null,
      issues: paths,
    }),
  );
}
