import {
  ProtocolError,
  ProtocolErrorCode,
  Server,
  fromJsonSchema,
} from "@modelcontextprotocol/server";
import type { CallToolResult, JsonSchemaType, Tool } from "@modelcontextprotocol/server";
import {
  ADAPTER_ERRORS,
  EFFECTS_SCHEMA,
  ERRORS_SCHEMA,
  conformingResult,
  successValidator,
} from "#mcp/declarations";
import type { Validator } from "#mcp/declarations";
import { WorkerError, errorResult, isObject } from "#mcp/errors";
import { boundedResult, toolResult } from "#mcp/results";
import { JobLedger } from "#mcp/jobs-ledger";
import type { WorkerLoss } from "#mcp/jobs-ledger";
import { LIVE_TOOL_NAMES } from "#mcp/live-protocol";
import type { LiveHello } from "#mcp/live-protocol";
import { ToolsetState, parseToolsets, toolsetDeclaration } from "#mcp/toolsets";

export interface NativeApi {
  request: (method: string, params?: Record<string, unknown>) => Promise<unknown>;
  close: () => void;
  /** How the worker was lost, once it was. */
  loss?: () => WorkerLoss | undefined;
}

interface NativeTool extends Tool {
  description: string;
  annotations: NonNullable<Tool["annotations"]>;
  effects?: Record<string, unknown>;
  errors?: string[];
}

const objectSchema = {
  type: "object",
  required: ["type"],
  properties: { type: { const: "object" } },
};

function descriptorSchema(declared: boolean): Parameters<typeof fromJsonSchema>[0] {
  const required = ["name", "description", "inputSchema", "annotations"];
  return {
    type: "object" as const,
    required: declared ? [...required, "outputSchema", "effects", "errors"] : required,
    properties: {
      name: { type: "string", pattern: "^[a-zA-Z0-9_.-]{1,128}$" },
      description: { type: "string", maxLength: 8192 },
      inputSchema: objectSchema,
      outputSchema: objectSchema,
      annotations: {
        type: "object",
        properties: {
          readOnlyHint: { type: "boolean" },
          destructiveHint: { type: "boolean" },
          idempotentHint: { type: "boolean" },
          openWorldHint: { type: "boolean" },
        },
      },
      effects: EFFECTS_SCHEMA,
      errors: ERRORS_SCHEMA,
    },
  };
}

const MAX_CATALOG_TOOLS = 1024;
const CATALOG_PAGE = 64;

async function discover(
  worker: NativeApi,
  requested: string[],
  domains: string[],
): Promise<ToolsetState> {
  const first = await worker.request("automation.capabilities", { limit: CATALOG_PAGE });
  if (!isObject(first) || first["protocol_version"] !== 3) {
    throw new WorkerError("INCOMPATIBLE_WORKER", "Expected native automation protocol version 3");
  }
  const entries = await catalogPages(worker, first);
  const tools = await catalog(entries, true);
  return new ToolsetState(parseToolsets(first["toolsets"], tools), tools, requested, domains);
}

/** Read every page of the native tool descriptors, which come a page at a time. */
async function catalogPages(worker: NativeApi, first: Record<string, unknown>): Promise<unknown[]> {
  const entries: unknown[] = [];
  let page: Record<string, unknown> = first;
  for (;;) {
    const tools = page["tools"];
    if (!Array.isArray(tools)) throw new WorkerError("INVALID_CATALOG", "Expected a tools page");
    entries.push(...tools);
    const next = page["next_offset"];
    if (next === null) return entries;
    if (typeof next !== "number" || entries.length > MAX_CATALOG_TOOLS) {
      throw new WorkerError("INVALID_CATALOG", "Invalid native tool catalog paging");
    }
    const result = await worker.request("automation.capabilities", {
      limit: CATALOG_PAGE,
      offset: next,
    });
    if (!isObject(result)) throw new WorkerError("INVALID_CATALOG", "Expected a tools page");
    page = result;
  }
}

const ROOT_KEYWORDS = ["oneOf", "anyOf", "allOf", "not", "$ref"];

/** Several clients and model APIs accept only a plain object at the root of an input schema. */
function requirePlainRoot(tool: NativeTool): void {
  const schema: Record<string, unknown> = tool.inputSchema;
  const keyword = ROOT_KEYWORDS.find((name) => name in schema);
  if (schema["type"] !== "object" || keyword !== undefined) {
    throw new WorkerError(
      "INVALID_CATALOG",
      `The input schema of ${tool.name} must be a plain object at its root` +
        (keyword === undefined ? "" : `, without ${keyword}`),
    );
  }
}

function forwarded(tool: NativeTool): Tool {
  const entry: Tool = {
    name: tool.name,
    description: tool.description,
    inputSchema: tool.inputSchema,
    annotations: tool.annotations,
  };
  if (tool.outputSchema !== undefined) entry.outputSchema = tool.outputSchema;
  return entry;
}

async function catalog(entries: unknown, declared: boolean): Promise<Tool[]> {
  if (!Array.isArray(entries) || entries.length > MAX_CATALOG_TOOLS) {
    throw new WorkerError(
      "INVALID_CATALOG",
      `Expected at most ${MAX_CATALOG_TOOLS} native tool descriptors`,
    );
  }
  const schema = fromJsonSchema<NativeTool>(descriptorSchema(declared));
  const tools: Tool[] = [];
  const names = new Set<string>();
  for (const entry of entries) {
    const parsed = await schema["~standard"].validate(entry);
    if (parsed.issues) throw new WorkerError("INVALID_CATALOG", "Invalid native tool descriptor");
    const tool = parsed.value;
    requirePlainRoot(tool);
    if (names.has(tool.name)) {
      throw new WorkerError("INVALID_CATALOG", "Duplicate native tool name");
    }
    names.add(tool.name);
    tools.push(forwarded(tool));
  }
  return tools;
}

/** Build an MCP server exposing the enabled toolsets of this native binary.
 *
 * Negotiation needs nothing from the worker: its catalog is read, and a deferred worker
 * launched, when a client first lists or calls tools.
 *
 * Args:
 *   worker: A private worker whose lifetime ends when the MCP connection closes.
 *   toolsets: Toolsets enabled at start in addition to core; "all" enables every toolset.
 *   unavailable: Told once when the catalog cannot be read, for the adapter's diagnostics.
 *   strict: Refuse results outside their output schema.
 *   domains: The toolsets the launch grants, or ["all"]; no other toolset can be enabled.
 * Returns:
 *   A new MCP server ready for one connection's protocol negotiation.
 */
export function createServer(
  worker: NativeApi,
  toolsets: string[] = ["core"],
  unavailable: (error: WorkerError) => void = () => {},
  strict = false,
  domains: string[] = ["all"],
): Server {
  let loading: Promise<Catalog> | undefined;
  const load = () =>
    (loading ??= discover(worker, toolsets, domains)
      .then((state) => compile(state.all(), state))
      .catch((error: unknown) => {
        const failure = catalogFailure(error);
        unavailable(failure);
        throw failure;
      }));
  return serverWithTools(worker, load, { live: false, strict });
}

/** Expose only the authenticated editor's exact six-tool live catalog.
 *
 * Args:
 *   connection: Native connector whose closure leaves the editor running.
 *   hello: Authenticated native hello containing the active permission mode.
 * Returns:
 *   An MCP server with no detached-worker tools or fallback.
 */
export async function createLiveServer(
  connection: NativeApi,
  hello: LiveHello,
  strict = false,
): Promise<Server> {
  const tools = await catalog(hello.tools, false);
  if (
    tools.length !== LIVE_TOOL_NAMES.size ||
    tools.some((tool) => !LIVE_TOOL_NAMES.has(tool.name))
  ) {
    throw new WorkerError("INVALID_CATALOG", "Expected the exact six-tool live editor catalog");
  }
  const compiled = compile(tools);
  return serverWithTools(connection, async () => compiled, { live: true, strict });
}

interface Compiled {
  tool: Tool;
  input: Validator;
  output: Validator | undefined;
}

interface Catalog {
  tools: Tool[];
  state: ToolsetState | undefined;
  validators: Map<string, Compiled>;
}

function compile(tools: Tool[], state?: ToolsetState): Catalog {
  const validators = new Map(
    tools.map((tool) => [
      tool.name,
      {
        tool,
        // SDK Tool permits optional undefined fields; native JSON cannot contain them.
        // fromJsonSchema compiles and validates the unchanged schema before any call.
        input: fromJsonSchema<Record<string, unknown>>(tool.inputSchema as JsonSchemaType),
        output: successValidator(tool),
      },
    ]),
  );
  return { tools, state, validators };
}

function catalogFailure(error: unknown): WorkerError {
  if (error instanceof WorkerError) return error;
  const reason = error instanceof Error ? error.message : String(error);
  return new WorkerError("INVALID_CATALOG", `Invalid native tool catalog: ${reason}`);
}

async function listedCatalog(load: () => Promise<Catalog>): Promise<Catalog> {
  try {
    return await load();
  } catch (error) {
    const failure = catalogFailure(error);
    throw new ProtocolError(
      ProtocolErrorCode.InternalError,
      `${failure.code}: ${failure.message}`,
      {
        code: failure.code,
        details: failure.details,
      },
    );
  }
}

async function toolsetCall(
  state: ToolsetState,
  server: Server,
  name: string,
  args: Record<string, unknown>,
): Promise<Record<string, unknown>> {
  if (name === "toolsets.list") return state.list();
  const names = args["names"] as string[];
  const changed = name === "toolsets.enable" ? state.enable(names) : state.disable(names);
  if (changed) await server.sendToolListChanged();
  return { ...state.list(), changed };
}

interface Mode {
  live: boolean;
  strict: boolean;
}

/** A listed tool: no output schema, which the adapter keeps for its own result checks. */
function listed(tool: Tool): Tool {
  const { outputSchema: _kept, ...entry } = tool;
  return entry;
}

async function describedTools(
  state: ToolsetState,
  worker: NativeApi,
  names: string[],
): Promise<Record<string, unknown>> {
  const native = names.filter((name) => !state.isToolsetTool(name));
  const reply = native.length > 0 ? await worker.request("tool.describe", { names: native }) : {};
  const remote = isObject(reply) && Array.isArray(reply["tools"]) ? reply["tools"] : [];
  const described = new Map<unknown, unknown>();
  for (const tool of remote) {
    if (!isObject(tool) || !Array.isArray(tool["errors"])) continue;
    const errors = tool["errors"] as unknown[];
    const added = ADAPTER_ERRORS.filter((code) => !errors.includes(code));
    described.set(tool["name"], { ...tool, errors: [...errors, ...added] });
  }
  return {
    tools: names.map((name) =>
      state.isToolsetTool(name) ? toolsetDeclaration(name) : described.get(name),
    ),
  };
}

function serverWithTools(worker: NativeApi, load: () => Promise<Catalog>, mode: Mode): Server {
  const { live, strict } = mode;
  const server = new Server(
    { name: "groundzero", version: "0.1.0" },
    { capabilities: { tools: live ? {} : { listChanged: true } } },
  );
  server.onclose = () => worker.close();
  const ledger = new JobLedger();
  server.setRequestHandler("tools/list", async () => {
    const { tools, state } = await listedCatalog(load);
    return { tools: (state ? state.visible() : tools).map(listed) };
  });
  server.setRequestHandler("tools/call", async (request, context) => {
    let result: CallToolResult;
    try {
      boundedResult({ content: [] }, context.mcpReq.id);
      const { state, validators } = await load();
      const name = request.params.name;
      const compiled = validators.get(name);
      if (!compiled) throw new WorkerError("UNKNOWN_TOOL", `Unknown tool: ${name}`);
      state?.requireEnabled(name);
      const args = await validArguments(compiled.input, request.params.arguments ?? {});
      const call = { state, worker, ledger, name, args, signal: context.mcpReq.signal };
      result = await conformingResult(
        { tool: compiled.tool, validator: compiled.output, args, strict },
        state?.isToolsetTool(name) === true
          ? toolResult(await toolsetCall(state, server, name, args))
          : nativeToolResult(await nativeValue(call), live),
      );
    } catch (error) {
      result = errorResult(error);
    }
    try {
      return boundedResult(result, context.mcpReq.id);
    } catch (error) {
      await server.close();
      throw error;
    }
  });
  return server;
}

interface NativeCall {
  state: ToolsetState | undefined;
  worker: NativeApi;
  ledger: JobLedger;
  name: string;
  args: Record<string, unknown>;
  signal: AbortSignal;
}

async function nativeValue(call: NativeCall): Promise<unknown> {
  const { state, worker, ledger, name, args, signal } = call;
  if (name === "tool.describe" && state !== undefined) {
    return describedTools(state, worker, args["names"] as string[]);
  }
  return sessionResult(state, name, await ledgerRequest(worker, ledger, name, args, signal));
}

function sessionResult(state: ToolsetState | undefined, name: string, value: unknown): unknown {
  return state !== undefined && name === "runtime.inspect" ? state.sessionRuntime(value) : value;
}

// A native call whose job records go to the session ledger. A cancelled MCP request is
// abandoned, never cancelled natively: a job it started is recorded as such. After the
// worker is lost, job.status and job.list answer from the ledger.
async function ledgerRequest(
  worker: NativeApi,
  ledger: JobLedger,
  name: string,
  args: Record<string, unknown>,
  signal: AbortSignal,
): Promise<unknown> {
  try {
    const value = await worker.request(name, args);
    ledger.observe(value, signal.aborted);
    return name === "job.list" ? ledger.annotate(value) : value;
  } catch (error) {
    const answer = ledgerAnswer(worker, ledger, name, args, error);
    if (answer === undefined) throw error;
    return answer;
  }
}

function ledgerAnswer(
  worker: NativeApi,
  ledger: JobLedger,
  name: string,
  args: Record<string, unknown>,
  error: unknown,
): unknown {
  const loss = worker.loss?.();
  if (!(error instanceof WorkerError) || error.code !== "WORKER_UNAVAILABLE" || !loss) {
    return undefined;
  }
  if (name === "job.list") return ledger.list(args, loss);
  return name === "job.status" ? ledger.status(String(args["job_id"]), loss) : undefined;
}

async function validArguments(
  validator: Validator,
  args: Record<string, unknown>,
): Promise<Record<string, unknown>> {
  const parsed = await validator["~standard"].validate(args);
  if (parsed.issues) {
    throw new WorkerError("INVALID_ARGUMENTS", "Tool arguments do not match its input schema", {
      issues: parsed.issues.map((issue) => issue.message),
    });
  }
  return parsed.value;
}

function nativeToolResult(value: unknown, live: boolean): CallToolResult {
  if (live && isObject(value) && "image" in value) {
    throw new WorkerError("INVALID_RESULT", "Live inspection does not return image artifacts");
  }
  return toolResult(value);
}
