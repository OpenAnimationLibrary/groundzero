import { spawn } from "node:child_process";
import type { ChildProcess } from "node:child_process";
import { once } from "node:events";
import { fileURLToPath } from "node:url";
import { afterEach, expect, test } from "vitest";
import { WorkerConnection } from "#mcp/worker";

const children: ChildProcess[] = [];
const posix = test.skipIf(process.platform === "win32");

afterEach(async () => {
  for (const child of children.splice(0)) {
    if (child.exitCode !== null || child.signalCode !== null) continue;
    const closed = once(child, "close");
    child.kill("SIGTERM");
    await closed;
  }
});

posix.each([
  "wrong-echo",
  "oversized-echo",
  "truncated-echo",
  "silent-echo",
  "close-before-ack",
  "extra-after-ack",
])("refuses %s on the private native channel and closes the group", async (mode) => {
  const child = spawn(
    process.execPath,
    [
      fileURLToPath(new URL("../dist/supervisor.js", import.meta.url)),
      process.execPath,
      fileURLToPath(new URL("../fixtures/supervisor-boundary.mjs", import.meta.url)),
      mode,
    ],
    { detached: true, stdio: ["pipe", "pipe", "pipe", "ipc"] },
  );
  children.push(child);
  const diagnostics: Buffer[] = [];
  child.stderr?.on("data", (chunk: Buffer) => diagnostics.push(chunk));
  const closed = once(child, "close");
  const connection = new WorkerConnection(child, 9_000, true);
  await expect(connection.request("ready")).rejects.toMatchObject({
    code: "WORKER_UNAVAILABLE",
    details: { outcome_unknown: true },
  });
  await closed;
  expect(child.signalCode).toBe("SIGKILL");
  if (mode !== "close-before-ack") {
    expect(Buffer.concat(diagnostics).toString()).toMatch(/handshake|channel/u);
  }
});
