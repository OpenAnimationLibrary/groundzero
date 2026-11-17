import { WorkerError, isObject } from "#mcp/errors";

export const LIVE_REQUEST_BYTES = 64 * 1024;
export const LIVE_RESPONSE_BYTES = 1024 * 1024;
export const LIVE_PENDING_REQUESTS = 4;
export const LIVE_TOOL_NAMES = new Set([
  "live.status",
  "live.inspect",
  "live.stage.set_key_value",
  "live.fx.set_scalar_value",
  "live.receipt",
  "live.capabilities",
]);

export interface LiveHello {
  instanceId: string;
  clientId: string;
  mode: "inspect" | "edit";
  tools: unknown[];
  limits: Record<string, unknown>;
}

const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const decoder = new TextDecoder("utf-8", { fatal: true });

/** Require exact envelope fields before accepting connector data.
 *
 * Args:
 *   value: Parsed native envelope.
 *   fields: Complete allowed and required key set.
 */
export function exactFields(value: Record<string, unknown>, fields: string[]): void {
  const keys = Object.keys(value);
  if (keys.length !== fields.length || keys.some((key) => !fields.includes(key))) {
    throw new Error("Unexpected live protocol envelope fields");
  }
}

function checkDepth(value: unknown, depth: number): void {
  if (depth > 16) throw new Error("Live JSON nesting exceeds 16 levels");
  if (typeof value === "number" && !Number.isFinite(value)) {
    throw new Error("Live JSON contains a non-finite number");
  }
  if (Array.isArray(value)) {
    for (const item of value) checkDepth(item, depth + 1);
  } else if (isObject(value)) {
    for (const item of Object.values(value)) checkDepth(item, depth + 1);
  }
}

/** Decode a bounded native response without evaluating or normalizing its contents.
 *
 * Args:
 *   bytes: One complete framed response body.
 * Returns:
 *   Finite JSON values within the protocol nesting limit.
 */
export function decodeLiveJson(bytes: Buffer): unknown {
  const value: unknown = JSON.parse(decoder.decode(bytes));
  checkDepth(value, 0);
  return value;
}

/** Validate the authenticated connector hello and its fixed transport contract.
 *
 * Args:
 *   value: First message forwarded after native peer authentication.
 * Returns:
 *   The instance, active permission mode, exact catalog, and advertised limits.
 */
export function liveHello(value: unknown): LiveHello {
  if (!isObject(value)) throw new Error("Expected an authenticated live hello");
  exactFields(value, ["instance_id", "client_id", "mode", "tools", "limits"]);
  const instanceId = liveIdentity(value["instance_id"]);
  const clientId = liveIdentity(value["client_id"]);
  const mode = value["mode"];
  if (mode !== "inspect" && mode !== "edit") throw new Error("Unknown live permission mode");
  const tools = value["tools"];
  const limits = value["limits"];
  if (!Array.isArray(tools) || tools.length !== LIVE_TOOL_NAMES.size || !isObject(limits)) {
    throw new Error("Live hello requires the exact live catalog and limits");
  }
  checkLimits(limits);
  return { instanceId, clientId, mode, tools, limits };
}

function liveIdentity(value: unknown): string {
  if (typeof value !== "string" || !uuid.test(value)) {
    throw new Error("Live hello requires UUID instance and client identities");
  }
  return value;
}

function checkLimits(limits: Record<string, unknown>): void {
  if (
    limits["request_bytes"] !== LIVE_REQUEST_BYTES ||
    limits["response_bytes"] !== LIVE_RESPONSE_BYTES ||
    limits["pending_per_client"] !== LIVE_PENDING_REQUESTS
  ) {
    throw new Error("Live endpoint transport limits are incompatible");
  }
}

/** Serialize a single bounded request; transport identity is not an operation receipt.
 *
 * Args:
 *   id: Unique connection-local request identity.
 *   method: One approved live method name.
 *   params: Schema-validated arguments, including the original operation UUID for writes.
 * Returns:
 *   A four-byte big-endian header followed by at most 64 KiB of JSON.
 */
export function liveRequest(id: string, method: string, params: Record<string, unknown>): Buffer {
  if (!LIVE_TOOL_NAMES.has(method)) {
    throw new WorkerError("UNKNOWN_TOOL", "The operation is not in the live catalog");
  }
  checkDepth(params, 1);
  const body = Buffer.from(JSON.stringify({ id, method, params }));
  if (body.length > LIVE_REQUEST_BYTES) {
    throw new WorkerError("resource_limit", "Live request exceeds 64 KiB", {
      outcome: "not_started",
    });
  }
  const frame = Buffer.allocUnsafe(4 + body.length);
  frame.writeUInt32BE(body.length);
  body.copy(frame, 4);
  return frame;
}

/** Preserve a native domain failure as a structured MCP tool error.
 *
 * Args:
 *   value: Native error envelope from an authenticated connection.
 * Returns:
 *   The unchanged domain code, diagnostic, and details.
 */
export function liveError(value: unknown): WorkerError {
  if (!isObject(value)) throw new Error("Expected a structured live error");
  exactFields(value, ["code", "message", "details"]);
  if (
    typeof value["code"] !== "string" ||
    typeof value["message"] !== "string" ||
    !isObject(value["details"])
  ) {
    throw new Error("Invalid live error fields");
  }
  return new WorkerError(value["code"], value["message"], value["details"]);
}
