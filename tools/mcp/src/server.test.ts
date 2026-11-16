import { Client, InMemoryTransport } from "@modelcontextprotocol/client";
import { afterEach, expect, test, vi } from "vitest";
import { WorkerError } from "#mcp/errors";
import { createServer } from "#mcp/server";

const tool = {
  name: "scene.rename",
  description: "Rename the private scene",
  inputSchema: {
    type: "object",
    additionalProperties: false,
    required: ["name", "operation_id", "expected_revision"],
    properties: {
      name: { type: "string", maxLength: 80 },
      operation_id: { type: "string" },
      expected_revision: { type: "string" },
    },
  },
  annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true },
};

const closers: (() => Promise<void>)[] = [];

async function session() {
  const request = vi.fn(async (method: string, params: Record<string, unknown> = {}) => {
    if (method === "automation.capabilities") {
      return { protocol_version: 1, tools: [{ ...tool, mutates_document: true }] };
    }
    return { name: params["name"], revision: "next" };
  });
  const close = vi.fn();
  const server = await createServer({ request, close });
  const client = new Client({ name: "adapter-test", version: "1.0.0" });
  const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
  await server.connect(serverTransport);
  await client.connect(clientTransport);
  closers.push(async () => {
    await client.close();
    await server.close();
  });
  return { client, request, close };
}

afterEach(async () => {
  for (const close of closers.splice(0)) await close();
});

test("advertises only tools implemented by the current native worker", async () => {
  const { client } = await session();
  const result = await client.listTools();
  expect(result.tools).toEqual([tool]);
});

test("passes operation identities unchanged to native mutations", async () => {
  const { client, request } = await session();
  const args = { name: "Shot", operation_id: "rename-7", expected_revision: "epoch:6" };
  const result = await client.callTool({ name: tool.name, arguments: args });
  expect(result.structuredContent).toEqual({ name: "Shot", revision: "next" });
  expect(request).toHaveBeenLastCalledWith(tool.name, args);
});

test.each([
  { name: "Shot" },
  { name: 42, operation_id: "rename-7", expected_revision: "epoch:6" },
  { name: "Shot", operation_id: "rename-7", expected_revision: "epoch:6", script: "anything" },
])("rejects invalid mutation arguments before they reach native state", async (args) => {
  const { client, request } = await session();
  const result = await client.callTool({ name: tool.name, arguments: args });
  expect(result.isError).toBe(true);
  expect(request).toHaveBeenCalledTimes(1);
});

test("rejects unadvertised method names", async () => {
  const { client, request } = await session();
  const result = await client.callTool({ name: "execute_script", arguments: {} });
  expect(result.isError).toBe(true);
  expect(request).toHaveBeenCalledTimes(1);
});

test("keeps native failures visible as tool errors", async () => {
  const { client, request } = await session();
  request.mockRejectedValueOnce(new WorkerError("REVISION_CONFLICT", "Scene changed"));
  const result = await client.callTool({
    name: tool.name,
    arguments: { name: "Shot", operation_id: "rename-7", expected_revision: "epoch:6" },
  });
  expect(result.isError).toBe(true);
  expect(result.structuredContent).toMatchObject({ error: { code: "REVISION_CONFLICT" } });
});

test.each([
  { protocol_version: 2, tools: [] },
  { protocol_version: 1, tools: [tool, tool] },
  { protocol_version: 1, tools: [{ name: "missing-schema" }] },
  { protocol_version: 1, tools: Array.from({ length: 257 }, () => tool) },
])("rejects an incompatible or malformed native catalog", async (catalog) => {
  await expect(
    createServer({ request: async () => catalog, close: () => {} }),
  ).rejects.toBeInstanceOf(WorkerError);
});
