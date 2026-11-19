import type { CallToolResult, ImageContent, RequestId } from "@modelcontextprotocol/server";
import { WorkerError, errorResult, isObject } from "#mcp/errors";

const MAX_IMAGE_BYTES = 4 * 1024 * 1024;
const MAX_IMAGE_DIMENSION = 2048;
const MAX_RESPONSE_BYTES = 9 * 1024 * 1024;
const SDK_METADATA_RESERVE_BYTES = 1024;
const PNG_SIGNATURE = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);

function checkPng(bytes: Buffer): void {
  if (bytes.length < 33 || !bytes.subarray(0, 8).equals(PNG_SIGNATURE)) {
    throw new WorkerError("INVALID_PREVIEW", "Preview does not have a PNG signature");
  }
  if (bytes.readUInt32BE(8) !== 13 || bytes.toString("ascii", 12, 16) !== "IHDR") {
    throw new WorkerError("INVALID_PREVIEW", "Preview does not have a valid PNG header");
  }
  const width = bytes.readUInt32BE(16);
  const height = bytes.readUInt32BE(20);
  if (Math.min(width, height) < 1 || Math.max(width, height) > MAX_IMAGE_DIMENSION) {
    throw new WorkerError("INVALID_PREVIEW", "Preview dimensions must be 1–2048 pixels");
  }
}

function previewImage(value: unknown): ImageContent {
  if (!isObject(value) || value["mimeType"] !== "image/png") {
    throw new WorkerError("INVALID_PREVIEW", "Native preview must be a PNG image");
  }
  const data = value["data"];
  if (typeof data !== "string" || data.length > Math.ceil(MAX_IMAGE_BYTES / 3) * 4) {
    throw new WorkerError("INVALID_PREVIEW", "Preview exceeds 4 MiB");
  }
  const bytes = Buffer.from(data, "base64");
  if (bytes.length > MAX_IMAGE_BYTES || bytes.toString("base64") !== data) {
    throw new WorkerError("INVALID_PREVIEW", "Preview has invalid or oversized base64 data");
  }
  checkPng(bytes);
  return { type: "image", mimeType: "image/png", data };
}

/** Preserve metadata and turn bounded native preview bytes into MCP image content.
 *
 * Args:
 *   value: Native result with an optional inline PNG image.
 * Returns:
 *   The metadata as JSON text, so a client that shows only text still has it, and as
 *   structured content, plus any image content.
 */
export function toolResult(value: unknown): CallToolResult {
  if (!isObject(value)) {
    throw new WorkerError("INVALID_RESULT", "Native operation result must be an object");
  }
  const { image, ...metadata } = value;
  const content: CallToolResult["content"] = [{ type: "text", text: JSON.stringify(metadata) }];
  if (image !== undefined) content.push(previewImage(image));
  return { content, structuredContent: metadata };
}

function responseBytes(result: CallToolResult, requestId: RequestId): number {
  const envelope = { jsonrpc: "2.0", id: requestId, result };
  return Buffer.byteLength(JSON.stringify(envelope)) + SDK_METADATA_RESERVE_BYTES;
}

/** Bound serialized responses below the SDK's 10 MiB stdio receive buffer.
 *
 * Args:
 *   result: Tool success or error content after native conversion.
 *   requestId: The client's JSON-RPC identity, included in the size calculation.
 * Returns:
 *   The complete result or a compact error, with room for SDK response metadata.
 */
export function boundedResult(result: CallToolResult, requestId: RequestId): CallToolResult {
  const bytes = responseBytes(result, requestId);
  if (bytes <= MAX_RESPONSE_BYTES) return result;
  const failure = errorResult(
    new WorkerError("RESULT_TOO_LARGE", "Native response exceeds the 9 MiB MCP response limit", {
      response_bytes: bytes,
      limit_bytes: MAX_RESPONSE_BYTES,
      recovery: "Request a smaller selection; inspect state before retrying any write",
    }),
  );
  if (responseBytes(failure, requestId) > MAX_RESPONSE_BYTES) {
    throw new WorkerError("REQUEST_ID_TOO_LARGE", "Request identity cannot fit a bounded response");
  }
  return failure;
}
