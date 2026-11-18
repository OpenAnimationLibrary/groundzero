import { Client, InMemoryTransport } from "@modelcontextprotocol/client";
import { afterEach, expect, test, vi } from "vitest";
import { sessionEffects } from "#mcp/declarations";
import { createServer } from "#mcp/server";
import { MAX_EXPOSED_TOOLS, TOOLSET_TOOLS } from "#mcp/toolsets";

function nativeTool(name: string) {
  return {
    name,
    description: `Native ${name}`,
    inputSchema: { type: "object", additionalProperties: false, properties: {} },
    annotations: { readOnlyHint: true, destructiveHint: false, idempotentHint: true },
    effects: { ...sessionEffects("read"), document: "reads" },
    errors: ["invalid_params"],
  };
}

interface Group {
  name: string;
  tools: string[];
}

function capabilities(groups: Group[]) {
  return {
    protocol_version: 2,
    tools: groups.flatMap((group) => group.tools.map((name) => nativeTool(name))),
    toolsets: groups.map((group) => ({
      name: group.name,
      description: `The ${group.name} tools`,
      tools: group.tools,
    })),
  };
}

const groups: Group[] = [
  { name: "core", tools: ["scene.summary", "scene.create"] },
  { name: "audio", tools: ["audio.inspect", "audio.mixdown"] },
  { name: "rigging", tools: ["rig.inspect"] },
];

const closers: (() => Promise<void>)[] = [];

afterEach(async () => {
  for (const close of closers.splice(0)) await close();
});

async function session(toolsets?: string[], catalog = capabilities(groups)) {
  const request = vi.fn(async (method: string) =>
    method === "automation.capabilities" ? catalog : { ok: method },
  );
  const server = createServer({ request, close: () => {} }, toolsets);
  const client = new Client({ name: "toolset-test", version: "1.0.0" });
  const changes = vi.fn();
  client.setNotificationHandler("notifications/tools/list_changed", async () => changes());
  const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
  await server.connect(serverTransport);
  await client.connect(clientTransport);
  closers.push(async () => {
    await client.close();
    await server.close();
  });
  return { client, request, changes };
}

async function names(client: Client): Promise<string[]> {
  return (await client.listTools()).tools.map((tool) => tool.name);
}

const toolsetNames = TOOLSET_TOOLS.map((tool) => tool.name);

test("exposes only core and the toolset tools by default", async () => {
  const { client } = await session();
  expect(await names(client)).toEqual(["scene.summary", "scene.create", ...toolsetNames]);
});

test("refuses a tool of a disabled toolset without calling the worker", async () => {
  const { client, request } = await session();
  const result = await client.callTool({ name: "audio.mixdown", arguments: {} });
  expect(result.isError).toBe(true);
  expect(result.structuredContent).toMatchObject({
    error: { code: "TOOLSET_DISABLED", details: { toolset: "audio" } },
  });
  expect(request).toHaveBeenCalledTimes(1);
});

test("lists toolsets with counts and enabled state", async () => {
  const { client } = await session();
  const result = await client.callTool({ name: "toolsets.list", arguments: {} });
  expect(result.structuredContent).toEqual({
    toolsets: [
      { name: "core", description: "The core tools", tool_count: 2, enabled: true },
      { name: "audio", description: "The audio tools", tool_count: 2, enabled: false },
      { name: "rigging", description: "The rigging tools", tool_count: 1, enabled: false },
    ],
    exposed_tool_count: 2,
  });
});

test("enabling a toolset exposes its tools and announces the change once", async () => {
  const { client, request, changes } = await session();
  const enabled = await client.callTool({
    name: "toolsets.enable",
    arguments: { names: ["audio"] },
  });
  expect(enabled.structuredContent).toMatchObject({ changed: true, exposed_tool_count: 4 });
  await vi.waitFor(() => expect(changes).toHaveBeenCalledTimes(1));
  expect(await names(client)).toContain("audio.mixdown");
  const call = await client.callTool({ name: "audio.mixdown", arguments: {} });
  expect(call.structuredContent).toEqual({ ok: "audio.mixdown" });
  expect(request).toHaveBeenLastCalledWith("audio.mixdown", {});
  const again = await client.callTool({ name: "toolsets.enable", arguments: { names: ["audio"] } });
  expect(again.structuredContent).toMatchObject({ changed: false });
  expect(changes).toHaveBeenCalledTimes(1);
});

test("disabling hides tools again but never disables core", async () => {
  const { client, changes } = await session(["audio"]);
  const core = await client.callTool({ name: "toolsets.disable", arguments: { names: ["core"] } });
  expect(core.structuredContent).toMatchObject({ error: { code: "TOOLSET_REQUIRED" } });
  await client.callTool({ name: "toolsets.disable", arguments: { names: ["audio"] } });
  await vi.waitFor(() => expect(changes).toHaveBeenCalledTimes(1));
  expect(await names(client)).not.toContain("audio.mixdown");
});

test("rejects unknown toolset names without changing the session", async () => {
  const { client, changes } = await session();
  const result = await client.callTool({
    name: "toolsets.enable",
    arguments: { names: ["audio", "nonexistent"] },
  });
  expect(result.structuredContent).toMatchObject({
    error: { code: "UNKNOWN_TOOLSET", details: { unknown: ["nonexistent"] } },
  });
  expect(await names(client)).not.toContain("audio.mixdown");
  expect(changes).not.toHaveBeenCalled();
});

test("refuses to expose more than the tool limit and keeps the session unchanged", async () => {
  const large = (prefix: string, count: number) =>
    Array.from({ length: count }, (_, index) => `${prefix}.tool${index}`);
  const catalog = capabilities([
    { name: "core", tools: large("core", 200) },
    { name: "extra", tools: large("extra", MAX_EXPOSED_TOOLS - 199) },
  ]);
  const { client } = await session(undefined, catalog);
  const result = await client.callTool({
    name: "toolsets.enable",
    arguments: { names: ["extra"] },
  });
  expect(result.structuredContent).toMatchObject({
    error: { code: "TOOLSET_LIMIT", details: { exposed_tool_count: MAX_EXPOSED_TOOLS + 1 } },
  });
  expect((await names(client)).length).toBe(200 + TOOLSET_TOOLS.length);
});

test("selects toolsets at startup, including all", async () => {
  const { client } = await session(["all"]);
  expect(await names(client)).toEqual([...groups.flatMap((group) => group.tools), ...toolsetNames]);
  const { client: unknown } = await session(["nonexistent"]);
  await expect(names(unknown)).rejects.toMatchObject({ data: { code: "UNKNOWN_TOOLSET" } });
});

test.each([
  [
    "a tool in two toolsets",
    [
      { name: "core", tools: ["a"] },
      { name: "x", tools: ["a"] },
    ],
    ["a"],
  ],
  ["a tool in no toolset", [{ name: "core", tools: ["a"] }], ["a", "b"]],
  ["no core toolset", [{ name: "audio", tools: ["a"] }], ["a"]],
  ["a reserved tool name", [{ name: "core", tools: ["toolsets.list"] }], ["toolsets.list"]],
])("rejects a native catalog with %s", async (_label, toolsets, tools) => {
  const catalog = {
    protocol_version: 2,
    tools: tools.map((name) => nativeTool(name)),
    toolsets: toolsets.map((group) => ({ ...group, description: "Tools" })),
  };
  const { client } = await session(undefined, catalog);
  await expect(names(client)).rejects.toMatchObject({ data: { code: "INVALID_CATALOG" } });
});

test("toolset tools publish output schemas and declarations their results satisfy", async () => {
  const { client } = await session();
  const listed = (await client.listTools()).tools.filter((tool) =>
    tool.name.startsWith("toolsets."),
  );
  expect(listed.map((tool) => tool.name)).toEqual(toolsetNames);
  for (const tool of listed) {
    expect(tool.outputSchema).toMatchObject({ type: "object" });
    expect(tool._meta?.["groundzero/effects"]).toMatchObject({ document: "none", files: [] });
    expect(tool._meta?.["groundzero/errors"]).toContain("INVALID_ARGUMENTS");
  }
  expect(listed[0]?.annotations?.readOnlyHint).toBe(true);
  for (const [name, args] of [
    ["toolsets.list", {}],
    ["toolsets.enable", { names: ["audio"] }],
    ["toolsets.disable", { names: ["audio"] }],
  ] as const) {
    const result = await client.callTool({ name, arguments: args });
    expect(result.isError).toBeUndefined();
  }
});
