import { Client, parseJSONRPCMessage } from "@modelcontextprotocol/client";
import type { JSONRPCMessage } from "@modelcontextprotocol/client";
import { StdioClientTransport } from "@modelcontextprotocol/client/stdio";
import { spawn } from "node:child_process";
import type { ChildProcessWithoutNullStreams } from "node:child_process";
import { once } from "node:events";
import { readFile, rm } from "node:fs/promises";
import { join } from "node:path";
import { setTimeout as pause } from "node:timers/promises";
import { fileURLToPath } from "node:url";
import { afterEach, expect, test } from "vitest";
import { isObject } from "#mcp/errors";
import { MAX_LINE_BYTES } from "#mcp/stdio-guard";

interface FakeNative {
  directory: string;
  workspace: string;
  executable: string;
}

const fixture = (await import(new URL("../fixtures/fake-native.mjs", import.meta.url).href)) as {
  fakeNativeExecutable: () => Promise<FakeNative>;
};
const cli = fileURLToPath(new URL("../dist/cli.js", import.meta.url));
const posix = test.skipIf(process.platform === "win32");
const scratch: string[] = [];
const cleanups: (() => Promise<void>)[] = [];

afterEach(async () => {
  for (const cleanup of cleanups.splice(0)) await cleanup();
  for (const directory of scratch.splice(0)) await rm(directory, { recursive: true, force: true });
});

async function nativeGrants(): Promise<{ args: string[]; log: string }> {
  const { directory, workspace, executable } = await fixture.fakeNativeExecutable();
  scratch.push(directory);
  const args = ["--executable", executable, "--workspace", workspace];
  return { args, log: join(directory, "native-requests.log") };
}

async function nativeRequests(log: string): Promise<string[]> {
  return (await readFile(log, "utf8")).trim().split("\n");
}

type Frame = Record<string, unknown>;

/** Accept an MCP message, or the JSON-RPC error with a null id that answers a malformed line. */
function mcpFrame(line: string): Frame {
  const value: unknown = JSON.parse(line);
  if (isObject(value) && value["id"] === null) {
    expect(Object.keys(value).sort()).toEqual(["error", "id", "jsonrpc"]);
    expect(value["jsonrpc"]).toBe("2.0");
    expect(Object.keys(value["error"] as object).sort()).toEqual(["code", "message"]);
    return value;
  }
  return parseJSONRPCMessage(value) as Frame;
}

/** Parse adapter stdout, requiring every byte to belong to a newline-ended MCP frame. */
function mcpFrames(stdout: string): Frame[] {
  expect(stdout === "" || stdout.endsWith("\n")).toBe(true);
  return stdout
    .split("\n")
    .filter((line) => line !== "")
    .map(mcpFrame);
}

function malformedReply(id: number | string | null, code: number): Frame {
  const message = code === -32700 ? "Parse error" : "Invalid Request";
  return { jsonrpc: "2.0", id, error: { code, message } };
}

class RawAdapter {
  readonly child: ChildProcessWithoutNullStreams;
  stdout = "";
  stderr = "";

  constructor(args: string[], log: string) {
    this.child = spawn(process.execPath, [cli, ...args], {
      env: { ...process.env, FAKE_NATIVE_LOG: log },
    });
    this.child.stdout.setEncoding("utf8").on("data", (chunk: string) => (this.stdout += chunk));
    this.child.stderr.setEncoding("utf8").on("data", (chunk: string) => (this.stderr += chunk));
    cleanups.push(async () => {
      if (this.child.exitCode !== null || this.child.signalCode !== null) return;
      const closed = once(this.child, "close");
      this.child.kill("SIGKILL");
      await closed;
    });
  }

  write(frame: unknown): void {
    this.child.stdin.write(`${typeof frame === "string" ? frame : JSON.stringify(frame)}\n`);
  }

  call(id: number, name: string, args: Record<string, unknown>): void {
    this.write({ jsonrpc: "2.0", id, method: "tools/call", params: { name, arguments: args } });
  }

  async response(id: number): Promise<Record<string, unknown>> {
    const deadline = performance.now() + 15_000;
    while (performance.now() < deadline) {
      const found = this.frames().find((frame) => frame["id"] === id);
      if (found) return found;
      await pause(20);
    }
    throw new Error(`No response to request ${id}: ${this.stderr}`);
  }

  frames(): Frame[] {
    const complete = this.stdout.slice(0, this.stdout.lastIndexOf("\n") + 1);
    return mcpFrames(complete);
  }

  async end(): Promise<number | null> {
    const closed = once(this.child, "close");
    this.child.stdin.end();
    const [code] = (await closed) as [number | null];
    mcpFrames(this.stdout);
    return code;
  }
}

function toolResult(response: Record<string, unknown>): Record<string, unknown> {
  const result = response["result"];
  if (!isObject(result)) throw new Error(`Expected a tool result: ${JSON.stringify(response)}`);
  return result;
}

function toolError(response: Record<string, unknown>): Record<string, unknown> {
  const result = toolResult(response);
  expect(result["isError"]).toBe(true);
  const content = result["structuredContent"];
  if (!isObject(content) || !isObject(content["error"])) throw new Error("No structured error");
  return content["error"];
}

test.each([
  [["--workspace", "relative"]],
  [["--executable", process.execPath, "--workspace", "relative"]],
  [["--executable", process.execPath, "--workspace", "/", "--toolsets", "Bad"]],
])("refuses the launch %j with nothing on stdout", async (args) => {
  const child = spawn(process.execPath, [cli, ...args]);
  let stdout = "";
  let stderr = "";
  child.stdout.setEncoding("utf8").on("data", (chunk: string) => (stdout += chunk));
  child.stderr.setEncoding("utf8").on("data", (chunk: string) => (stderr += chunk));
  child.stdin.end();
  const [code] = (await once(child, "close")) as [number | null];
  expect(code).toBe(1);
  expect(stdout).toBe("");
  expect(stderr).toMatch(/^Groundzero MCP startup failed: /u);
});

posix("answers each malformed line once, forwards none and keeps serving", async () => {
  const { args, log } = await nativeGrants();
  const adapter = new RawAdapter(args, log);
  adapter.call(1, "probe.echo", { value: 1 });
  await adapter.response(1);
  const secret = "SECRET-INPUT";
  const lines: [string, Frame | undefined][] = [
    [`{malformed json ${secret}`, malformedReply(null, -32700)],
    [`\u0001 ${secret}`, malformedReply(null, -32700)],
    ["[]", malformedReply(null, -32600)],
    [JSON.stringify([{ jsonrpc: "2.0", id: 90, method: "ping" }]), malformedReply(null, -32600)],
    [JSON.stringify({ jsonrpc: "1.0", id: 91, method: "ping" }), malformedReply(91, -32600)],
    [
      JSON.stringify({ jsonrpc: "2.0", id: { secret }, method: "ping" }),
      malformedReply(null, -32600),
    ],
    [
      JSON.stringify({ jsonrpc: "1.0", id: "x".repeat(129), method: "ping" }),
      malformedReply(null, -32600),
    ],
    [
      JSON.stringify({ jsonrpc: "2.0", id: 92, method: "tools/call", params: secret }),
      malformedReply(92, -32600),
    ],
    [
      JSON.stringify({ jsonrpc: "2.0", id: "ninety-three" }),
      malformedReply("ninety-three", -32600),
    ],
    [JSON.stringify({ jsonrpc: "1.0", method: "notifications/initialized" }), undefined],
    [JSON.stringify({ jsonrpc: "1.0", id: 94, result: { secret } }), undefined],
    [JSON.stringify({ jsonrpc: "2.0", error: secret }), undefined],
    ["", undefined],
    ["   \r", undefined],
  ];
  for (const [line] of lines) adapter.write(line);
  adapter.write({ jsonrpc: "2.0", id: 2, method: "ping" });
  expect(await adapter.response(2)).toMatchObject({ result: {} });
  await pause(200);
  const replies = adapter.frames().filter((frame) => frame["id"] !== 1 && frame["id"] !== 2);
  expect(replies).toEqual(lines.flatMap(([, reply]) => (reply ? [reply] : [])));
  expect(await adapter.end()).toBe(0);
  expect(adapter.stdout).not.toContain(secret);
  expect(await nativeRequests(log)).toEqual(["automation.capabilities", "probe.echo"]);
});

posix("answers a line at the 10 MiB bound and closes the session on a longer one", async () => {
  const { args, log } = await nativeGrants();
  const adapter = new RawAdapter(args, log);
  adapter.write("x".repeat(MAX_LINE_BYTES));
  adapter.write({ jsonrpc: "2.0", id: 1, method: "ping" });
  expect(await adapter.response(1)).toMatchObject({ result: {} });
  adapter.write("y".repeat(MAX_LINE_BYTES + 1));
  adapter.write({ jsonrpc: "2.0", id: 2, method: "ping" });
  await pause(500);
  expect(adapter.frames()).toEqual([
    malformedReply(null, -32700),
    { jsonrpc: "2.0", id: 1, result: {} },
  ]);
  expect(await adapter.end()).toBe(0);
});

posix("keeps stdout to MCP frames through native noise and large results", async () => {
  const { args, log } = await nativeGrants();
  const adapter = new RawAdapter(args, log);
  adapter.call(2, "probe.noise", { bytes: 256 * 1024 });
  expect(toolResult(await adapter.response(2))["structuredContent"]).toEqual({
    written: 256 * 1024,
  });
  adapter.call(3, "probe.large", { bytes: 7 * 1024 * 1024 });
  const large = toolResult(await adapter.response(3))["structuredContent"];
  expect(isObject(large) && typeof large["text"] === "string" && large["text"].length).toBe(
    7 * 1024 * 1024,
  );
  expect(await adapter.end()).toBe(0);
  expect(adapter.stderr).toContain("native diagnostic native diagnostic");
  expect(await nativeRequests(log)).toEqual([
    "automation.capabilities",
    "probe.noise",
    "probe.large",
  ]);
});

posix.each(["probe.crash", "probe.stdout_noise"])(
  "keeps stdout to MCP frames when %s ends the native worker",
  async (name) => {
    const { args, log } = await nativeGrants();
    const adapter = new RawAdapter(args, log);
    adapter.call(1, name, {});
    expect(toolError(await adapter.response(1))).toMatchObject({
      code: "WORKER_UNAVAILABLE",
      details: { outcome_unknown: true },
    });
    adapter.call(2, "probe.echo", { value: 1 });
    expect(toolError(await adapter.response(2))).toMatchObject({ code: "WORKER_UNAVAILABLE" });
    expect(await adapter.end()).toBe(0);
    expect(adapter.stdout).not.toContain("native diagnostic");
  },
);

posix("abandons a cancelled call without answering it or touching native state", async () => {
  const { args, log } = await nativeGrants();
  const adapter = new RawAdapter(args, log);
  adapter.call(1, "probe.slow", { value: 1, delay_ms: 300 });
  adapter.write({
    jsonrpc: "2.0",
    method: "notifications/cancelled",
    params: { requestId: 1, reason: "test" },
  });
  await pause(600);
  adapter.call(2, "probe.echo", { value: 2 });
  expect(toolResult(await adapter.response(2))["structuredContent"]).toEqual({ value: 2 });
  expect(adapter.frames().filter((frame) => frame["id"] === 1)).toEqual([]);
  expect(await adapter.end()).toBe(0);
  expect(await nativeRequests(log)).toEqual([
    "automation.capabilities",
    "probe.slow",
    "probe.echo",
  ]);
});

async function sdkSession(mode: "modern" | "legacy", capabilities = {}) {
  const { args, log } = await nativeGrants();
  const client = new Client(
    { name: "groundzero-cli-test", version: "1.0.0" },
    {
      capabilities,
      versionNegotiation: { mode: mode === "modern" ? { pin: "2026-07-28" } : "legacy" },
    },
  );
  const errors: Error[] = [];
  client.onerror = (error) => errors.push(error);
  const transport = new StdioClientTransport({
    command: process.execPath,
    args: [cli, ...args],
    env: { ...(process.env as Record<string, string>), FAKE_NATIVE_LOG: log },
    stderr: "pipe",
  });
  await client.connect(transport);
  cleanups.push(() => client.close());
  return { client, transport, errors, log };
}

const anyResult = {
  "~standard": {
    version: 1 as const,
    vendor: "groundzero-test",
    validate: (value: unknown) => ({ value }),
  },
};

posix.each(["modern", "legacy"] as const)(
  "answers resources, prompts, tasks and unknown methods with method not found (%s)",
  async (mode) => {
    const { client, errors } = await sdkSession(mode);
    expect(client.getServerCapabilities()).toEqual({ tools: { listChanged: true } });
    const requests: { method: string; params: Record<string, unknown> }[] = [
      { method: "resources/list", params: {} },
      { method: "resources/templates/list", params: {} },
      { method: "resources/read", params: { uri: "file:///etc/hosts" } },
      { method: "prompts/list", params: {} },
      { method: "prompts/get", params: { name: "anything" } },
      { method: "groundzero/execute", params: {} },
    ];
    if (mode === "legacy") {
      requests.push({ method: "tasks/list", params: {} });
      requests.push({ method: "tasks/get", params: { taskId: "anything" } });
    }
    for (const request of requests) {
      await expect(client.request(request, anyResult), request.method).rejects.toMatchObject({
        code: -32601,
      });
    }
    const result = await client.callTool({ name: "probe.echo", arguments: { value: 4 } });
    expect(result.structuredContent).toEqual({ value: 4 });
    expect(errors).toEqual([]);
  },
);

posix("serves a client that offers tasks without declaring or creating any", async () => {
  const tasks = { tasks: { list: {}, cancel: {}, requests: { sampling: { createMessage: {} } } } };
  const { client, errors } = await sdkSession("legacy", tasks);
  expect(client.getServerCapabilities()).not.toHaveProperty("tasks");
  const { tools } = await client.listTools();
  expect(tools.filter((tool) => "execution" in tool)).toEqual([]);
  const result = await client.callTool({ name: "probe.echo", arguments: { value: 5 } });
  expect(result).toEqual({
    content: [{ type: "text", text: '{"value":5}' }],
    structuredContent: { value: 5 },
  });
  expect(errors).toEqual([]);
});

posix("reports a cancelled call as abandoned by the client, never as cancelled work", async () => {
  const { client, transport, errors, log } = await sdkSession("modern");
  const abort = new AbortController();
  const send = transport.send.bind(transport);
  transport.send = async (message: JSONRPCMessage) => {
    await send(message);
    if ("method" in message && message.method === "tools/call") abort.abort("test");
  };
  const call = client.callTool(
    { name: "probe.slow", arguments: { value: 6, delay_ms: 300 } },
    { signal: abort.signal },
  );
  await expect(call).rejects.toThrow();
  await pause(600);
  const after = await client.callTool({ name: "probe.echo", arguments: { value: 7 } });
  expect(after.structuredContent).toEqual({ value: 7 });
  expect(errors).toEqual([]);
  expect(await nativeRequests(log)).toEqual([
    "automation.capabilities",
    "automation.capabilities",
    "probe.slow",
    "probe.echo",
  ]);
});
