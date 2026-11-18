import { PassThrough } from "node:stream";
import type { Readable, Writable } from "node:stream";
import {
  ProtocolErrorCode,
  STDIO_DEFAULT_MAX_BUFFER_SIZE,
  parseJSONRPCMessage,
} from "@modelcontextprotocol/server";
import type { JSONRPCMessage } from "@modelcontextprotocol/server";
import { StdioServerTransport } from "@modelcontextprotocol/server/stdio";
import { isObject } from "#mcp/errors";

/** Longest input line accepted, the SDK's own stdio bound; a longer one closes the session. */
export const MAX_LINE_BYTES = STDIO_DEFAULT_MAX_BUFFER_SIZE;
const MAX_ECHOED_ID_LENGTH = 128;

export interface LineReply {
  code: number;
  message: string;
  id?: string | number;
}

export type LineVerdict = JSONRPCMessage | LineReply | undefined;

function echoedId(id: unknown): { id?: string | number } {
  if (Number.isSafeInteger(id)) return { id: id as number };
  if (typeof id === "string" && id.length <= MAX_ECHOED_ID_LENGTH) return { id };
  return {};
}

function notificationOrResponse(value: Record<string, unknown>): boolean {
  if ("method" in value) return !("id" in value);
  return "result" in value || "error" in value;
}

/** Decide what one input line is: a message to serve, a malformed one to answer, or neither.
 *
 * Args:
 *   line: One line of client input without its line ending.
 * Returns:
 *   The parsed message; a reply for text that is not JSON (-32700) or for an invalid request
 *   (-32600, with its id only when that id is valid and short, as MCP leaves an unknown id
 *   out rather than null); or undefined for a blank line
 *   and for an invalid notification or response, which JSON-RPC never answers.
 */
export function classifyLine(line: string): LineVerdict {
  if (line.trim() === "") return undefined;
  let value: unknown;
  try {
    value = JSON.parse(line);
  } catch {
    return { code: ProtocolErrorCode.ParseError, message: "Parse error" };
  }
  try {
    return parseJSONRPCMessage(value);
  } catch {
    if (isObject(value) && notificationOrResponse(value)) return undefined;
    const id = isObject(value) ? echoedId(value["id"]) : {};
    return { code: ProtocolErrorCode.InvalidRequest, message: "Invalid Request", ...id };
  }
}

function isReply(verdict: LineVerdict): verdict is LineReply {
  return verdict !== undefined && "code" in verdict && !("jsonrpc" in verdict);
}

/** Serve MCP over stdio through the SDK transport, answering malformed input lines.
 *
 * The SDK transport skips text that is not JSON without telling anyone and reports other
 * invalid lines only to its error callback, which never sees the line and so cannot answer
 * it. This frames the input itself, hands every valid message to the transport's `onmessage`
 * and answers each malformed request once. A line over MAX_LINE_BYTES closes the session
 * unanswered, as the SDK does, and input is no longer read.
 *
 * Args:
 *   input: Client input, normally stdin.
 *   output: Client output, normally stdout; only the SDK transport writes to it.
 * Returns:
 *   The transport to pass to `serveStdio`.
 */
export function guardedStdioTransport(
  input: Readable = process.stdin,
  output: Writable = process.stdout,
): StdioServerTransport {
  const lifecycle = new PassThrough();
  const transport = new StdioServerTransport(lifecycle, output);
  let pending: Buffer = Buffer.alloc(0);
  const deliver = (line: Buffer) => {
    const verdict = classifyLine(line.toString("utf8").replace(/\r$/u, ""));
    if (verdict === undefined) return;
    if (!isReply(verdict)) {
      transport.onmessage?.(verdict);
      return;
    }
    const { code, message, ...id } = verdict;
    const reply: JSONRPCMessage = { jsonrpc: "2.0", ...id, error: { code, message } };
    transport.send(reply).catch((error: unknown) => transport.onerror?.(error as Error));
  };
  const overflow = () => {
    input.off("data", receive);
    input.pause();
    pending = Buffer.alloc(0);
    void transport.close();
  };
  const receive = (chunk: Buffer) => {
    pending = Buffer.concat([pending, chunk]);
    let newline = pending.indexOf(10);
    while (newline >= 0) {
      if (newline > MAX_LINE_BYTES) return overflow();
      const line = pending.subarray(0, newline);
      pending = pending.subarray(newline + 1);
      deliver(line);
      newline = pending.indexOf(10);
    }
    if (pending.length > MAX_LINE_BYTES) overflow();
  };
  input.on("data", receive);
  input.once("end", () => lifecycle.end());
  input.once("error", (error: Error) => lifecycle.destroy(error));
  return transport;
}
