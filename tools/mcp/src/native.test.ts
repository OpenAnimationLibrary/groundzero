import { Client } from "@modelcontextprotocol/client";
import { StdioClientTransport } from "@modelcontextprotocol/client/stdio";
import { mkdtemp, realpath, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout } from "node:timers/promises";
import { fileURLToPath } from "node:url";
import { expect, test } from "vitest";
import { isObject } from "#mcp/errors";

function environment(): Record<string, string> {
  const result: Record<string, string> = {};
  for (const [name, value] of Object.entries(process.env)) {
    if (value !== undefined) result[name] = value;
  }
  return result;
}

async function call(client: Client, name: string, args: Record<string, unknown>) {
  const result = await client.callTool({ name, arguments: args });
  expect(result.isError, JSON.stringify(result)).not.toBe(true);
  const data = result.structuredContent;
  if (!isObject(data)) throw new Error(`Missing structured result for ${name}`);
  return data;
}

async function preview(client: Client, documentId: unknown, revision: unknown): Promise<void> {
  const job = await call(client, "render.preview", {
    document_id: documentId,
    expected_revision: revision,
    operation_id: "preview",
    frame: 0,
    width: 32,
    height: 32,
  });
  let status = job;
  const deadline = Date.now() + 30_000;
  while (!["completed", "failed", "canceled"].includes(String(status["status"]))) {
    if (Date.now() > deadline) throw new Error(`Preview timed out: ${JSON.stringify(status)}`);
    await setTimeout(50);
    status = await call(client, "render.status", { job_id: job["job_id"] });
  }
  expect(status["status"], JSON.stringify(status)).toBe("completed");
  expect(status["writers_closed"]).toBe(true);
  const output = await call(client, "render.artifacts", { job_id: job["job_id"] });
  const artifacts = output["artifacts"];
  if (!Array.isArray(artifacts) || !isObject(artifacts[0])) {
    throw new Error("Completed preview did not publish an artifact");
  }
  const result = await client.callTool({
    name: "artifact.read",
    arguments: { artifact_id: artifacts[0]["artifact_id"] },
  });
  expect(result.isError, JSON.stringify(result)).not.toBe(true);
  expect(result.structuredContent).toMatchObject({
    source_revision: revision,
    width: 32,
    height: 32,
  });
  expect(result.content).toEqual(
    expect.arrayContaining([expect.objectContaining({ type: "image", mimeType: "image/png" })]),
  );
}

test.each(["legacy", "2026-07-28"])(
  "drives the native worker through actual MCP stdio (%s)",
  async (protocol) => {
    const executable = process.env["GROUNDZERO_EXECUTABLE"];
    if (!executable) throw new Error("test:native requires GROUNDZERO_EXECUTABLE");
    const workspace = await realpath(await mkdtemp(join(tmpdir(), "groundzero-mcp-native-")));
    const mode = protocol === "legacy" ? "legacy" : { pin: protocol };
    const client = new Client(
      { name: "groundzero-native-test", version: "1.0.0" },
      { versionNegotiation: { mode } },
    );
    try {
      await client.connect(
        new StdioClientTransport({
          command: process.execPath,
          args: [
            fileURLToPath(new URL("../dist/cli.js", import.meta.url)),
            "--executable",
            executable,
            "--workspace",
            workspace,
          ],
          env: environment(),
          stderr: "inherit",
        }),
      );
      const catalog = await client.listTools();
      expect(catalog.tools.map((tool) => tool.name)).toEqual(
        expect.arrayContaining(["scene.create", "scene.summary", "artifact.read"]),
      );
      const created = await call(client, "scene.create", {
        name: "MCP fixture",
        operation_id: "create",
        width_px: 32,
        height_px: 32,
      });
      const summary = await call(client, "scene.summary", { document_id: created["document_id"] });
      expect(summary["revision"]).toBe(created["revision"]);
      expect(summary["mode"]).toBe("detached");
      const conflict = await client.callTool({
        name: "camera.update",
        arguments: {
          document_id: created["document_id"],
          expected_revision: "stale",
          operation_id: "stale-camera",
          width_px: 64,
        },
      });
      expect(conflict.isError).toBe(true);
      expect(conflict.structuredContent).toMatchObject({ error: { code: "revision_conflict" } });
      await preview(client, created["document_id"], created["revision"]);
      const discarded = await call(client, "scene.discard", {
        document_id: created["document_id"],
        expected_revision: created["revision"],
        operation_id: "discard",
      });
      expect(discarded["discarded"]).toBe(true);
    } finally {
      await client.close();
      await rm(workspace, { recursive: true, force: true });
    }
  },
  90_000,
);
