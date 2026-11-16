import { Client } from "@modelcontextprotocol/client";
import { StdioClientTransport } from "@modelcontextprotocol/client/stdio";
import { createHash } from "node:crypto";
import { mkdtemp, realpath, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout } from "node:timers/promises";
import { fileURLToPath } from "node:url";
import { crc32, inflateSync } from "node:zlib";
import { expect, test } from "vitest";
import { isObject } from "#mcp/errors";

const previewSize = 32;
const previewColor = [24, 72, 192, 255];

function paeth(left: number, above: number, diagonal: number): number {
  const prediction = left + above - diagonal;
  const distances = [Math.abs(prediction - left), Math.abs(prediction - above)];
  const closest = Math.min(...distances, Math.abs(prediction - diagonal));
  if (distances[0] === closest) return left;
  if (distances[1] === closest) return above;
  return diagonal;
}

function unfilterPng(data: Buffer): Buffer {
  const stride = previewSize * 4;
  expect(data.length).toBe(previewSize * (stride + 1));
  const predictors = [
    () => 0,
    (left: number) => left,
    (_left: number, above: number) => above,
    (left: number, above: number) => Math.floor((left + above) / 2),
    paeth,
  ];
  const pixels = Buffer.alloc(previewSize * stride);
  let previous: Buffer = Buffer.alloc(stride);
  for (let y = 0; y < previewSize; y += 1) {
    const start = y * (stride + 1);
    const predictor = predictors[data.readUInt8(start)];
    if (!predictor) throw new Error("Native PNG has an unsupported row filter");
    const row = Buffer.from(data.subarray(start + 1, start + stride + 1));
    for (let index = 0; index < stride; index += 1) {
      const left = index >= 4 ? row.readUInt8(index - 4) : 0;
      const diagonal = index >= 4 ? previous.readUInt8(index - 4) : 0;
      row[index] =
        (row.readUInt8(index) + predictor(left, previous.readUInt8(index), diagonal)) & 255;
    }
    row.copy(pixels, y * stride);
    previous = row;
  }
  return pixels;
}

function decodePreview(bytes: Buffer): Buffer {
  expect(bytes.subarray(0, 8)).toEqual(Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]));
  const compressed: Buffer[] = [];
  let headers = 0;
  let ended = false;
  let offset = 8;
  while (offset < bytes.length) {
    expect(offset + 12).toBeLessThanOrEqual(bytes.length);
    const size = bytes.readUInt32BE(offset);
    const end = offset + 8 + size;
    expect(end + 4).toBeLessThanOrEqual(bytes.length);
    expect(crc32(bytes.subarray(offset + 4, end))).toBe(bytes.readUInt32BE(end));
    const kind = bytes.toString("ascii", offset + 4, offset + 8);
    const content = bytes.subarray(offset + 8, end);
    if (kind === "IHDR") {
      expect(offset).toBe(8);
      expect(content.length).toBe(13);
      expect(content.readUInt32BE(0)).toBe(previewSize);
      expect(content.readUInt32BE(4)).toBe(previewSize);
      expect([...content.subarray(8)]).toEqual([8, 6, 0, 0, 0]);
      headers += 1;
    } else if (kind === "IDAT") {
      compressed.push(content);
    } else if (kind === "IEND") {
      expect(size).toBe(0);
      expect(end + 4).toBe(bytes.length);
      ended = true;
    }
    offset = end + 4;
  }
  expect(headers).toBe(1);
  expect(ended).toBe(true);
  expect(compressed.length).toBeGreaterThan(0);
  const data = inflateSync(Buffer.concat(compressed), {
    maxOutputLength: previewSize * (previewSize * 4 + 1),
  });
  return unfilterPng(data);
}

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

async function colorCard(client: Client, documentId: unknown, revision: unknown) {
  const effect = await call(client, "fx.create", {
    document_id: documentId,
    expected_revision: revision,
    operation_id: "create-card",
    type: "colorCardFx",
    frame_count: 1,
  });
  const colored = await call(client, "fx.parameter.set", {
    document_id: documentId,
    expected_revision: effect["revision"],
    operation_id: "color-card",
    fx_id: effect["fx_id"],
    parameter: "color",
    value: previewColor,
  });
  const connected = await call(client, "fx.terminal.set", {
    document_id: documentId,
    expected_revision: colored["revision"],
    operation_id: "connect-card",
    fx_id: effect["fx_id"],
    enabled: true,
  });
  return connected["revision"];
}

async function preview(client: Client, documentId: unknown, revision: unknown): Promise<void> {
  const job = await call(client, "render.preview", {
    document_id: documentId,
    expected_revision: revision,
    operation_id: "preview",
    frame: 0,
    width: previewSize,
    height: previewSize,
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
    width: previewSize,
    height: previewSize,
  });
  const images = result.content.filter((content) => content.type === "image");
  expect(images.length).toBe(1);
  const image = images[0];
  if (!image) throw new Error("Native artifact did not return MCP image content");
  expect(image.mimeType).toBe("image/png");
  const bytes = Buffer.from(image.data, "base64");
  expect(bytes.toString("base64")).toBe(image.data);
  expect(createHash("sha256").update(bytes).digest("hex")).toBe(artifacts[0]["sha256"]);
  const expected = Buffer.alloc(previewSize * previewSize * 4).fill(Buffer.from(previewColor));
  expect(decodePreview(bytes)).toEqual(expected);
  const missing = await client.callTool({
    name: "artifact.read",
    arguments: { artifact_id: "unowned-artifact" },
  });
  expect(missing.isError).toBe(true);
  expect(missing.structuredContent).toMatchObject({ error: { code: "unknown_artifact" } });
  expect(missing.content.every((content) => content.type === "text")).toBe(true);
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
        expect.arrayContaining(["scene.create", "scene.summary", "fx.create", "artifact.read"]),
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
      const revision = await colorCard(client, created["document_id"], created["revision"]);
      await preview(client, created["document_id"], revision);
      const discarded = await call(client, "scene.discard", {
        document_id: created["document_id"],
        expected_revision: revision,
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
