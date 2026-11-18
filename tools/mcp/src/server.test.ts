import { Client, InMemoryTransport } from "@modelcontextprotocol/client";
import { afterEach, expect, test, vi } from "vitest";
import { WorkerError } from "#mcp/errors";
import { createLiveServer, createServer } from "#mcp/server";
import { TOOLSET_TOOLS } from "#mcp/toolsets";

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

async function connected(server: ReturnType<typeof createServer>): Promise<Client> {
  const client = new Client({ name: "adapter-test", version: "1.0.0" });
  const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
  await server.connect(serverTransport);
  await client.connect(clientTransport);
  closers.push(async () => {
    await client.close();
    await server.close();
  });
  return client;
}

async function session() {
  const request = vi.fn(async (method: string, params: Record<string, unknown> = {}) => {
    if (method === "automation.capabilities") {
      return {
        protocol_version: 3,
        tools: [{ ...tool, mutates_document: true, toolset: "core" }],
        toolsets: [{ name: "core", description: "Core tools", tools: [tool.name] }],
      };
    }
    return { name: params["name"], revision: "next" };
  });
  const close = vi.fn();
  const client = await connected(createServer({ request, close }));
  return { client, request, close };
}

afterEach(async () => {
  for (const close of closers.splice(0)) await close();
});

test("advertises only tools implemented by the current native worker", async () => {
  const { client } = await session();
  const result = await client.listTools();
  expect(result.tools).toEqual([tool, ...TOOLSET_TOOLS]);
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

const core = (names: string[]) => [{ name: "core", description: "Core tools", tools: names }];

test.each([
  { protocol_version: 2, tools: [tool], toolsets: core([tool.name]) },
  { protocol_version: 3, tools: [tool, tool], toolsets: core([tool.name]) },
  { protocol_version: 3, tools: [{ name: "missing-schema" }], toolsets: core(["missing-schema"]) },
  {
    protocol_version: 3,
    tools: Array.from({ length: 1025 }, (_, i) => ({ ...tool, name: `tool.${i}` })),
    toolsets: core(Array.from({ length: 1025 }, (_, i) => `tool.${i}`)),
  },
])("rejects an incompatible or malformed native catalog on first use", async (catalog) => {
  await expectUnusableCatalog(catalog, /^(INCOMPATIBLE_WORKER|INVALID_CATALOG)$/u);
});

test("compiles and rejects invalid native schemas before accepting tool calls", async () => {
  const invalid = {
    ...tool,
    inputSchema: { type: "object", properties: { name: { type: "not-a-schema-type" } } },
  };
  const catalog = { protocol_version: 3, tools: [invalid], toolsets: core([tool.name]) };
  await expectUnusableCatalog(catalog, /^INVALID_CATALOG$/u);
});

async function expectUnusableCatalog(catalog: unknown, code: RegExp): Promise<void> {
  const request = vi.fn(async () => catalog);
  const unavailable = vi.fn();
  const client = await connected(createServer({ request, close: () => {} }, ["core"], unavailable));
  expect(request).not.toHaveBeenCalled();
  await expect(client.listTools()).rejects.toMatchObject({
    code: -32603,
    data: { code: expect.stringMatching(code) },
  });
  const call = await client.callTool({ name: tool.name, arguments: {} });
  expect(call.structuredContent).toMatchObject({ error: { code: expect.stringMatching(code) } });
  expect(request).toHaveBeenCalledTimes(1);
  expect(unavailable).toHaveBeenCalledTimes(1);
  expect(unavailable.mock.calls[0]?.[0]).toBeInstanceOf(WorkerError);
}

test("negotiates without the worker and reads its catalog once, on first use", async () => {
  const { client, request } = await session();
  expect(request).not.toHaveBeenCalled();
  await client.listTools();
  await client.listTools();
  expect(request.mock.calls).toEqual([["automation.capabilities"]]);
});

function liveCatalog() {
  return [
    "live.status",
    "live.inspect",
    "live.stage.set_key_value",
    "live.fx.set_scalar_value",
    "live.receipt",
    "live.capabilities",
  ].map((name) => ({
    ...tool,
    name,
    inputSchema: {
      type: "object" as const,
      additionalProperties: false,
      required: ["operation_id", "expected_fingerprint"],
      properties: {
        operation_id: { type: "string" as const },
        expected_fingerprint: { type: "string" as const },
      },
    },
  }));
}

async function liveSession(mode: "inspect" | "edit") {
  const request = vi.fn(async (_method: string, params: Record<string, unknown> = {}) => ({
    mode,
    operation_id: params["operation_id"],
  }));
  const server = await createLiveServer(
    { request, close: () => {} },
    {
      instanceId: "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
      clientId: "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb",
      mode,
      tools: liveCatalog(),
      limits: {},
    },
  );
  const client = new Client({ name: "live-adapter-test", version: "1.0.0" });
  const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
  await server.connect(serverTransport);
  await client.connect(clientTransport);
  closers.push(async () => {
    await client.close();
    await server.close();
  });
  return { client, request };
}

test.each(["inspect", "edit"] as const)(
  "uses only the authenticated live catalog in %s mode",
  async (mode) => {
    const { client, request } = await liveSession(mode);
    expect((await client.listTools()).tools.map((entry) => entry.name)).toEqual(
      liveCatalog().map((entry) => entry.name),
    );
    expect(request).not.toHaveBeenCalled();
    const args = { operation_id: "shared-operation", expected_fingerprint: "a".repeat(64) };
    await client.callTool({ name: "live.stage.set_key_value", arguments: args });
    expect(request).toHaveBeenLastCalledWith("live.stage.set_key_value", args);
    const rejected = await client.callTool({ name: "scene.create", arguments: {} });
    expect(rejected.isError).toBe(true);
    expect(request).toHaveBeenCalledTimes(1);
  },
);

test("retains native live permission decisions", async () => {
  const { client, request } = await liveSession("inspect");
  const args = { operation_id: "same-operation", expected_fingerprint: "b".repeat(64) };
  request.mockRejectedValueOnce(
    new WorkerError("permission_denied", "This session is inspect-only"),
  );
  const denied = await client.callTool({ name: "live.stage.set_key_value", arguments: args });
  expect(denied.structuredContent).toMatchObject({ error: { code: "permission_denied" } });
});

test("rejects a live catalog that adds detached authority or omits an approved method", async () => {
  const entries = liveCatalog();
  for (const tools of [entries.slice(1), [...entries.slice(1), tool]]) {
    await expect(
      createLiveServer(
        { request: async () => ({}), close: () => {} },
        {
          instanceId: "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
          clientId: "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb",
          mode: "edit",
          tools,
          limits: {},
        },
      ),
    ).rejects.toMatchObject({ code: "INVALID_CATALOG" });
  }
});
