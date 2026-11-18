import { Readable } from "node:stream";
import { expect, test } from "vitest";

const { adapterArguments, bridgeResult, parseRequest, readFrames } = await import(
  new URL("native-client.mjs", import.meta.url).href
);
const png =
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a5xkAAAAASUVORK5CYII=";
const image = { type: "image", mimeType: "image/png", data: png };
const bound = 8 * 1024 * 1024;

test("preserves ordinary structured results independently of their text summary", () => {
  const metadata = { revision: "1", rows: [{ value: 7 }] };
  expect(
    bridgeResult("scene.summary", {
      content: [{ type: "text", text: "summary only" }],
      structuredContent: metadata,
    }),
  ).toEqual({ result: metadata });
});

test("bridges only actual MCP image content and preserves all structured metadata", () => {
  const metadata = { artifact_id: "owned", source_revision: "7", width: 1, height: 1 };
  const response = bridgeResult("artifact.read", {
    content: [{ type: "text", text: "metadata summary" }, image],
    structuredContent: metadata,
  });
  expect(response).toEqual({
    result: { ...metadata, image: { mimeType: "image/png", data: png } },
  });
  expect(metadata).not.toHaveProperty("image");
});

test("bridges a Library thumbnail in the same shape as an artifact image", () => {
  const metadata = { kind: "template", name: "Rig", pose_index: null, width: 1, height: 1 };
  expect(
    bridgeResult("library.thumbnail", {
      content: [{ type: "text", text: "metadata summary" }, image],
      structuredContent: metadata,
    }),
  ).toEqual({ result: { ...metadata, image: { mimeType: "image/png", data: png } } });
  expect(() =>
    bridgeResult("library.thumbnail", {
      content: [{ type: "text", text: "no image" }],
      structuredContent: metadata,
    }),
  ).toThrow("must return one MCP ImageContent");
  expect(() =>
    bridgeResult("library.inspect", {
      content: [{ type: "text", text: "summary" }, image],
      structuredContent: metadata,
    }),
  ).toThrow("declares none");
});

test("preserves native tool failure codes and recovery details", () => {
  const error = { code: "revision_conflict", message: "Changed", details: { revision: "8" } };
  expect(
    bridgeResult("camera.update", {
      isError: true,
      content: [{ type: "text", text: "Changed" }],
      structuredContent: { error },
    }),
  ).toEqual({ error });
});

test.each([
  { content: [image] },
  { content: [{ type: "resource_link", uri: "file:///unused.png" }], structuredContent: {} },
  { content: [image], structuredContent: { image: { mimeType: "image/png", data: png } } },
  { content: [{ type: "text", text: "no image" }], structuredContent: {} },
  { content: [image, image], structuredContent: {} },
  { content: [{ ...image, mimeType: "image/jpeg" }], structuredContent: {} },
  { content: [{ ...image, data: "invalid" }], structuredContent: {} },
  { content: [image], structuredContent: {}, isError: "true" },
])("rejects missing, unexpected, or ambiguous MCP image results", (response) => {
  expect(() => bridgeResult("artifact.read", response)).toThrow();
});

test("rejects images attached to an ordinary tool result", () => {
  expect(() =>
    bridgeResult("scene.summary", { content: [image], structuredContent: {} }),
  ).toThrow();
});

test("rejects malformed error metadata instead of fabricating a native outcome", () => {
  expect(() =>
    bridgeResult("camera.update", {
      isError: true,
      content: [{ type: "text", text: "failure" }],
      structuredContent: { error: { code: "failure", message: "failure", details: null } },
    }),
  ).toThrow();
});

test("reassembles split UTF-8 requests without forwarding configuration fields", async () => {
  const request = { id: "one", method: "scene.create", params: { name: "灯光" } };
  const encoded = Buffer.from(JSON.stringify(request) + "\n");
  const chunks = Array.from(encoded, (byte) => Buffer.from([byte]));
  const parsed = [];
  for await (const frame of readFrames(Readable.from(chunks))) parsed.push(parseRequest(frame));
  expect(parsed).toEqual([request]);
  expect(() =>
    parseRequest(Buffer.from(JSON.stringify({ ...request, workspace: "/unused" }))),
  ).toThrow();
});

test("accepts a complete frame exactly at the native worker's request bound", async () => {
  const input = Buffer.concat([Buffer.alloc(bound, 65), Buffer.from("\n")]);
  const lengths = [];
  for await (const frame of readFrames(Readable.from([input]))) lengths.push(frame.length);
  expect(lengths).toEqual([bound]);
});

test("passes a request above the adapter's native request bound on to the adapter", () => {
  const mapping = Array.from({ length: 4097 }, () => ({ source: 1, destination: 2 }));
  const request = {
    id: "big",
    method: "drawing.renumber",
    params: { mapping, pad: "x".repeat(300_000) },
  };
  expect(parseRequest(Buffer.from(JSON.stringify(request)))).toEqual(request);
});

test.each([
  Buffer.from("unfinished"),
  Buffer.alloc(bound + 1, 65),
  Buffer.concat([Buffer.alloc(bound + 1, 65), Buffer.from("\n")]),
])("rejects truncated or oversized fixture input", async (chunk) => {
  const collect = async () => {
    for await (const frame of readFrames(Readable.from([chunk]))) parseRequest(frame);
  };
  await expect(collect()).rejects.toThrow();
});

test.each([
  { id: 4, method: "scene.summary", params: {} },
  { id: "one", method: "execute/script", params: {} },
  { id: "one", method: "scene.summary", params: [] },
])("rejects malformed test request envelopes", (request) => {
  expect(() => parseRequest(Buffer.from(JSON.stringify(request)))).toThrow();
});

test("passes worker grants to the adapter unchanged so it validates them alone", () => {
  const grants = [
    "--workspace",
    "relative",
    "--media-tools",
    "--shared-root",
    "library=/library",
    "--shared-write",
    "library",
    "--input-root",
    "/inputs",
  ];
  expect(adapterArguments(["--automation-worker", ...grants], "/bin/OpenToonz", "/cli.js")).toEqual(
    ["/cli.js", "--executable", "/bin/OpenToonz", "--toolsets", "all", ...grants],
  );
});

test.each([
  [["--workspace", "/w"], "/bin/OpenToonz", "acceptance-test fixture"],
  [
    ["--automation-worker", "--workspace", "/w", "--test-domains"],
    "/bin/OpenToonz",
    "--test-domains",
  ],
  [["--automation-worker", "--toolsets", "core"], "/bin/OpenToonz", "--toolsets"],
  [["--automation-worker", "--executable", "/other"], "/bin/OpenToonz", "--executable"],
  [["--automation-worker", "--workspace=/w"], "/bin/OpenToonz", "--workspace=/w"],
  [["--automation-worker", "--workspace", "/w"], "relative", "absolute"],
  [["--automation-worker", "--workspace", "/w"], undefined, "absolute"],
])("refuses launches that are not plain worker grants: %j", (argv, executable, message) => {
  expect(() => adapterArguments(argv, executable, "/cli.js")).toThrow(message);
});
