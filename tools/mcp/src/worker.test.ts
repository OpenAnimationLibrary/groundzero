import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { afterEach, expect, test } from "vitest";
import { WorkerConnection } from "#mcp/worker";

const connections: WorkerConnection[] = [];

function connect(mode: string, timeoutMs = 5_000): WorkerConnection {
  const child = spawn(process.execPath, [
    fileURLToPath(new URL("../fixtures/worker.mjs", import.meta.url)),
    mode,
  ]);
  const connection = new WorkerConnection(child, timeoutMs);
  connections.push(connection);
  return connection;
}

afterEach(() => {
  for (const connection of connections.splice(0)) connection.close();
});

test("reassembles split UTF-8 responses and correlates concurrent requests", async () => {
  const connection = connect("split");
  const results = await Promise.all([
    connection.request("read", { label: "灯光" }),
    connection.request("read", { label: "second" }),
  ]);
  expect(results).toEqual([{ value: { label: "灯光" } }, { value: { label: "second" } }]);
});

test("preserves structured native failure details and keeps the connection usable", async () => {
  const connection = connect("error");
  await expect(connection.request("write")).rejects.toMatchObject({
    code: "REVISION_CONFLICT",
    details: { revision: 7 },
  });
  await expect(connection.request("write")).rejects.toMatchObject({ code: "REVISION_CONFLICT" });
});

test.each(["malformed", "utf8", "oversized", "unknown", "ambiguous", "exit"])(
  "fails closed after %s native output and never replays writes",
  async (mode) => {
    const connection = connect(mode);
    await expect(connection.request("write", { operation_id: "one" })).rejects.toMatchObject({
      code: "WORKER_UNAVAILABLE",
      details: { outcome_unknown: true },
    });
    await expect(connection.request("read")).rejects.toMatchObject({ code: "WORKER_UNAVAILABLE" });
  },
);

test("times out a hung worker and rejects subsequent requests", async () => {
  const connection = connect("stall", 200);
  await expect(connection.request("write")).rejects.toMatchObject({ code: "WORKER_UNAVAILABLE" });
  await expect(connection.request("read")).rejects.toMatchObject({ code: "WORKER_UNAVAILABLE" });
});

test("rejects an oversized request without poisoning a usable connection", async () => {
  const connection = connect("echo");
  const oversized = connection.request("write", { value: "x".repeat(256 * 1024) });
  await expect(oversized).rejects.toMatchObject({ code: "LIMIT_EXCEEDED" });
  await expect(connection.request("read", { value: 7 })).resolves.toEqual({ value: { value: 7 } });
});

test("bounds pending operations and rejects all of them on disconnect", async () => {
  const connection = connect("stall");
  const pending = Promise.allSettled(Array.from({ length: 16 }, () => connection.request("read")));
  await expect(connection.request("read")).rejects.toMatchObject({ code: "BUSY" });
  connection.close();
  expect((await pending).every((result) => result.status === "rejected")).toBe(true);
});
