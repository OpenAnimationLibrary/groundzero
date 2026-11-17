import { appendFileSync } from "node:fs";

const mode = process.argv[2];
const methods = [
  "live.status",
  "live.inspect",
  "live.stage.set_key_value",
  "live.fx.set_scalar_value",
  "live.receipt",
  "live.capabilities",
];
const hello = {
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

function initial() {
  if (mode === "no-hello") return;
  if (mode === "invalid-hello") hello.mode = "admin";
  if (mode === "invalid-identity") hello.instance_id = "not-an-instance-uuid";
  if (mode === "wrong-limits") hello.limits.pending_per_client = 16;
  process.stdout.write(frame(hello));
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

function writeResponse(request) {
  const result =
    mode === "error"
      ? { id: request.id, error: { code: "gui_busy", message: "Finish the gesture", details: {} } }
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
initial();
