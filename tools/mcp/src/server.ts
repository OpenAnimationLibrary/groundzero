import { Server, fromJsonSchema } from "@modelcontextprotocol/server";
import type { Tool } from "@modelcontextprotocol/server";
import { WorkerError, errorResult, isObject } from "#mcp/errors";
import { toolResult } from "#mcp/results";
import type { WorkerConnection } from "#mcp/worker";

interface NativeApi {
  request: WorkerConnection["request"];
  close: WorkerConnection["close"];
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
  const entries = result["tools"];
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
  const validators = new Map(
    tools.map((tool) => [tool.name, fromJsonSchema<Record<string, unknown>>(tool.inputSchema)]),
  );
  const server = new Server(
    { name: "groundzero", version: "0.1.0" },
    { capabilities: { tools: {} } },
  );
  server.onclose = () => worker.close();
  server.setRequestHandler("tools/list", async () => ({ tools }));
  server.setRequestHandler("tools/call", async (request) => {
    try {
      const validator = validators.get(request.params.name);
      if (!validator) throw new WorkerError("UNKNOWN_TOOL", `Unknown tool: ${request.params.name}`);
      const parsed = await validator["~standard"].validate(request.params.arguments ?? {});
      if (parsed.issues) {
        throw new WorkerError("INVALID_ARGUMENTS", "Tool arguments do not match its input schema", {
          issues: parsed.issues.map((issue) => issue.message),
        });
      }
      return toolResult(await worker.request(request.params.name, parsed.value));
    } catch (error) {
      return errorResult(error);
    }
  });
  return server;
}
