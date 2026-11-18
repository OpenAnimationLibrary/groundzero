import { Client } from "@modelcontextprotocol/client";
import { StdioClientTransport } from "@modelcontextprotocol/client/stdio";
import { createHash } from "node:crypto";
import { lstat, mkdtemp, readFile, realpath, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { isAbsolute, join } from "node:path";
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

async function colorCard(client: Client, documentId: unknown, revision: unknown, frames = 1) {
  const effect = await call(client, "fx.create", {
    document_id: documentId,
    expected_revision: revision,
    operation_id: "create-card",
    type: "colorCardFx",
    frame_count: frames,
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

async function completedJob(client: Client, job: Record<string, unknown>, timeoutMs = 30_000) {
  let status = job;
  const deadline = Date.now() + timeoutMs;
  while (!["completed", "failed", "canceled"].includes(String(status["status"]))) {
    if (Date.now() > deadline) throw new Error(`Render job timed out: ${JSON.stringify(status)}`);
    await setTimeout(50);
    status = await call(client, "job.status", { job_id: job["job_id"] });
  }
  expect(status["status"], JSON.stringify(status)).toBe("completed");
  expect(status["writers_closed"]).toBe(true);
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
  await completedJob(client, job);
  const output = await call(client, "job.result", { job_id: job["job_id"] });
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

async function checkMovieFile(workspace: string, artifact: Record<string, unknown>): Promise<void> {
  const relative = artifact["workspace_relative_path"];
  if (typeof relative !== "string") throw new Error("Movie has no workspace-relative path");
  expect(isAbsolute(relative)).toBe(false);
  expect(relative).toBe(`renders/${String(artifact["job_id"])}/movie.mp4`);
  expect(relative.split(/[\\/]/)).not.toContain("..");
  const path = join(workspace, relative);
  expect(await realpath(path)).toBe(path);
  const info = await lstat(path);
  expect(info.isSymbolicLink()).toBe(false);
  expect(info.isFile()).toBe(true);
  expect(info.size).toBeGreaterThan(0);
  expect(info.size).toBeLessThanOrEqual(128 * 1024 * 1024);
  const bytes = await readFile(path);
  expect(bytes.length).toBe(info.size);
  expect(artifact["size_bytes"]).toBe(bytes.length);
  expect(createHash("sha256").update(bytes).digest("hex")).toBe(artifact["sha256"]);
  expect(bytes.toString("ascii", 4, 8)).toBe("ftyp");
}

async function movie(client: Client, documentId: unknown, revision: unknown, workspace: string) {
  const job = await call(client, "render.submit", {
    document_id: documentId,
    expected_revision: revision,
    operation_id: "movie",
    format: "mp4",
    start_frame: 0,
    end_frame: 2,
    width: previewSize,
    height: previewSize,
  });
  await completedJob(client, job, 60_000);
  const output = await client.callTool({
    name: "job.result",
    arguments: { job_id: job["job_id"] },
  });
  expect(output.isError, JSON.stringify(output)).not.toBe(true);
  expect(output.content.every((content) => content.type === "text")).toBe(true);
  const metadata = output.structuredContent;
  if (!isObject(metadata)) throw new Error("Movie artifact result lacks structured metadata");
  const artifacts = metadata["artifacts"];
  if (!Array.isArray(artifacts) || artifacts.length !== 1 || !isObject(artifacts[0])) {
    throw new Error("Completed MP4 job did not publish exactly one movie artifact");
  }
  const artifact = artifacts[0];
  expect(artifact).toMatchObject({
    kind: "movie",
    mime_type: "video/mp4",
    width: previewSize,
    height: previewSize,
    frame_count: 3,
    frame_rate: 24,
    source_frame_rate: 24,
    start_frame: 0,
    end_frame: 2,
    step: 1,
    pixel_format: "yuv420p",
    audio: false,
    alpha: "opaque",
    matte: "black",
    container: "mp4",
    job_id: job["job_id"],
    document_id: documentId,
    source_revision: revision,
    codec: expect.any(String),
    major_brand: expect.any(String),
  });
  expect(artifact["codec"]).not.toBe("");
  expect(artifact["major_brand"]).not.toBe("");
  expect(artifact["duration_seconds"]).toBeCloseTo(3 / 24, 6);
  expect(artifact).not.toHaveProperty("image");
  expect(artifact).not.toHaveProperty("data");
  await checkMovieFile(workspace, artifact);
  const inline = await client.callTool({
    name: "artifact.read",
    arguments: { artifact_id: artifact["artifact_id"] },
  });
  expect(inline.isError).toBe(true);
  expect(inline.structuredContent).toMatchObject({ error: { code: "unsupported_artifact_kind" } });
  expect(inline.content.every((content) => content.type === "text")).toBe(true);
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

test("delivers an MP4 file and verified metadata through actual MCP stdio", async () => {
  const executable = process.env["GROUNDZERO_EXECUTABLE"];
  const media = process.env["GROUNDZERO_MEDIA_TOOLS"];
  if (!executable || !media) {
    throw new Error("test:native requires GROUNDZERO_EXECUTABLE and GROUNDZERO_MEDIA_TOOLS");
  }
  const workspace = await realpath(await mkdtemp(join(tmpdir(), "groundzero-mcp-movie-")));
  const client = new Client(
    { name: "groundzero-movie-test", version: "1.0.0" },
    { versionNegotiation: { mode: { pin: "2026-07-28" } } },
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
          "--media-tools",
          media,
        ],
        env: environment(),
        stderr: "inherit",
      }),
    );
    const capabilities = await call(client, "render.capabilities", {});
    expect(capabilities["profiles"]).toContain("opaque8-mp4");
    expect(capabilities["mp4"]).toMatchObject({
      media_tools_configured: true,
      writer_registered: true,
    });
    const created = await call(client, "scene.create", {
      name: "MCP movie fixture",
      operation_id: "create",
      width_px: previewSize,
      height_px: previewSize,
      frame_rate: 24,
    });
    const revision = await colorCard(client, created["document_id"], created["revision"], 3);
    await movie(client, created["document_id"], revision, workspace);
    const summary = await call(client, "scene.summary", { document_id: created["document_id"] });
    expect(summary["revision"]).toBe(revision);
  } finally {
    await client.close();
    await rm(workspace, { recursive: true, force: true });
  }
}, 120_000);

function productionItems(data: Record<string, unknown>): Record<string, unknown>[] {
  const values = data["items"];
  if (!Array.isArray(values)) throw new Error("Production read has no item array");
  return values.map((value) => {
    if (!isObject(value)) throw new Error("Production read contains a non-object item");
    return value;
  });
}

async function productionScene(client: Client): Promise<Record<string, unknown>> {
  let state = await call(client, "scene.create", {
    name: "Production reads",
    operation_id: "production-create",
  });
  for (const name of ["First", "Second"]) {
    state = await call(client, "level.create", {
      document_id: state["document_id"],
      expected_revision: state["revision"],
      operation_id: `production-${name}`,
      name,
      format: "vector",
    });
  }
  return state;
}

async function checkProductionReads(client: Client, state: Record<string, unknown>): Promise<void> {
  const args = { document_id: state["document_id"], expected_revision: state["revision"] };
  const before = await call(client, "scene.summary", { document_id: args.document_id });
  const first = await call(client, "cast.list", { ...args, limit: 1 });
  const second = await call(client, "cast.list", { ...args, offset: 1, limit: 1 });
  expect(productionItems(first)[0]).toMatchObject({
    name: "First",
    format: "vector",
    library: null,
  });
  expect(productionItems(second)[0]).toMatchObject({ name: "Second", format: "vector" });
  expect(first["next_offset"]).toBe(1);
  expect(second["next_offset"]).toBeNull();
  for (const name of ["cast.list", "cast.usage", "scene.dependencies", "scene.validate"]) {
    const result = await call(client, name, args);
    expect(result).toMatchObject({
      source_revision: state["revision"],
      scope: "root_sheet",
      traversal_complete: true,
      limits: { scanned_cells: 100_000, response_bytes: 1_048_576 },
    });
    const stale = await client.callTool({
      name,
      arguments: { ...args, expected_revision: "stale" },
    });
    expect(stale.isError).toBe(true);
    expect(stale.structuredContent).toMatchObject({ error: { code: "stale_revision" } });
    const invalid = await client.callTool({ name, arguments: { document_id: args.document_id } });
    expect(invalid.isError).toBe(true);
    expect(invalid.structuredContent).toMatchObject({ error: { code: "INVALID_ARGUMENTS" } });
    const unicode = await client.callTool({
      name,
      arguments: { ...args, expected_revision: "🚀".repeat(128) },
    });
    expect(unicode.isError).toBe(true);
    expect(unicode.structuredContent).toMatchObject({ error: { code: "stale_revision" } });
    const tooLong = await client.callTool({
      name,
      arguments: { ...args, expected_revision: "🚀".repeat(129) },
    });
    expect(tooLong.isError).toBe(true);
    expect(tooLong.structuredContent).toMatchObject({ error: { code: "INVALID_ARGUMENTS" } });
  }
  expect(await call(client, "scene.summary", { document_id: args.document_id })).toEqual(before);
}

test("keeps production reads revision-bound through actual MCP stdio", async () => {
  const executable = process.env["GROUNDZERO_EXECUTABLE"];
  if (!executable) throw new Error("test:native requires GROUNDZERO_EXECUTABLE");
  const workspace = await realpath(await mkdtemp(join(tmpdir(), "groundzero-mcp-production-")));
  const client = new Client(
    { name: "groundzero-production-test", version: "1.0.0" },
    { versionNegotiation: { mode: { pin: "2026-07-28" } } },
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
    for (const name of ["cast.list", "cast.usage", "scene.dependencies", "scene.validate"]) {
      const tool = catalog.tools.find((candidate) => candidate.name === name);
      expect(tool?.inputSchema).toMatchObject({
        required: ["document_id", "expected_revision"],
        additionalProperties: false,
        properties: { limit: { minimum: 1, maximum: 128 }, offset: { maximum: 4096 } },
      });
      expect(tool?.annotations).toMatchObject({ readOnlyHint: true, destructiveHint: false });
    }
    await checkProductionReads(client, await productionScene(client));
  } finally {
    try {
      await client.close();
    } finally {
      await rm(workspace, { recursive: true, force: true });
    }
  }
}, 30_000);

test("preserves Unicode operation ID boundaries and retry receipts", async () => {
  const executable = process.env["GROUNDZERO_EXECUTABLE"];
  if (!executable) throw new Error("test:native requires GROUNDZERO_EXECUTABLE");
  const workspace = await realpath(await mkdtemp(join(tmpdir(), "groundzero-mcp-unicode-id-")));
  const client = new Client({ name: "unicode-operation-test", version: "1.0.0" });
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
    const args = {
      operation_id: "🚀".repeat(128),
      name: "Unicode receipt",
      width_px: 32,
      height_px: 32,
      frame_rate: 24,
    };
    const created = await call(client, "scene.create", args);
    expect(created["operation_id"]).toBe(args.operation_id);
    expect(await call(client, "scene.create", args)).toEqual(created);
    const before = await call(client, "scene.summary", { document_id: created["document_id"] });
    for (const [arguments_, code] of [
      [{ ...args, name: "Conflict" }, "operation_conflict"],
      [{ ...args, operation_id: "🚀".repeat(129) }, "INVALID_ARGUMENTS"],
    ] as const) {
      const rejected = await client.callTool({ name: "scene.create", arguments: arguments_ });
      expect(rejected.isError).toBe(true);
      expect(rejected.structuredContent).toMatchObject({ error: { code } });
    }
    expect(
      await call(client, "scene.summary", {
        document_id: created["document_id"],
      }),
    ).toEqual(before);
  } finally {
    try {
      await client.close();
    } finally {
      await rm(workspace, { recursive: true, force: true });
    }
  }
}, 30_000);

async function rigJourney(client: Client): Promise<void> {
  const scene = await call(client, "scene.create", { name: "Rig", operation_id: "rig-scene" });
  const document = scene["document_id"];
  const peg = await call(client, "object.create", {
    document_id: document,
    expected_revision: scene["revision"],
    operation_id: "rig-peg",
    name: "Peg",
    kind: "pegbar",
  });
  const layerArgs = {
    document_id: document,
    expected_revision: peg["revision"],
    operation_id: "rig-layer",
    object_id: peg["object_id"],
    name: "Breath",
  };
  const created = await call(client, "offset_layer.create", layerArgs);
  expect(await call(client, "offset_layer.create", layerArgs)).toEqual(created);
  const layer = created["layer"];
  if (!isObject(layer)) throw new Error("offset_layer.create returned no layer record");
  const keyed = await call(client, "animation.set_keys", {
    document_id: document,
    expected_revision: created["revision"],
    operation_id: "rig-scale",
    object_id: peg["object_id"],
    layer_id: layer["layer_id"],
    channel: "scale",
    unit: "scalar",
    keys: [{ frame: 0, value: 0.5, interpolation: "constant" }],
  });
  expect(keyed["value_space"]).toBe("offset_layer_curve");
  const evaluated = await call(client, "rig.evaluate", {
    document_id: document,
    expected_revision: keyed["revision"],
    object_ids: [peg["object_id"]],
    frames: [0],
  });
  expect(evaluated["samples"]).toMatchObject([
    {
      frame: 0,
      placement: { a11: 1.5, a22: 1.5, dx_inch: 0, dy_inch: 0 },
      channels: expect.arrayContaining([
        { channel: "scale", unit: "scalar", base: 1, offset: 0.5, effective: 1.5 },
      ]),
    },
  ]);
  const base = { document_id: document, expected_revision: keyed["revision"] };
  for (const [name, args, code] of [
    ["offset_layer.create", { ...layerArgs, ...base, operation_id: "rig-again" }, "name_conflict"],
    ["rig.evaluate", { ...base, object_ids: [peg["object_id"]], frames: [0, 0] }, "invalid_params"],
    ["rig.evaluate", { ...base, object_ids: [], frames: [0] }, "INVALID_ARGUMENTS"],
  ] as const) {
    const rejected = await client.callTool({ name, arguments: args });
    expect(rejected.isError).toBe(true);
    expect(rejected.structuredContent).toMatchObject({ error: { code } });
  }
  const summary = await call(client, "scene.summary", { document_id: document });
  expect(summary["revision"]).toBe(keyed["revision"]);
}

test("drives a rig journey in the rigging toolset through actual MCP stdio", async () => {
  const executable = process.env["GROUNDZERO_EXECUTABLE"];
  if (!executable) throw new Error("test:native requires GROUNDZERO_EXECUTABLE");
  const workspace = await realpath(await mkdtemp(join(tmpdir(), "groundzero-mcp-rig-")));
  const client = new Client(
    { name: "groundzero-rig-test", version: "1.0.0" },
    { versionNegotiation: { mode: { pin: "2026-07-28" } } },
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
          "--toolsets",
          "rigging",
        ],
        env: environment(),
        stderr: "inherit",
      }),
    );
    const catalog = await client.listTools();
    const evaluate = catalog.tools.find((tool) => tool.name === "rig.evaluate");
    expect(evaluate?.inputSchema).toMatchObject({
      required: ["document_id", "expected_revision", "object_ids", "frames"],
      additionalProperties: false,
      properties: { object_ids: { minItems: 1, maxItems: 64 }, frames: { maxItems: 64 } },
    });
    expect(evaluate?.annotations).toMatchObject({ readOnlyHint: true, destructiveHint: false });
    const parent = catalog.tools.find((tool) => tool.name === "object.parent");
    expect(parent?.inputSchema).toMatchObject({
      required: expect.arrayContaining(["placement_policy"]),
      properties: { placement_policy: { const: "preserve_local" } },
    });
    await rigJourney(client);
  } finally {
    try {
      await client.close();
    } finally {
      await rm(workspace, { recursive: true, force: true });
    }
  }
}, 30_000);

// tools/list before the effect declarations, plus 5 %: growth past it is a decision.
const LISTING_BUDGET_BYTES = { core: 94_380, all: 126_036 };

test.each(["core", "all"] as const)(
  "keeps the %s tools/list within its byte budget and free of declarations",
  async (toolsets) => {
    const executable = process.env["GROUNDZERO_EXECUTABLE"];
    if (!executable) throw new Error("test:native requires GROUNDZERO_EXECUTABLE");
    const workspace = await realpath(await mkdtemp(join(tmpdir(), "groundzero-mcp-listing-")));
    const client = new Client({ name: "groundzero-listing-test", version: "1.0.0" });
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
            "--toolsets",
            toolsets,
          ],
          env: environment(),
          stderr: "inherit",
        }),
      );
      const { tools } = await client.listTools();
      for (const tool of tools) {
        expect(Object.keys(tool).sort()).toEqual([
          "annotations",
          "description",
          "inputSchema",
          "name",
        ]);
      }
      const bytes = Buffer.byteLength(JSON.stringify({ tools }));
      expect(bytes).toBeLessThanOrEqual(LISTING_BUDGET_BYTES[toolsets]);
    } finally {
      await client.close();
      await rm(workspace, { recursive: true, force: true });
    }
  },
);
