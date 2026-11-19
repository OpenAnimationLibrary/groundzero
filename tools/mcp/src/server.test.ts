import { Client, InMemoryTransport } from "@modelcontextprotocol/client";
import { afterEach, expect, test, vi } from "vitest";
import { ADAPTER_ERRORS, ERROR_ENVELOPE, outputSchema, sessionEffects } from "#mcp/declarations";
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
  annotations: {
    readOnlyHint: false,
    destructiveHint: true,
    idempotentHint: true,
    openWorldHint: false,
  },
};

const renamed = {
  type: "object",
  additionalProperties: false,
  required: ["name", "revision"],
  properties: { name: { type: "string" }, revision: { type: "string" } },
};

const declared = {
  outputSchema: outputSchema(renamed),
  effects: {
    ...sessionEffects("read"),
    class: "document_write",
    document: "writes",
    overwrites: true,
    no_op: "revision_kept",
    after_change_failure: "partially_applied",
    retry: "receipt_replay",
    grants: ["inspect", "draft"],
  },
  errors: ["invalid_params", "revision_conflict"],
};

const closers: (() => Promise<void>)[] = [];
const liveBuild = {
  application: "groundzero" as const,
  version: "1.8",
  build_digest: "c".repeat(64),
};

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

const describe = {
  name: "tool.describe",
  description: "Describe tools",
  inputSchema: {
    type: "object",
    additionalProperties: false,
    required: ["names"],
    properties: { names: { type: "array", items: { type: "string" }, maxItems: 16 } },
  },
  annotations: { ...tool.annotations, readOnlyHint: true, destructiveHint: false },
  outputSchema: outputSchema({ type: "object" }),
  effects: sessionEffects("read"),
  errors: ["invalid_params", "unknown_tool"],
};

async function session(strict = false) {
  const request = vi.fn(async (method: string, params: Record<string, unknown> = {}) => {
    if (method === "automation.capabilities") {
      return {
        protocol_version: 3,
        tools: [
          { ...tool, ...declared, toolset: "core" },
          { ...describe, toolset: "core" },
        ],
        toolsets: [{ name: "core", description: "Core tools", tools: [tool.name, describe.name] }],
      };
    }
    if (method === "tool.describe") {
      return { tools: [{ name: tool.name, toolset: "core", ...declared }] };
    }
    return { name: params["name"], revision: "next" };
  });
  const close = vi.fn();
  const client = await connected(createServer({ request, close }, ["core"], () => {}, strict));
  return { client, request, close };
}

afterEach(async () => {
  for (const close of closers.splice(0)) await close();
});

test("advertises only tools implemented by the current native worker", async () => {
  const { client } = await session();
  const result = await client.listTools();
  expect(result.tools.map((entry) => entry.name)).toEqual([
    tool.name,
    describe.name,
    ...TOOLSET_TOOLS.map((entry) => entry.name),
  ]);
});

test("lists only name, description, input schema and annotations", async () => {
  const { client } = await session();
  const [listed] = (await client.listTools()).tools;
  expect(listed).toEqual(tool);
});

test("describes native and toolset tools with the adapter's own error codes", async () => {
  const { client, request } = await session();
  const names = [tool.name, "toolsets.list"];
  const result = await client.callTool({ name: describe.name, arguments: { names } });
  expect(request).toHaveBeenLastCalledWith(describe.name, { names: [tool.name] });
  const tools = (result.structuredContent as { tools: Record<string, unknown>[] }).tools;
  expect(tools.map((entry) => entry["name"])).toEqual(names);
  expect(tools[0]?.["errors"]).toEqual([...declared.errors, ...ADAPTER_ERRORS]);
  expect(tools[0]?.["outputSchema"]).toEqual(declared.outputSchema);
  expect(tools[1]?.["effects"]).toMatchObject({ class: "read", document: "none" });
});

test("returns a nonconforming result unchanged, flagged, with only its paths on stderr", async () => {
  const { client, request } = await session();
  await client.listTools();
  const stderr = vi.spyOn(process.stderr, "write").mockImplementation(() => true);
  request.mockResolvedValueOnce({ name: "Shot", revision: 7 } as never);
  const args = { name: "Shot", operation_id: "rename-8", expected_revision: "epoch:6" };
  const result = await client.callTool({ name: tool.name, arguments: args });
  const lines = stderr.mock.calls.map(([line]) => String(line));
  stderr.mockRestore();
  expect(result.isError).toBeUndefined();
  expect(result.structuredContent).toEqual({ name: "Shot", revision: 7 });
  expect(result._meta).toEqual({ "groundzero/output_mismatch": true });
  expect(lines).toEqual([
    "groundzero-mcp: scene.rename result does not match its output schema at revision\n",
  ]);
});

test("a strict session refuses nonconforming content and reports the applied write", async () => {
  const { client, request } = await session(true);
  await client.listTools();
  request.mockResolvedValueOnce({ name: "Shot", revision: 7 } as never);
  const args = { name: "Shot", operation_id: "rename-8", expected_revision: "epoch:6" };
  const result = await client.callTool({ name: tool.name, arguments: args });
  expect(result.isError).toBe(true);
  expect(result.structuredContent).toMatchObject({
    error: {
      code: "INVALID_RESULT",
      details: { tool: tool.name, outcome: "applied", operation_id: "rename-8" },
    },
  });
  const issues = (result.structuredContent as { error: { details: { issues: string[] } } }).error
    .details.issues;
  expect(issues).toEqual(["revision"]);
});

test("names every failing path of a nonconforming result", async () => {
  const { client, request } = await session(true);
  await client.listTools();
  request.mockResolvedValueOnce({ name: 4, revision: 7, stray: "x" } as never);
  const args = { name: "Shot", operation_id: "rename-10", expected_revision: "epoch:6" };
  const result = await client.callTool({ name: tool.name, arguments: args });
  const details = (result.structuredContent as { error: { details: { issues: string[] } } }).error
    .details;
  expect(details.issues).toEqual(["(result)", "name", "revision"]);
});

test("error results conform to the error envelope of every output schema", async () => {
  const { client, request } = await session();
  request.mockRejectedValueOnce(new WorkerError("revision_conflict", "Scene changed", { a: 1 }));
  const args = { name: "Shot", operation_id: "rename-9", expected_revision: "epoch:6" };
  const failure = await client.callTool({ name: tool.name, arguments: args });
  const invalid = await client.callTool({ name: tool.name, arguments: { name: 1 } });
  for (const result of [failure, invalid]) {
    expect(result.isError).toBe(true);
    const content = result.structuredContent as { error: Record<string, unknown> };
    expect(Object.keys(content)).toEqual(["error"]);
    expect(Object.keys(content.error).sort()).toEqual(
      [...ERROR_ENVELOPE.properties.error.required].sort(),
    );
  }
});

test.each([
  { ...declared, outputSchema: { type: "array" } },
  { ...declared, outputSchema: renamed },
  { ...declared, outputSchema: { type: "object", oneOf: [renamed, renamed] } },
  { ...declared, effects: undefined },
  { ...declared, outputSchema: undefined },
  { ...declared, errors: [] },
  { ...declared, effects: { ...declared.effects, class: "writes_everything" } },
])("rejects a catalog whose output schema or declarations are malformed", async (fields) => {
  const tools = [{ ...tool, ...fields, toolset: "core" }];
  await expectUnusableCatalog(
    { protocol_version: 3, tools, toolsets: core([tool.name]) },
    /^INVALID_CATALOG$/u,
  );
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
  { protocol_version: 1, tools: [{ ...tool, ...declared }], toolsets: core([tool.name]) },
  {
    protocol_version: 3,
    tools: [
      { ...tool, ...declared },
      { ...tool, ...declared },
    ],
    toolsets: core([tool.name]),
  },
  { protocol_version: 3, tools: [{ name: "missing-schema" }], toolsets: core(["missing-schema"]) },
  {
    protocol_version: 3,
    tools: Array.from({ length: 1025 }, (_, i) => ({ ...tool, ...declared, name: `tool.${i}` })),
    toolsets: core(Array.from({ length: 1025 }, (_, i) => `tool.${i}`)),
  },
])("rejects an incompatible or malformed native catalog on first use", async (catalog) => {
  await expectUnusableCatalog(catalog, /^(INCOMPATIBLE_WORKER|INVALID_CATALOG)$/u);
});

test("compiles and rejects invalid native schemas before accepting tool calls", async () => {
  const invalid = {
    ...tool,
    ...declared,
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
      protocol: "groundzero-live-v2",
      build: liveBuild,
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
          protocol: "groundzero-live-v2",
          build: liveBuild,
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
