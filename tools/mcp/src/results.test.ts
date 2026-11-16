import { expect, test } from "vitest";
import { toolResult } from "#mcp/results";

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
