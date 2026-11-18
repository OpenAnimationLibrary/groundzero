import { appendFileSync } from "node:fs";
import { randomUUID } from "node:crypto";
import { createConnection } from "node:net";

const mode = process.argv[2];
const methods = [
  "live.status",
  "live.inspect",
  "live.stage.set_key_value",
  "live.fx.set_scalar_value",
  "live.receipt",
  "live.capabilities",
];
const build = { application: "groundzero", version: "1.8", build_digest: "c".repeat(64) };
const identity = { protocol: "groundzero-live-v2", build };
const hello = {
  protocol: "groundzero-live-v2",
  build,
  instance_id: "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
  client_id: "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb",
  mode: "edit",
  tools: methods.map((name) => ({
    name,
    description: "Native connector fixture",
    inputSchema: { type: "object", additionalProperties: false, properties: {} },
    annotations: { readOnlyHint: !name.includes("set_"), idempotentHint: true },
  })),
  limits: { request_bytes: 65536, response_bytes: 1048576, pending_per_client: 4 },
};
let buffer = Buffer.alloc(0);

function frame(value) {
  const body = Buffer.from(JSON.stringify(value));
  const output = Buffer.alloc(4 + body.length);
  output.writeUInt32BE(body.length);
  body.copy(output, 4);
  return output;
}

function refusal(code, message = "The connector refused before its hello.") {
  const editor = { protocol: "groundzero-live-v1", build: null };
  return { refusal: { code, message, details: { connector: identity, editor } } };
}

function refuse() {
  const value = mode === "oversized-refusal" ? refusal("build_mismatch", "x".repeat(513)) : null;
  const code = mode.slice("refusal-".length);
  process.stdout.write(frame(value ?? refusal(code)), () => process.exit(2));
}

const helloChanges = {
  "invalid-hello": () => (hello.mode = "admin"),
  "invalid-identity": () => (hello.instance_id = "not-an-instance-uuid"),
  "wrong-limits": () => (hello.limits.pending_per_client = 16),
  "unversioned-hello": () => delete hello.protocol,
  "previous-hello": () => (hello.protocol = "groundzero-live-v1"),
  "invalid-build": () => (hello.build = { ...build, build_digest: "short" }),
};

function initial() {
  if (mode === "no-hello") return;
  if (mode.startsWith("refusal-") || mode === "oversized-refusal") return refuse();
  helloChanges[mode]?.();
  process.stdout.write(frame(hello));
  if (mode === "late-refusal") process.stdout.write(frame(refusal("build_mismatch")));
}

function malformed(request) {
  const cases = {
    "extra-field": { id: request.id, result: {}, token: "must-not-be-logged" },
    "both-results": { id: request.id, result: {}, error: {} },
    "unknown-id": { id: "not-the-request-id", result: {} },
    "bad-error": { id: request.id, error: { code: "failure" } },
  };
  if (!(mode in cases)) return false;
  process.stdout.write(frame(cases[mode]));
  return true;
}

function answer(request) {
  if (process.env["LIVE_FIXTURE_RECORD"]) {
    appendFileSync(process.env["LIVE_FIXTURE_RECORD"], `${JSON.stringify(request)}\n`);
  }
  if (mode === "hold") return;
  if (malformed(request)) return;
  if (mode === "oversized" || mode === "empty-frame") {
    const header = Buffer.alloc(4);
    header.writeUInt32BE(mode === "oversized" ? 1048577 : 0);
    process.stdout.write(header);
    return;
  }
  writeResponse(request);
}

const errors = {
  error: { code: "gui_busy", message: "Finish the gesture", details: {} },
  stale: {
    code: "stale_target",
    message: "Inspect before editing",
    details: { reason: "history_changed", current_fingerprint: "d".repeat(64), channel: "x" },
  },
  "bad-stale": {
    code: "stale_target",
    message: "Inspect before editing",
    details: { reason: "undone" },
  },
};

function writeResponse(request) {
  const result =
    mode in errors
      ? { id: request.id, error: errors[mode] }
      : {
          id: request.id,
          result: {
            instance_id: hello.instance_id,
            method: request.method,
            arguments: request.params,
            padding: mode === "final" ? "x".repeat(2048) : "",
          },
        };
  const output = frame(result);
  if (mode === "duplicate") {
    process.stdout.write(Buffer.concat([output, output]));
    return;
  }
  if (mode === "split") {
    process.stdout.write(output.subarray(0, 2));
    setTimeout(() => process.stdout.write(output.subarray(2)), 5);
    return;
  }
  process.stdout.write(output, () => {
    if (mode === "final") process.exit(0);
  });
}

process.stdin.on("data", (chunk) => {
  buffer = Buffer.concat([buffer, chunk]);
  while (buffer.length >= 4 && buffer.length >= buffer.readUInt32BE(0) + 4) {
    const size = buffer.readUInt32BE(0);
    const request = JSON.parse(buffer.subarray(4, size + 4).toString("utf8"));
    buffer = buffer.subarray(size + 4);
    answer(request);
  }
});
function start() {
  if (mode !== "leased") {
    initial();
    return;
  }
  hello.client_id = randomUUID();
  const lease = createConnection(process.env["LIVE_FIXTURE_SOCKET"]);
  lease.once("data", () => initial());
  lease.on("error", () => process.exit(1));
  lease.on("end", () => process.exit(1));
  process.stdin.once("end", () => lease.end());
}

start();
