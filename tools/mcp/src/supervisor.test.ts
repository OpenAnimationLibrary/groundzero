import { spawn } from "node:child_process";
import type { ChildProcess } from "node:child_process";
import { once } from "node:events";
import { connect } from "node:net";
import { fileURLToPath } from "node:url";
import { afterEach, expect, test } from "vitest";
import { isObject } from "#mcp/errors";
import { WorkerConnection } from "#mcp/worker";

const supervisors: ChildProcess[] = [];
const posix = test.skipIf(process.platform === "win32");

function start(timeoutMs = 5_000): { child: ChildProcess; connection: WorkerConnection } {
  const child = spawn(
    process.execPath,
    [
      fileURLToPath(new URL("../dist/supervisor.js", import.meta.url)),
      process.execPath,
      fileURLToPath(new URL("../fixtures/process-tree.mjs", import.meta.url)),
    ],
    { detached: true, stdio: ["pipe", "pipe", "pipe", "ipc"] },
  );
  supervisors.push(child);
  return { child, connection: new WorkerConnection(child, timeoutMs, true) };
}

async function descendantPort(connection: WorkerConnection): Promise<number> {
  const identity = await connection.request("ready");
  if (!isObject(identity) || typeof identity["port"] !== "number") {
    throw new Error("Fixture did not report its descendant listener");
  }
  return identity["port"];
}

function listening(port: number): Promise<boolean> {
  return new Promise((resolve) => {
    const socket = connect({ host: "127.0.0.1", port });
    socket.once("connect", () => {
      socket.destroy();
      resolve(true);
    });
    socket.once("error", () => resolve(false));
  });
}

afterEach(async () => {
  for (const child of supervisors.splice(0)) {
    if (child.exitCode !== null || child.signalCode !== null) continue;
    const closed = once(child, "close");
    child.kill("SIGTERM");
    await closed;
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
  const closed = once(child, "close");
  child.disconnect();
  await closed;
  expect(await listening(port)).toBe(false);
  await expect(connection.request("ready")).rejects.toMatchObject({ code: "WORKER_UNAVAILABLE" });
});

posix("invalid private controls fail closed without exposing another operation", async () => {
  const { child, connection } = start();
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
