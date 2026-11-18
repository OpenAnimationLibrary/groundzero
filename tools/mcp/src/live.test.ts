import { spawn } from "node:child_process";
import type { ChildProcessWithoutNullStreams } from "node:child_process";
import { once } from "node:events";
import { mkdtemp, readFile, rmdir, unlink } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { afterEach, expect, test } from "vitest";
import { LiveConnection } from "#mcp/live";

const children: ChildProcessWithoutNullStreams[] = [];
const connections: LiveConnection[] = [];
const files: string[] = [];
const directories: string[] = [];
const fixture = fileURLToPath(new URL("../fixtures/live-connector.mjs", import.meta.url));

function connector(mode = "normal", timeout = 2000, record?: string) {
  const child = spawn(process.execPath, [fixture, mode], {
    stdio: ["pipe", "pipe", "pipe"],
    env: { ...process.env, ...(record ? { LIVE_FIXTURE_RECORD: record } : {}) },
  });
  children.push(child);
  child.stderr.resume();
  const connection = new LiveConnection(child, timeout);
  connections.push(connection);
  return { connection, child };
}

afterEach(async () => {
  const closing = children.splice(0).map(async (child) => {
    if (child.exitCode !== null || child.signalCode !== null) return;
    await once(child, "close");
  });
  for (const connection of connections.splice(0)) connection.close();
  await Promise.all(closing);
  for (const file of files.splice(0)) await unlink(file);
  for (const directory of directories.splice(0)) await rmdir(directory);
});

test("authenticates before calls and preserves operation identity across split frames", async () => {
  const { connection } = connector("split");
  await expect(connection.request("live.status")).rejects.toMatchObject({ code: "LIVE_NOT_READY" });
  const hello = await connection.ready;
  expect(hello.mode).toBe("edit");
  const arguments_ = { operation_id: "original-operation", expected_fingerprint: "a".repeat(64) };
  await expect(connection.request("live.stage.set_key_value", arguments_)).resolves.toMatchObject({
    method: "live.stage.set_key_value",
    arguments: arguments_,
  });
});

test.each([
  "invalid-hello",
  "invalid-identity",
  "wrong-limits",
  "no-hello",
  "unversioned-hello",
  "previous-hello",
  "invalid-build",
])("rejects invalid or absent authentication: %s", async (mode) => {
  const { connection } = connector(mode, 300);
  await expect(connection.ready).rejects.toMatchObject({ code: "LIVE_UNAVAILABLE" });
});

test("accepts a versioned hello", async () => {
  const { connection } = connector();
  await expect(connection.ready).resolves.toMatchObject({
    protocol: "groundzero-live-v2",
    build: { application: "groundzero", version: "1.8", executable_sha256: "c".repeat(64) },
  });
});

test.each(["build_mismatch", "protocol_mismatch"])(
  "maps a %s refusal to LIVE_INCOMPATIBLE",
  async (cause) => {
    const { connection } = connector(`refusal-${cause}`);
    await expect(connection.ready).rejects.toMatchObject({
      code: "LIVE_INCOMPATIBLE",
      details: {
        cause,
        connector: { protocol: "groundzero-live-v2" },
        editor: { protocol: "groundzero-live-v1", build: null },
        recovery: "Start the editor and the adapter from one build.",
      },
    });
  },
);

test.each([
  "authentication_failed",
  "live_unavailable",
  "invalid_capabilities",
  "live_security",
  "connector_io",
  "invalid_frame",
])("maps a %s refusal to LIVE_UNAVAILABLE with its cause", async (cause) => {
  const { connection } = connector(`refusal-${cause}`);
  await expect(connection.ready).rejects.toMatchObject({
    code: "LIVE_UNAVAILABLE",
    details: { cause, outcome_unknown: false },
  });
});

test.each(["refusal-unknown_code", "oversized-refusal"])(
  "treats a malformed refusal as an invalid frame: %s",
  async (mode) => {
    const { connection } = connector(mode);
    await expect(connection.ready).rejects.toMatchObject({
      code: "LIVE_UNAVAILABLE",
      details: { cause: "invalid_frame" },
    });
  },
);

test("closes on a refusal after the hello", async () => {
  const { connection } = connector("late-refusal");
  await connection.ready;
  await expect(connection.request("live.status")).rejects.toMatchObject({
    code: "LIVE_UNAVAILABLE",
  });
});

test.each(["extra-field", "both-results", "unknown-id", "bad-error", "oversized", "empty-frame"])(
  "closes malformed framed connections without echoing payload secrets: %s",
  async (mode) => {
    const { connection } = connector(mode);
    await connection.ready;
    const result = connection.request("live.status");
    await expect(result).rejects.toMatchObject({ code: "LIVE_UNAVAILABLE" });
    await expect(result).rejects.not.toThrow("must-not-be-logged");
  },
);

test("keeps GUI domain errors usable without disconnecting", async () => {
  const { connection } = connector("error");
  await connection.ready;
  await expect(connection.request("live.inspect")).rejects.toMatchObject({ code: "gui_busy" });
  await expect(connection.request("live.status")).rejects.toMatchObject({ code: "gui_busy" });
});

test("closes after a duplicate receipt frame without replaying the completed operation", async () => {
  const { connection } = connector("duplicate");
  await connection.ready;
  await expect(connection.request("live.receipt")).resolves.toMatchObject({
    method: "live.receipt",
  });
  await expect(connection.request("live.status")).rejects.toMatchObject({
    code: "LIVE_UNAVAILABLE",
  });
});

test("bounds pending work to four and never automatically replays a timed-out edit", async () => {
  const directory = await mkdtemp(join(tmpdir(), "groundzero-live-"));
  directories.push(directory);
  const record = join(directory, "requests.ndjson");
  files.push(record);
  const { connection } = connector("hold", 1000, record);
  await connection.ready;
  const pending = Array.from({ length: 4 }, () => connection.request("live.stage.set_key_value"));
  const outcomes = Promise.allSettled(pending);
  await expect(connection.request("live.status")).rejects.toMatchObject({ code: "resource_limit" });
  for (const outcome of await outcomes) {
    expect(outcome.status).toBe("rejected");
    if (outcome.status === "rejected") {
      expect(outcome.reason).toMatchObject({
        code: "LIVE_UNAVAILABLE",
        details: { outcome_unknown: true },
      });
    }
  }
  expect((await readFile(record, "utf8")).trim().split("\n")).toHaveLength(4);
});

test("rejects oversized requests and detached tool names without closing the live connection", async () => {
  const { connection } = connector();
  await connection.ready;
  await expect(
    connection.request("live.inspect", { value: "x".repeat(65536) }),
  ).rejects.toMatchObject({ code: "resource_limit" });
  await expect(connection.request("scene.create")).rejects.toMatchObject({ code: "UNKNOWN_TOOL" });
  await expect(connection.request("live.status")).resolves.toMatchObject({ method: "live.status" });
});

test("accepts a final complete receipt before connector process exit", async () => {
  const { connection, child } = connector("final");
  await connection.ready;
  child.stdout.pause();
  const exited = once(child, "exit");
  const pending = connection.request("live.receipt", { operation_id: "committed-before-close" });
  const outcome = pending.then(
    (value) => ({ value }),
    (error: unknown) => ({ error }),
  );
  await exited;
  child.stdout.resume();
  expect(await outcome).toMatchObject({
    value: {
      arguments: { operation_id: "committed-before-close" },
      padding: "x".repeat(2048),
    },
  });
});
