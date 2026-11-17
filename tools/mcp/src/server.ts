import { Server, fromJsonSchema } from "@modelcontextprotocol/server";
import type { CallToolResult, JsonSchemaType, Tool } from "@modelcontextprotocol/server";
import { WorkerError, errorResult, isObject } from "#mcp/errors";
import { boundedResult, toolResult } from "#mcp/results";
import { LIVE_TOOL_NAMES } from "#mcp/live-protocol";
import type { LiveHello } from "#mcp/live-protocol";

export interface NativeApi {
  request: (method: string, params?: Record<string, unknown>) => Promise<unknown>;
  close: () => void;
}

interface NativeTool extends Tool {
  description: string;
  annotations: NonNullable<Tool["annotations"]>;
}

const descriptorSchema: Parameters<typeof fromJsonSchema>[0] = {
  type: "object" as const,
  required: ["name", "description", "inputSchema", "annotations"],
  properties: {
    name: { type: "string", pattern: "^[a-zA-Z0-9_.-]{1,128}$" },
    description: { type: "string", maxLength: 8192 },
    inputSchema: {
      type: "object",
      required: ["type"],
      properties: { type: { const: "object" } },
    },
    annotations: {
      type: "object",
      properties: {
        readOnlyHint: { type: "boolean" },
        destructiveHint: { type: "boolean" },
        idempotentHint: { type: "boolean" },
        openWorldHint: { type: "boolean" },
      },
    },
  },
};

async function discover(worker: NativeApi): Promise<Tool[]> {
  const result = await worker.request("automation.capabilities");
  if (!isObject(result) || result["protocol_version"] !== 1) {
    throw new WorkerError("INCOMPATIBLE_WORKER", "Expected native automation protocol version 1");
  }
  return catalog(result["tools"]);
}

async function catalog(entries: unknown): Promise<Tool[]> {
  if (!Array.isArray(entries) || entries.length > 256) {
    throw new WorkerError("INVALID_CATALOG", "Expected at most 256 native tool descriptors");
  }
  const schema = fromJsonSchema<NativeTool>(descriptorSchema);
  const tools: Tool[] = [];
  const names = new Set<string>();
  for (const entry of entries) {
    const parsed = await schema["~standard"].validate(entry);
    if (parsed.issues) throw new WorkerError("INVALID_CATALOG", "Invalid native tool descriptor");
    const tool = parsed.value;
    if (names.has(tool.name)) {
      throw new WorkerError("INVALID_CATALOG", "Duplicate native tool name");
    }
    names.add(tool.name);
    tools.push({
      name: tool.name,
      description: tool.description,
      inputSchema: tool.inputSchema,
      annotations: tool.annotations,
    });
  }
  return tools;
}

/** Build an MCP server exposing the validated catalog of this native binary.
 *
 * Args:
 *   worker: A private worker whose lifetime ends when the MCP connection closes.
 * Returns:
 *   A new MCP server ready for one connection's protocol negotiation.
 */
export async function createServer(worker: NativeApi): Promise<Server> {
  const tools = await discover(worker);
  return serverWithTools(worker, tools, false);
}

/** Expose only the authenticated editor's exact six-tool live catalog.
 *
 * Args:
 *   connection: Native connector whose closure leaves the editor running.
 *   hello: Authenticated native hello containing the active permission mode.
 * Returns:
 *   An MCP server with no detached-worker tools or fallback.
 */
export async function createLiveServer(connection: NativeApi, hello: LiveHello): Promise<Server> {
  const tools = await catalog(hello.tools);
  if (
    tools.length !== LIVE_TOOL_NAMES.size ||
    tools.some((tool) => !LIVE_TOOL_NAMES.has(tool.name))
  ) {
    throw new WorkerError("INVALID_CATALOG", "Expected the exact six-tool live editor catalog");
  }
  return serverWithTools(connection, tools, true);
}

function serverWithTools(worker: NativeApi, tools: Tool[], live: boolean): Server {
  const validators = new Map(
    tools.map((tool) => [
      tool.name,
      // SDK Tool permits optional undefined fields; native JSON cannot contain them.
      // fromJsonSchema compiles and validates the unchanged schema before any call.
      fromJsonSchema<Record<string, unknown>>(tool.inputSchema as JsonSchemaType),
    ]),
  );
  const server = new Server(
    { name: "groundzero", version: "0.1.0" },
    { capabilities: { tools: {} } },
  );
  server.onclose = () => worker.close();
  server.setRequestHandler("tools/list", async () => ({ tools }));
  server.setRequestHandler("tools/call", async (request, context) => {
    let result: CallToolResult;
    try {
      boundedResult({ content: [] }, context.mcpReq.id);
      const validator = validators.get(request.params.name);
      if (!validator) throw new WorkerError("UNKNOWN_TOOL", `Unknown tool: ${request.params.name}`);
      const parsed = await validator["~standard"].validate(request.params.arguments ?? {});
      if (parsed.issues) {
        throw new WorkerError("INVALID_ARGUMENTS", "Tool arguments do not match its input schema", {
          issues: parsed.issues.map((issue) => issue.message),
        });
      }
      result = nativeToolResult(await worker.request(request.params.name, parsed.value), live);
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

function nativeToolResult(value: unknown, live: boolean): CallToolResult {
  if (live && isObject(value) && "image" in value) {
    throw new WorkerError("INVALID_RESULT", "Live inspection does not return image artifacts");
  }
  return toolResult(value);
}
