import { spawn } from "node:child_process";
import type { ChildProcess } from "node:child_process";
import { EventEmitter, once } from "node:events";
import { PassThrough } from "node:stream";
import { fileURLToPath } from "node:url";
import { setTimeout as pause } from "node:timers/promises";
import { afterEach, expect, test, vi } from "vitest";
import { processStart } from "#mcp/process-identity";
import type { StartProbe } from "#mcp/process-identity";
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

// Stands in for the supervisor process at the signalling boundary: the PID it
// had is free for reuse once it has exited, so nothing may signal it after.
class ExitedChild extends EventEmitter {
  readonly stdin = new PassThrough();
  readonly stdout = new PassThrough();
  readonly stderr = new PassThrough();
  readonly pid = 4242;
  exitCode: number | null = null;
  signalCode: NodeJS.Signals | null = null;
  connected = true;
  readonly kill = vi.fn(() => true);
  readonly send = vi.fn();

  exit(): void {
    this.exitCode = 0;
    this.connected = false;
    this.emit("exit", 0, null);
  }
}

test.each([true, false])(
  "never signals a worker that exited during its close grace (supervised %s)",
  (supervised) => {
    vi.useFakeTimers();
    try {
      const child = new ExitedChild();
      const connection = new WorkerConnection(
        child as unknown as ChildProcess,
        1_000,
        supervised,
        () => "started",
      );
      connection.close();
      child.exit();
      vi.advanceTimersByTime(10_000);
      expect(child.kill).not.toHaveBeenCalled();
      expect(child.send).not.toHaveBeenCalled();
    } finally {
      vi.useRealTimers();
    }
  },
);

test.each([true, false])(
  "never signals a worker whose request timed out after it exited (supervised %s)",
  async (supervised) => {
    vi.useFakeTimers();
    try {
      const child = new ExitedChild();
      const connection = new WorkerConnection(
        child as unknown as ChildProcess,
        1_000,
        supervised,
        () => "started",
      );
      const pending = connection.request("read").catch((error: unknown) => error);
      child.exit();
      vi.advanceTimersByTime(10_000);
      expect(await pending).toMatchObject({ code: "WORKER_UNAVAILABLE" });
      expect(child.kill).not.toHaveBeenCalled();
      expect(child.send).not.toHaveBeenCalled();
    } finally {
      vi.useRealTimers();
    }
  },
);

function alive(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
}

// A real child: the adapter signals it while its ID and start time are the ones recorded,
// and leaves alone a process that holds the same ID with another start time.
test.skipIf(process.platform === "win32").each([false, true])(
  "signals a timed-out worker only while its ID names the started process (recycled %s)",
  async (recycled) => {
    let reused = false;
    const probe: StartProbe = (pid) => (reused ? "a later process" : processStart(pid));
    const child = spawn("/bin/sleep", ["30"], { stdio: ["pipe", "pipe", "pipe"] });
    const closed = once(child, "close");
    const connection = new WorkerConnection(child, 200, false, probe);
    reused = recycled;
    await expect(connection.request("read")).rejects.toMatchObject({ code: "WORKER_UNAVAILABLE" });
    await pause(500);
    expect(alive(child.pid ?? 0)).toBe(recycled);
    if (recycled) child.kill("SIGKILL");
    const [, signal] = await closed;
    expect(signal).toBe(recycled ? "SIGKILL" : "SIGTERM");
  },
);
