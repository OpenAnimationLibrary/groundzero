import { expect, test } from "vitest";
import { WorkerError, errorResult } from "#mcp/errors";
import { boundedResult, toolResult } from "#mcp/results";

const png =
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a5xkAAAAASUVORK5CYII=";

test("returns actual image content alongside source metadata", () => {
  const result = toolResult({
    artifact_id: "preview-one",
    source_revision: 3,
    image: { mimeType: "image/png", data: png },
  });
  expect(result.content).toContainEqual({ type: "image", mimeType: "image/png", data: png });
  expect(result.structuredContent).toEqual({ artifact_id: "preview-one", source_revision: 3 });
});

test.each([
  { mimeType: "image/svg+xml", data: "<svg/>" },
  { mimeType: "image/png", data: "not base64!" },
  { mimeType: "image/png", data: Buffer.from("not png").toString("base64") },
  { mimeType: "image/png", data: "A".repeat(6 * 1024 * 1024) },
])("rejects invalid or oversized image payloads", (image) => {
  expect(() => toolResult({ image })).toThrow();
});

test("rejects image dimensions beyond the preview limit", () => {
  const bytes = Buffer.from(png, "base64");
  bytes.writeUInt32BE(2049, 16);
  const result = { image: { mimeType: "image/png", data: bytes.toString("base64") } };
  expect(() => toolResult(result)).toThrow("dimensions");
});

test.each([null, [], 42, "text"])("rejects non-object native results", (value) => {
  expect(() => toolResult(value)).toThrow("must be an object");
});

test("carries the same JSON in a text block and in structured content", () => {
  const metadata = { revision: "7", nested: { rows: [1, 2, 3], note: "灯" } };
  const result = toolResult(metadata);
  expect(result.content).toEqual([{ type: "text", text: JSON.stringify(metadata) }]);
  expect(result.structuredContent).toEqual(metadata);
});

test("keeps a result of several hundred kilobytes complete in its text", () => {
  const metadata = { payload: "x".repeat(300 * 1024), revision: "7" };
  const result = boundedResult(toolResult(metadata), "large-result");
  expect(result.isError).not.toBe(true);
  expect(result.structuredContent).toEqual(metadata);
  expect(result.content).toEqual([{ type: "text", text: JSON.stringify(metadata) }]);
});

test("refuses a result whose text and structured copies exceed the response bound", () => {
  const result = boundedResult(toolResult({ payload: "x".repeat(5 * 1024 * 1024) }), 1);
  expect(result.isError).toBe(true);
  expect(result.structuredContent).toMatchObject({ error: { code: "RESULT_TOO_LARGE" } });
});

test("retains image bytes with large accompanying metadata", () => {
  const metadata = { payload: "灯".repeat(64 * 1024), source_revision: "3" };
  const result = boundedResult(
    toolResult({ ...metadata, image: { mimeType: "image/png", data: png } }),
    1,
  );
  expect(result.structuredContent).toEqual(metadata);
  expect(result.content).toEqual([
    { type: "text", text: JSON.stringify(metadata) },
    { type: "image", mimeType: "image/png", data: png },
  ]);
});

test.each(['"'.repeat(5 * 1024 * 1024), "灯".repeat(4 * 1024 * 1024)])(
  "accounts for JSON escaping and UTF-8 bytes in the final response bound",
  (payload) => {
    const result = boundedResult(toolResult({ payload }), 1);
    expect(result.isError).toBe(true);
    expect(result.structuredContent).toMatchObject({ error: { code: "RESULT_TOO_LARGE" } });
    expect(Buffer.byteLength(JSON.stringify(result))).toBeLessThan(1024);
  },
);

test("also bounds native error details without sending an oversized frame", () => {
  const error = new WorkerError("NATIVE_FAILURE", "Operation failed", {
    detail: "x".repeat(10 * 1024 * 1024),
  });
  const result = boundedResult(errorResult(error), 1);
  expect(result.structuredContent).toMatchObject({ error: { code: "RESULT_TOO_LARGE" } });
});

test("rejects request identities that cannot fit even a compact error envelope", () => {
  const requestId = "x".repeat(9 * 1024 * 1024);
  expect(() => boundedResult(toolResult({}), requestId)).toThrow("Request identity");
});
