#!/usr/bin/env node
import assert from "node:assert/strict";
import { isAbsolute, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/client";
import { StdioClientTransport } from "@modelcontextprotocol/client/stdio";

const maxRequestBytes = 8 * 1024 * 1024;
const maxResponseBytes = 9 * 1024 * 1024;
const maxImageBytes = 4 * 1024 * 1024;
const decoder = new TextDecoder("utf-8", { fatal: true });
const imageTools = new Set(["artifact.read", "library.thumbnail"]);
const protocolVersion = "2026-07-28";
const workerGrants = new Set([
  "--workspace",
  "--input-root",
  "--media-tools",
  "--shared-root",
  "--shared-write",
]);

function object(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function keys(value, expected) {
  assert.deepEqual(Object.keys(value).sort(), [...expected].sort(), "Unexpected fixture schema");
}

/** Parse one complete test request without accepting product configuration changes.
 *
 * Args:
 *   frame: A bounded UTF-8 NDJSON frame without its newline.
 * Returns:
 *   The original request identity, advertised method name, and object arguments.
 */
export function parseRequest(frame) {
  assert(frame.length <= maxRequestBytes, "Fixture request exceeds 8 MiB");
  const request = JSON.parse(decoder.decode(frame));
  assert(object(request), "Fixture request must be an object");
  keys(request, ["id", "method", "params"]);
  assert.equal(typeof request.id, "string", "Fixture request ID must be a string");
  assert(request.id.length > 0 && request.id.length <= 128, "Invalid fixture request ID");
  assert.equal(typeof request.method, "string", "Fixture method must be a string");
  assert(/^[A-Za-z0-9._-]{1,128}$/.test(request.method), "Invalid fixture method name");
  assert(object(request.params), "Fixture arguments must be an object");
  return request;
}

/** Read bounded NDJSON, rejecting truncated final frames instead of replaying them.
 *
 * Args:
 *   input: An asynchronous stream of binary chunks from the acceptance fixture.
 * Yields:
 *   Complete frames whose bytes remain valid across chunk boundaries.
 */
export async function* readFrames(input) {
  let buffered = Buffer.alloc(0);
  for await (const chunk of input) {
    buffered = Buffer.concat([buffered, chunk]);
    let newline = buffered.indexOf(10);
    while (newline >= 0) {
      assert(newline <= maxRequestBytes, "Fixture request exceeds 8 MiB");
      const frame = buffered.subarray(0, newline);
      buffered = buffered.subarray(newline + 1);
      yield frame;
      newline = buffered.indexOf(10);
    }
    assert(buffered.length <= maxRequestBytes, "Fixture request exceeds 8 MiB");
  }
  assert.equal(buffered.length, 0, "Fixture input ended during a request");
}

function imagePayload(image) {
  keys(image, ["type", "mimeType", "data"]);
  assert.equal(image.mimeType, "image/png", "MCP artifact must contain a PNG image");
  assert.equal(typeof image.data, "string", "MCP image data must be base64 text");
  assert(image.data.length <= Math.ceil(maxImageBytes / 3) * 4, "MCP image is too large");
  const bytes = Buffer.from(image.data, "base64");
  assert(bytes.length <= maxImageBytes, "MCP image is too large");
  assert.equal(bytes.toString("base64"), image.data, "MCP image has invalid base64");
  assert.deepEqual(bytes.subarray(0, 8), Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]));
  return { mimeType: image.mimeType, data: image.data };
}

function responseImages(response) {
  assert(object(response), "MCP tool result must be an object");
  assert(object(response.structuredContent), "MCP tool result lacks structured metadata");
  assert(!("image" in response.structuredContent), "Image bytes must come from MCP ImageContent");
  assert(Array.isArray(response.content), "MCP tool result lacks content");
  assert(
    response.content.length > 0 && response.content.length <= 2,
    "Unexpected MCP content count",
  );
  const images = [];
  for (const content of response.content) {
    assert(object(content), "MCP content must be an object");
    if (content.type === "image") images.push(imagePayload(content));
    else {
      assert.equal(content.type, "text", "Unexpected MCP content type");
      assert.equal(typeof content.text, "string", "MCP text content must be a string");
    }
  }
  return images;
}

/** Adapt validated MCP results only for the existing Python acceptance fixture.
 *
 * Args:
 *   method: The advertised MCP tool called by the fixture.
 *   response: Its SDK-validated result with structured metadata and content.
 * Returns:
 *   A native-shaped test outcome, recovering images exclusively from ImageContent
 *   and only for the tools declared to return one.
 */
export function bridgeResult(method, response) {
  const images = responseImages(response);
  const metadata = response.structuredContent;
  if (response.isError === true) {
    assert.equal(images.length, 0, "MCP errors cannot contain images");
    keys(metadata, ["error"]);
    assert(object(metadata.error), "MCP tool error lacks structured details");
    keys(metadata.error, ["code", "message", "details"]);
    assert.equal(typeof metadata.error.code, "string");
    assert.equal(typeof metadata.error.message, "string");
    assert(object(metadata.error.details), "MCP error details must be an object");
    return { error: metadata.error };
  }
  assert(response.isError === undefined || response.isError === false, "Invalid MCP error flag");
  if (imageTools.has(method)) {
    assert.equal(images.length, 1, `${method} must return one MCP ImageContent`);
    return { result: { ...metadata, image: images[0] } };
  }
  assert.equal(images.length, 0, "Unexpected image from a tool that declares none");
  return { result: metadata };
}

/** Turn the worker command line this fixture stands in for into the adapter's.
 *
 * Args:
 *   argv: Worker arguments: --automation-worker, then filesystem grants.
 *   executable: The absolute native executable the adapter launches.
 *   cli: The adapter entry point.
 * Returns:
 *   Adapter arguments enabling every toolset, with the grants passed on unchanged so the
 *   adapter alone validates them.
 */
export function adapterArguments(argv, executable, cli) {
  const [mode, ...grants] = argv;
  assert.equal(mode, "--automation-worker", "This executable is only an acceptance-test fixture");
  for (const token of grants) {
    assert(!token.startsWith("-") || workerGrants.has(token), `Not a worker grant: ${token}`);
  }
  assert(executable && isAbsolute(executable), "GROUNDZERO_NATIVE_EXECUTABLE must be absolute");
  return [cli, "--executable", executable, "--toolsets", "all", ...grants];
}

async function writeFrame(response) {
  const frame = JSON.stringify(response) + "\n";
  assert(Buffer.byteLength(frame) <= maxResponseBytes, "Fixture response exceeds 9 MiB");
  await new Promise((resolveWrite, rejectWrite) => {
    process.stdout.write(frame, (error) => {
      if (error) rejectWrite(error);
      else resolveWrite();
    });
  });
}

async function serve(client) {
  const catalog = await client.listTools();
  assert.equal(catalog.nextCursor, undefined, "Unexpected paginated tool catalog");
  const names = new Set(catalog.tools.map((tool) => tool.name));
  assert.equal(names.size, catalog.tools.length, "Duplicate MCP tool names");
  let calls = 0;
  let images = 0;
  for await (const frame of readFrames(process.stdin)) {
    const request = parseRequest(frame);
    assert(names.has(request.method), `Unadvertised MCP tool: ${request.method}`);
    const response = await client.callTool({ name: request.method, arguments: request.params });
    const outcome = bridgeResult(request.method, response);
    await writeFrame({ id: request.id, ...outcome });
    calls += 1;
    if (outcome.result?.image) images += 1;
  }
  process.stderr.write(`groundzero-mcp-shot-client: ${calls} MCP calls; ${images} MCP images\n`);
}

async function connect(client) {
  const args = adapterArguments(
    process.argv.slice(2),
    process.env["GROUNDZERO_NATIVE_EXECUTABLE"],
    fileURLToPath(new URL("../dist/cli.js", import.meta.url)),
  );
  await client.connect(
    new StdioClientTransport({
      command: process.execPath,
      args,
      env: Object.fromEntries(
        Object.entries(process.env).filter(([, value]) => value !== undefined),
      ),
      stderr: "inherit",
    }),
  );
  assert.equal(client.getNegotiatedProtocolVersion(), protocolVersion, "Unexpected MCP protocol");
}

async function main() {
  // Pinning would probe on a sibling process whose stderr the SDK discards, hiding why an
  // adapter refused its launch. "auto" relaunches such an adapter as the session process, whose
  // stderr is kept; connect() still requires the modern protocol version.
  const client = new Client(
    { name: "groundzero-shot-fixture", version: "1.0.0" },
    { versionNegotiation: { mode: "auto" } },
  );
  let closing = false;
  let failure;
  let closePromise;
  const close = () => {
    closing = true;
    closePromise ??= client.close();
    return closePromise;
  };
  const stop = (error) => {
    failure ??= error;
    process.stdin.destroy();
    void close().catch((closeError) => {
      failure ??= closeError;
    });
  };
  const interrupted = () => stop(new Error("MCP shot fixture was interrupted"));
  client.onclose = () => {
    if (!closing) {
      failure ??= new Error("MCP adapter disconnected unexpectedly");
      process.stdin.destroy();
    }
  };
  process.once("SIGTERM", interrupted);
  process.once("SIGINT", interrupted);
  process.stdout.once("error", stop);
  try {
    await connect(client);
    await serve(client);
  } catch (error) {
    failure ??= error;
  } finally {
    await close();
    process.removeListener("SIGTERM", interrupted);
    process.removeListener("SIGINT", interrupted);
    process.stdout.removeListener("error", stop);
  }
  if (failure) throw failure;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  void main().catch((error) => {
    process.stderr.write(`MCP shot fixture failed: ${String(error)}\n`);
    process.exitCode = 1;
  });
}
