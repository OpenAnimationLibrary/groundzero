import { spawn } from "node:child_process";
import { once } from "node:events";
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

test("drains a final success response after process exit before failing the connection", async () => {
  const child = spawn(process.execPath, [
    fileURLToPath(new URL("../fixtures/worker.mjs", import.meta.url)),
    "final",
  ]);
  child.stdout.pause();
  const connection = new WorkerConnection(child, 5_000);
  connections.push(connection);
  const exited = once(child, "exit");
  const closed = once(child, "close");
  const response = Promise.allSettled([
    connection.request("write", { operation_id: "final-write" }),
  ]);
  await exited;
  child.stdout.resume();
  await closed;
  expect(await response).toEqual([
    { status: "fulfilled", value: { value: { operation_id: "final-write" } } },
  ]);
  await expect(connection.request("read")).rejects.toMatchObject({
    code: "WORKER_UNAVAILABLE",
    details: { outcome_unknown: false },
  });
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

function spawnFixture(mode: string): ReturnType<typeof spawn> {
  return spawn(process.execPath, [
    fileURLToPath(new URL("../fixtures/worker.mjs", import.meta.url)),
    mode,
  ]);
}

test("close ends the worker input so a worker can exit by itself", async () => {
  const child = spawnFixture("echo");
  const connection = new WorkerConnection(child, 5_000);
  await connection.request("read");
  const exited = once(child, "exit");
  connection.close();
  expect(await exited).toEqual([0, null]);
});

test("close signals a worker that outlives the end of its input by the grace period", async () => {
  const child = spawnFixture("ignore-eof");
  const connection = new WorkerConnection(child, 5_000);
  await connection.request("read");
  const exited = once(child, "exit");
  const started = performance.now();
  connection.close();
  expect(await exited).toEqual([null, "SIGTERM"]);
  expect(performance.now() - started).toBeGreaterThanOrEqual(1_900);
});
