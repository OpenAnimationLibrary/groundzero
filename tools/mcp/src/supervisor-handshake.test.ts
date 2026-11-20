import { spawn } from "node:child_process";
import type { ChildProcess } from "node:child_process";
import { once } from "node:events";
import { fileURLToPath } from "node:url";
import { afterEach, expect, test, vi } from "vitest";
import { superviseNativeChannel } from "#mcp/supervisor-handshake";
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
    {
      detached: true,
      stdio: ["pipe", "pipe", "pipe", "ipc"],
      env: { ...process.env, GROUNDZERO_SUPERVISOR_HANDSHAKE_MS: "2000" },
    },
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
  if (mode === "silent-echo") {
    expect(Buffer.concat(diagnostics).toString()).toMatch(
      /handshake timed out after \d+ ms \(limit 2000 ms\)/u,
    );
  }
});

posix("says a handshake answer that arrives after the limit is a timeout", async () => {
  const { PassThrough, Duplex } = await import("node:stream");
  const toWorker = new PassThrough();
  const fromWorker = new PassThrough();
  const channel = Duplex.from({ readable: fromWorker, writable: toWorker });
  const child = {
    stdio: [null, null, null, channel],
    pid: process.pid,
    exitCode: null,
    signalCode: null,
    once: () => child,
  } as unknown as ChildProcess;
  const stop = vi.fn();
  const diagnostics: string[] = [];
  const write = vi.spyOn(process.stderr, "write").mockImplementation((text) => {
    diagnostics.push(String(text));
    return true;
  });
  const clock = vi.spyOn(performance, "now");
  clock.mockReturnValue(1_000);
  try {
    superviseNativeChannel(child, stop, 5_000);
    const [frame] = (await once(toWorker, "data")) as [Buffer];
    clock.mockReturnValue(1_000 + 7_400);
    fromWorker.write(frame);
    await vi.waitFor(() => expect(stop).toHaveBeenCalled());
  } finally {
    clock.mockRestore();
    write.mockRestore();
  }
  expect(diagnostics.join("")).toBe(
    "Native supervisor handshake timed out after 7400 ms (limit 5000 ms); " +
      "the worker did not answer in time\n",
  );
});
