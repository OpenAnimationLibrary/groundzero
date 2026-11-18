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

export const LIVE_PROTOCOL = "groundzero-live-v2";
const REFUSAL_CODES = new Set([
  "protocol_mismatch",
  "build_mismatch",
  "authentication_failed",
  "live_unavailable",
  "invalid_capabilities",
  "live_security",
  "connector_io",
  "invalid_frame",
]);
const MISMATCH_CODES = new Set(["protocol_mismatch", "build_mismatch"]);
export const LIVE_RECOVERY =
  "Reconnect to the same instance and query live.receipt with the original operation_id. " +
  "Do not blindly replay edits; a new instance requires inspection.";

/** Why a live edit's expected_fingerprint is stale; fingerprints follow the undo history. */
export type StaleTargetReason = "content_changed" | "history_changed" | "target_replaced";

/** Details of a stale_target refusal: retry with current_fingerprint after re-checking. */
export interface StaleTargetDetails {
  reason: StaleTargetReason;
  current_fingerprint: string;
  [context: string]: unknown;
}

const STALE_TARGET_REASONS = new Set<unknown>([
  "content_changed",
  "history_changed",
  "target_replaced",
]);

export interface LiveBuild {
  application: "groundzero";
  version: string;
  executable_sha256: string;
}

export interface LiveHello {
  protocol: string;
  build: LiveBuild;
  instanceId: string;
  clientId: string;
  mode: "inspect" | "edit";
  tools: unknown[];
  limits: Record<string, unknown>;
}

const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const sha256 = /^[0-9a-f]{64}$/;
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
  if ("refusal" in value) throw liveRefusal(value);
  exactFields(value, ["protocol", "build", "instance_id", "client_id", "mode", "tools", "limits"]);
  if (value["protocol"] !== LIVE_PROTOCOL) {
    throw new Error("Live hello uses another bridge protocol");
  }
  const mode = value["mode"];
  if (mode !== "inspect" && mode !== "edit") throw new Error("Unknown live permission mode");
  const { tools, limits } = liveCatalog(value["tools"], value["limits"]);
  return {
    protocol: LIVE_PROTOCOL,
    build: liveBuild(value["build"]),
    instanceId: liveIdentity(value["instance_id"]),
    clientId: liveIdentity(value["client_id"]),
    mode,
    tools,
    limits,
  };
}

function liveCatalog(tools: unknown, limits: unknown) {
  if (!Array.isArray(tools) || tools.length !== LIVE_TOOL_NAMES.size || !isObject(limits)) {
    throw new Error("Live hello requires the exact live catalog and limits");
  }
  checkLimits(limits);
  return { tools, limits };
}

function liveBuild(value: unknown): LiveBuild {
  if (!isObject(value)) throw new Error("Live build identity must be an object");
  exactFields(value, ["application", "version", "executable_sha256"]);
  const version = value["version"];
  const digest = value["executable_sha256"];
  if (
    value["application"] !== "groundzero" ||
    typeof version !== "string" ||
    version.length < 1 ||
    version.length > 64 ||
    typeof digest !== "string" ||
    !sha256.test(digest)
  ) {
    throw new Error("Invalid live build identity");
  }
  return { application: "groundzero", version, executable_sha256: digest };
}

function peerIdentity(value: unknown, nullableBuild: boolean): unknown {
  if (value === null) return null;
  if (!isObject(value)) throw new Error("Invalid live refusal identity");
  exactFields(value, ["protocol", "build"]);
  const protocol = value["protocol"];
  if (typeof protocol !== "string" || protocol.length < 1 || protocol.length > 64) {
    throw new Error("Invalid live refusal protocol");
  }
  const build = nullableBuild && value["build"] === null ? null : liveBuild(value["build"]);
  return { protocol, build };
}

/** Map the connector's one framed refusal, sent instead of a hello, to an MCP error.
 *
 * Args:
 *   value: The connector's first frame, holding only a refusal.
 * Returns:
 *   LIVE_INCOMPATIBLE for a protocol or build mismatch, else LIVE_UNAVAILABLE naming the cause.
 */
export function liveRefusal(value: Record<string, unknown>): WorkerError {
  const { code, message, connector, editor } = refusalFields(value);
  if (MISMATCH_CODES.has(code)) {
    return new WorkerError("LIVE_INCOMPATIBLE", message, {
      cause: code,
      connector,
      editor,
      recovery: "Start the editor and the adapter from one build.",
    });
  }
  return new WorkerError("LIVE_UNAVAILABLE", message, {
    cause: code,
    outcome_unknown: false,
    instance_id: null,
    recovery: LIVE_RECOVERY,
  });
}

function refusalFields(value: Record<string, unknown>) {
  exactFields(value, ["refusal"]);
  const refusal = value["refusal"];
  if (!isObject(refusal)) throw new Error("Invalid live refusal");
  exactFields(refusal, ["code", "message", "details"]);
  const { code, message, details } = refusal;
  if (typeof code !== "string" || !REFUSAL_CODES.has(code)) {
    throw new Error("Unknown live refusal code");
  }
  if (typeof message !== "string" || message.length < 1 || message.length > 512) {
    throw new Error("Invalid live refusal message");
  }
  if (!isObject(details)) throw new Error("Invalid live refusal details");
  exactFields(details, ["connector", "editor"]);
  return {
    code,
    message,
    connector: peerIdentity(details["connector"], false),
    editor: peerIdentity(details["editor"], true),
  };
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
  if (value["code"] === "stale_target") staleTargetDetails(value["details"]);
  return new WorkerError(value["code"], value["message"], value["details"]);
}

function staleTargetDetails(details: Record<string, unknown>): StaleTargetDetails {
  const { reason, current_fingerprint: current } = details;
  if (!STALE_TARGET_REASONS.has(reason) || typeof current !== "string" || !sha256.test(current)) {
    throw new Error("A stale_target error requires its reason and current fingerprint");
  }
  return details as StaleTargetDetails;
}
