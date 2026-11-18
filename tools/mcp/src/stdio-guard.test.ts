import { STDIO_DEFAULT_MAX_BUFFER_SIZE } from "@modelcontextprotocol/server";
import type { JSONRPCMessage } from "@modelcontextprotocol/server";
import { StdioServerTransport } from "@modelcontextprotocol/server/stdio";
import { PassThrough } from "node:stream";
import { setTimeout as pause } from "node:timers/promises";
import { expect, test } from "vitest";
import { MAX_LINE_BYTES, classifyLine, guardedStdioTransport } from "#mcp/stdio-guard";

function captured(output: PassThrough): () => string {
  let text = "";
  output.setEncoding("utf8").on("data", (chunk: string) => (text += chunk));
  return () => text;
}

const malformed = ["{bad json", "[]", JSON.stringify({ jsonrpc: "1.0", id: 1, method: "ping" })];

test("the SDK transport still leaves malformed lines unanswered, so the guard answers them", async () => {
  const input = new PassThrough();
  const output = new PassThrough();
  const written = captured(output);
  const transport = new StdioServerTransport(input, output);
  const delivered: JSONRPCMessage[] = [];
  transport.onmessage = (message) => delivered.push(message);
  transport.onerror = () => {};
  await transport.start();
  input.write(malformed.map((line) => `${line}\n`).join(""));
  await pause(50);
  expect(delivered).toEqual([]);
  expect(written()).toBe("");
  await transport.close();
});

test("the SDK transport still closes on input over the bound the guard keeps", async () => {
  expect(MAX_LINE_BYTES).toBe(10 * 1024 * 1024);
  expect(STDIO_DEFAULT_MAX_BUFFER_SIZE).toBe(MAX_LINE_BYTES);
  const input = new PassThrough();
  const transport = new StdioServerTransport(input, new PassThrough());
  let closed = false;
  transport.onclose = () => (closed = true);
  transport.onerror = () => {};
  await transport.start();
  input.write(Buffer.alloc(MAX_LINE_BYTES + 1, 120));
  await pause(50);
  expect(closed).toBe(true);
});

test.each([
  ["{bad", { code: -32700, message: "Parse error", id: null }],
  ["7", { code: -32600, message: "Invalid Request", id: null }],
  ['{"jsonrpc":"2.0","id":7,"method":7}', { code: -32600, message: "Invalid Request", id: 7 }],
  ['{"jsonrpc":"2.0","id":"a","params":[]}', { code: -32600, message: "Invalid Request", id: "a" }],
  [
    '{"jsonrpc":"2.0","id":1.5,"method":"x"}',
    { code: -32600, message: "Invalid Request", id: null },
  ],
  ['{"jsonrpc":"2.0","method":7}', undefined],
  ['{"jsonrpc":"2.0","id":3,"result":7,"error":{}}', undefined],
  ["  ", undefined],
])("classifies %s", (line, verdict) => {
  expect(classifyLine(line)).toEqual(verdict);
});

test("passes valid messages to the transport and answers a malformed request once", async () => {
  const input = new PassThrough();
  const output = new PassThrough();
  const written = captured(output);
  const transport = guardedStdioTransport(input, output);
  const delivered: JSONRPCMessage[] = [];
  transport.onmessage = (message) => delivered.push(message);
  let closed = false;
  transport.onclose = () => (closed = true);
  await transport.start();
  input.write('{"jsonrpc":"2.0","id":1,"method":"ping"}\r\n{"jsonrpc":"2.0","id":2}\n');
  await pause(20);
  expect(delivered).toEqual([{ jsonrpc: "2.0", id: 1, method: "ping" }]);
  expect(written()).toBe(
    '{"jsonrpc":"2.0","id":2,"error":{"code":-32600,"message":"Invalid Request"}}\n',
  );
  input.end();
  await pause(20);
  expect(closed).toBe(true);
});
