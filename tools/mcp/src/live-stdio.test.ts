import { Client } from "@modelcontextprotocol/client";
import { StdioClientTransport } from "@modelcontextprotocol/client/stdio";
import { randomUUID } from "node:crypto";
import { once } from "node:events";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { createServer } from "node:net";
import type { Socket } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout } from "node:timers/promises";
import { fileURLToPath } from "node:url";
import { expect, test } from "vitest";
import { LIVE_TOOL_NAMES } from "#mcp/live-protocol";

type Profile = "modern" | "legacy";
const fixture = fileURLToPath(new URL("../fixtures/live-server.mjs", import.meta.url));

async function nativeBoundary() {
  const directory = await mkdtemp(join(tmpdir(), "gz-live-stdio-"));
  const endpoint =
    process.platform === "win32"
      ? `\\\\.\\pipe\\gz-live-fixture-${randomUUID()}`
      : join(directory, "socket");
  const sockets = new Set<Socket>();
  const observations = { peak: 0, accepted: 0, refused: 0 };
  const server = createServer((socket) => {
    socket.on("error", () => socket.destroy());
    if (sockets.size === 2) {
      observations.refused += 1;
      socket.end();
      return;
    }
    sockets.add(socket);
    observations.accepted += 1;
    observations.peak = Math.max(observations.peak, sockets.size);
    socket.once("close", () => sockets.delete(socket));
    socket.write("accepted");
  });
  try {
    server.listen(endpoint);
    await once(server, "listening");
  } catch (error) {
    await rm(directory, { recursive: true });
    throw error;
  }
  return { directory, endpoint, sockets, observations, server };
}

function clientSlot(profile: Profile, boundary: Awaited<ReturnType<typeof nativeBoundary>>) {
  const client = new Client(
    { name: `live-stdio-${profile}`, version: "1.0.0" },
    { versionNegotiation: { mode: profile === "modern" ? { pin: "2026-07-28" } : "legacy" } },
  );
  const environment: Record<string, string> = {};
  for (const [key, value] of Object.entries(process.env)) {
    if (value !== undefined) environment[key] = value;
  }
  const transport = new StdioClientTransport({
    command: process.execPath,
    args: [fixture],
    env: {
      ...environment,
      LIVE_FIXTURE_SOCKET: boundary.endpoint,
      LIVE_FIXTURE_WIRE: join(boundary.directory, "wire.ndjson"),
    },
    stderr: "pipe",
  });
  let diagnostics = "";
  transport.stderr?.on("data", (chunk: Buffer) => {
    diagnostics += chunk.toString("utf8");
  });
  return { client, transport, profile, diagnostics: () => diagnostics };
}

async function exercise(slot: ReturnType<typeof clientSlot>) {
  await slot.client.connect(slot.transport, { timeout: 3000 });
  expect(
    (await slot.client.listTools(undefined, { timeout: 3000 })).tools
      .map((tool) => tool.name)
      .sort(),
  ).toEqual([...LIVE_TOOL_NAMES].sort());
  const response = await slot.client.callTool(
    { name: "live.status", arguments: {} },
    {
      timeout: 3000,
    },
  );
  expect(response.isError).not.toBe(true);
  expect(response.structuredContent).toMatchObject({ method: "live.status" });
  expect(slot.diagnostics()).toBe("");
}

async function drained(boundary: Awaited<ReturnType<typeof nativeBoundary>>) {
  const deadline = Date.now() + 3000;
  while (boundary.sockets.size > 0 && Date.now() < deadline) await setTimeout(10);
  expect(boundary.sockets.size).toBe(0);
}

interface WireMessage {
  pid: number;
  method: string;
}

async function checkWire(boundary: Awaited<ReturnType<typeof nativeBoundary>>, modern: number) {
  const wire = await readFile(join(boundary.directory, "wire.ndjson"), "utf8");
  const messages: WireMessage[] = wire
    .trim()
    .split("\n")
    .map((line: string) => JSON.parse(line));
  const probes = messages.filter((message) => message.method === "server/discover");
  const active = messages.filter((message) => message.method === "tools/call");
  expect(probes).toHaveLength(modern);
  expect(new Set(probes.map((message) => message.pid)).size).toBe(modern);
  for (const probe of probes)
    expect(active.some((message) => message.pid === probe.pid)).toBe(false);
  expect(messages.filter((message) => message.method === "initialize")).toHaveLength(3 - modern);
  expect(boundary.observations).toEqual({ peak: 2, accepted: 3 + modern, refused: 0 });
}

test.each([
  ["modern", "modern"],
  ["legacy", "legacy"],
  ["modern", "legacy"],
] as const)(
  "two %s/%s SDK clients respect native slots through discovery and replacement",
  async (a, b) => {
    const boundary = await nativeBoundary();
    const slots = [clientSlot(a, boundary), clientSlot(b, boundary)];
    try {
      await Promise.all(slots.map((slot) => exercise(slot)));
      expect(boundary.sockets.size).toBe(2);
      await slots[0]!.client.close();
      const replacement = clientSlot(a, boundary);
      slots.push(replacement);
      await exercise(replacement);
      expect(boundary.sockets.size).toBe(2);
      await Promise.all(slots.map((slot) => slot.client.close()));
      await drained(boundary);
      await checkWire(boundary, slots.filter((slot) => slot.profile === "modern").length);
    } finally {
      await Promise.all(slots.map((slot) => slot.client.close()));
      for (const socket of boundary.sockets) socket.destroy();
      await new Promise<void>((resolve) => boundary.server.close(() => resolve()));
      await rm(boundary.directory, { recursive: true });
    }
  },
);
