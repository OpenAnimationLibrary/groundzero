import { spawn } from "node:child_process";
import type { ChildProcess } from "node:child_process";
import { once } from "node:events";
import { mkdtemp, readFile, realpath, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout as pause } from "node:timers/promises";
import { connect } from "node:net";
import { fileURLToPath } from "node:url";
import { afterEach, expect, test, vi } from "vitest";
import { isObject } from "#mcp/errors";
import { WorkerConnection } from "#mcp/worker";

const supervisors: { child: ChildProcess; diagnostics: Buffer[]; expected: string }[] = [];
const posix = test.skipIf(process.platform === "win32");

function start(
  timeoutMs = 5_000,
  expected = "",
  mode: string[] = [],
): { child: ChildProcess; connection: WorkerConnection } {
  const child = spawn(
    process.execPath,
    [
      fileURLToPath(new URL("../dist/supervisor.js", import.meta.url)),
      process.execPath,
      fileURLToPath(new URL("../fixtures/process-tree.mjs", import.meta.url)),
      ...mode,
    ],
    { detached: true, stdio: ["pipe", "pipe", "pipe", "ipc"] },
  );
  const diagnostics: Buffer[] = [];
  child.stderr?.on("data", (chunk: Buffer) => diagnostics.push(chunk));
  supervisors.push({ child, diagnostics, expected });
  return { child, connection: new WorkerConnection(child, timeoutMs, true) };
}

async function processTree(
  connection: WorkerConnection,
): Promise<{ port: number; pids: number[] }> {
  const identity = await connection.request("ready");
  if (
    !isObject(identity) ||
    typeof identity["port"] !== "number" ||
    typeof identity["pid"] !== "number" ||
    typeof identity["worker"] !== "number"
  ) {
    throw new Error("Fixture did not report its worker and descendant");
  }
  return { port: identity["port"], pids: [identity["worker"], identity["pid"]] };
}

async function descendantPort(connection: WorkerConnection): Promise<number> {
  return (await processTree(connection)).port;
}

function alive(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
}

async function expectGone(pids: number[]): Promise<void> {
  const deadline = performance.now() + 2_000;
  while (pids.some(alive) && performance.now() < deadline) await pause(20);
  expect(pids.filter(alive)).toEqual([]);
}

function listening(port: number): Promise<boolean> {
  return new Promise((resolve) => {
    const socket = connect({ host: "127.0.0.1", port });
    socket.once("connect", () => {
      socket.end();
      socket.resume();
      resolve(true);
    });
    socket.once("error", () => resolve(false));
  });
}

afterEach(async () => {
  for (const { child, diagnostics, expected } of supervisors.splice(0)) {
    if (child.exitCode === null && child.signalCode === null) {
      const closed = once(child, "close");
      child.kill("SIGTERM");
      await closed;
    }
    expect(Buffer.concat(diagnostics).toString()).toBe(expected);
  }
});

posix("disconnect kills a TERM-ignoring descendant before releasing the group leader", async () => {
  const { child, connection } = start();
  const port = await descendantPort(connection);
  expect(await listening(port)).toBe(true);
  const closed = once(child, "close");
  connection.close();
  await closed;
  expect(child.signalCode).toBe("SIGKILL");
  expect(await listening(port)).toBe(false);
});

posix("a worker that exits at the end of its input is released well within the grace", async () => {
  const { child, connection } = start(5_000, "", ["eof-cleanup"]);
  const { port, pids } = await processTree(connection);
  const nativeExit = once(child, "message");
  const closed = once(child, "close");
  const started = performance.now();
  connection.close();
  expect((await nativeExit)[0]).toEqual({ type: "native-exit", code: 0, signal: null });
  await closed;
  expect(performance.now() - started).toBeLessThan(1_000);
  await expectGone(pids);
  expect(await listening(port)).toBe(false);
});

posix(
  "a worker that ignores the end of its input and SIGTERM is signalled, then killed",
  async () => {
    const { child, connection } = start(5_000, "", ["ignore-term"]);
    const { port, pids } = await processTree(connection);
    const closed = once(child, "close");
    const started = performance.now();
    connection.close();
    await closed;
    expect(performance.now() - started).toBeGreaterThanOrEqual(3_900);
    expect(child.signalCode).toBe("SIGKILL");
    await expectGone(pids);
    expect(await listening(port)).toBe(false);
  },
  15_000,
);

posix("request timeout cleans descendants and reports the write outcome as unknown", async () => {
  const { child, connection } = start(3_000);
  const port = await descendantPort(connection);
  const closed = once(child, "close");
  await expect(connection.request("stall")).rejects.toMatchObject({
    code: "WORKER_UNAVAILABLE",
    details: { outcome_unknown: true },
  });
  await closed;
  expect(await listening(port)).toBe(false);
});

posix("cleanup leaves a different worker group running", async () => {
  const first = start();
  const second = start();
  const firstPort = await descendantPort(first.connection);
  const secondPort = await descendantPort(second.connection);
  const closed = once(first.child, "close");
  first.connection.close();
  await closed;
  expect(await listening(firstPort)).toBe(false);
  expect(await listening(secondPort)).toBe(true);
  await expect(second.connection.request("ready")).resolves.toMatchObject({ port: secondPort });
});

posix("native exit cleans descendants and drains its final protocol response", async () => {
  const { child, connection } = start();
  const port = await descendantPort(connection);
  child.stdout?.pause();
  const nativeExit = once(child, "message");
  const closed = once(child, "close");
  const response = Promise.allSettled([connection.request("final")]);
  expect((await nativeExit)[0]).toMatchObject({ type: "native-exit", code: 0 });
  child.stdout?.resume();
  await closed;
  expect(await response).toEqual([
    { status: "fulfilled", value: expect.objectContaining({ port }) },
  ]);
  expect(await listening(port)).toBe(false);
  await expect(connection.request("ready")).rejects.toMatchObject({
    code: "WORKER_UNAVAILABLE",
    details: { outcome_unknown: false },
  });
});

posix("loss of the parent IPC channel still cleans the whole media group", async () => {
  const { child, connection } = start();
  const port = await descendantPort(connection);
  const { stdout, stderr } = child;
  if (!stdout || !stderr) throw new Error("Supervisor fixture requires private output pipes");
  // Node 22's local disconnect omits IPC from aggregate close accounting:
  // https://github.com/nodejs/node/blob/v22.23.5/lib/internal/child_process.js#L917-L945
  const stopped = Promise.all([once(child, "exit"), once(stdout, "end"), once(stderr, "end")]);
  stderr.resume();
  child.disconnect();
  await stopped;
  expect(child.signalCode).toBe("SIGKILL");
  expect(await listening(port)).toBe(false);
  await expect(connection.request("ready")).rejects.toMatchObject({ code: "WORKER_UNAVAILABLE" });
});

posix("invalid private controls fail closed without exposing another operation", async () => {
  const { child, connection } = start(5_000, "Invalid private supervisor control message\n");
  const port = await descendantPort(connection);
  const closed = once(child, "close");
  child.send({ execute: "anything" });
  await closed;
  expect(await listening(port)).toBe(false);
  await expect(connection.request("ready")).rejects.toMatchObject({ code: "WORKER_UNAVAILABLE" });
});

test("the internal supervisor rejects launches without its private control channel", async () => {
  const child = spawn(process.execPath, [
    fileURLToPath(new URL("../dist/supervisor.js", import.meta.url)),
    process.execPath,
  ]);
  const diagnostics: Buffer[] = [];
  child.stderr.on("data", (chunk: Buffer) => diagnostics.push(chunk));
  await once(child, "close");
  expect(child.exitCode).toBe(1);
  expect(Buffer.concat(diagnostics).toString()).toContain("requires a private POSIX launch");
});

function shellWord(value: string): string {
  return "'" + value.replaceAll("'", "'\\''") + "'";
}

async function requireDescendantExit(port: number, pid: number): Promise<void> {
  const deadline = performance.now() + 5_000;
  while (await listening(port)) {
    if (performance.now() >= deadline) {
      process.kill(pid, "SIGKILL");
      throw new Error("The worker left its trusted descendant running after close");
    }
    await pause(25);
  }
}

posix("contains descendants even when no media tools were granted", async () => {
  const workspace = await realpath(await mkdtemp(join(tmpdir(), "groundzero-no-media-")));
  const executable = join(workspace, "native-fixture");
  const fixture = fileURLToPath(new URL("../fixtures/process-tree.mjs", import.meta.url));
  const errorLog = join(workspace, "fixture.stderr.log");
  const script = `#!/bin/bash\nset -euo pipefail\nexec ${shellWord(process.execPath)} ${shellWord(fixture)} "$@" 2>${shellWord(errorLog)}\n`;
  await writeFile(executable, script, { mode: 0o700 });
  const launcher = (await import(
    new URL("../dist/worker.js", import.meta.url).href
  )) as typeof import("#mcp/worker");
  const connection = launcher.launchWorker({
    executable,
    workspace,
    inputRoots: [],
    grants: ["inspect"],
    domains: ["core"],
  });
  let identity: { port: number; pid: number } | undefined;
  try {
    const value = await connection.request("ready");
    if (!isObject(value) || typeof value["port"] !== "number" || typeof value["pid"] !== "number") {
      throw new Error("Fixture did not identify its native descendant");
    }
    identity = { port: value["port"], pid: value["pid"] };
    expect(value["anchor"]).not.toBe(process.pid);
    expect(await listening(identity.port)).toBe(true);
  } finally {
    connection.close();
    try {
      if (identity) await requireDescendantExit(identity.port, identity.pid);
      expect(await readFile(errorLog, "utf8")).toBe("");
    } finally {
      await rm(workspace, { recursive: true, force: true });
    }
  }
});

// The supervisor leads the worker's process group. Once its control channel is gone the
// adapter falls back to a signal through the child handle, and only before the child is
// reaped: an unreaped child's ID, and so its group's ID, cannot have been reused.
posix.each([false, true])(
  "signals the supervisor group leader only before it is reaped (reaped %s)",
  async (reaped) => {
    const { child, connection } = start(5_000);
    await processTree(connection);
    const pid = child.pid ?? 0;
    const kill = vi.spyOn(child, "kill");
    // Node emits no close for a child whose channel the parent disconnected.
    const exited = once(child, "exit");
    process.kill(pid, "SIGSTOP");
    try {
      child.disconnect();
      connection.close();
      if (reaped) {
        process.kill(pid, "SIGKILL");
        await exited;
      }
      await pause(2_500);
      expect(kill.mock.calls).toEqual(reaped ? [] : [["SIGTERM"]]);
    } finally {
      if (!reaped) {
        process.kill(pid, "SIGCONT");
        await exited;
      }
    }
  },
);
