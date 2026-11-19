import { acceptControlChannel } from "#fixtures/control-channel";
import { outputSchema } from "#mcp/declarations";
import { appendFileSync } from "node:fs";
import { chmod, mkdir, mkdtemp, realpath, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";

const object = { type: "object", additionalProperties: false };
const readOnly = { readOnlyHint: true, destructiveHint: false, idempotentHint: true };
const effects = {
  class: "read",
  document: "none",
  files: [],
  job: "none",
  processes: [],
  overwrites: false,
  undo: "none",
  no_op: "not_applicable",
  after_change_failure: "not_applicable",
  rollback: "never_claimed",
  retry: "repeat_safe",
  grants: ["inspect"],
};
const tools = [
  {
    name: "probe.echo",
    description: "Return the given value.",
    inputSchema: { ...object, required: ["value"], properties: { value: { type: "integer" } } },
  },
  {
    name: "probe.slow",
    description: "Return the given value after the given delay.",
    inputSchema: {
      ...object,
      required: ["value", "delay_ms"],
      properties: { value: { type: "integer" }, delay_ms: { type: "integer", maximum: 10000 } },
    },
  },
  {
    name: "probe.large",
    description: "Return a string of the given length.",
    inputSchema: { ...object, required: ["bytes"], properties: { bytes: { type: "integer" } } },
  },
  {
    name: "probe.noise",
    description: "Write the given number of diagnostic bytes to stderr, then answer.",
    inputSchema: { ...object, required: ["bytes"], properties: { bytes: { type: "integer" } } },
  },
  {
    name: "probe.stdout_noise",
    description: "Write a diagnostic line to the protocol output instead of a response.",
    inputSchema: { ...object, properties: {} },
  },
  {
    name: "probe.crash",
    description: "Exit without answering.",
    inputSchema: { ...object, properties: {} },
  },
].map((tool) => ({
  ...tool,
  annotations: readOnly,
  outputSchema: outputSchema({ type: "object" }),
  effects,
  errors: ["invalid_params"],
  toolset: "core",
}));
const optionalTools = [
  { name: "probe.sound", toolset: "audio" },
  { name: "probe.joint", toolset: "rigging" },
].map((tool) => ({
  ...tool,
  description: "Return nothing.",
  inputSchema: { ...object, properties: {} },
  annotations: readOnly,
  outputSchema: outputSchema({ type: "object" }),
  effects,
  errors: ["invalid_params"],
}));
const catalog = [...tools, ...optionalTools];
const toolsets = [
  { name: "core", description: "Probe tools", tools: tools.map((t) => t.name) },
  { name: "audio", description: "Sound probe", tools: ["probe.sound"] },
  { name: "rigging", description: "Rig probe", tools: ["probe.joint"] },
];

function respond(id, result) {
  process.stdout.write(JSON.stringify({ id, result }) + "\n");
}

const handlers = {
  "automation.capabilities": (id) =>
    respond(id, {
      protocol_version: 5,
      tools: catalog,
      tool_count: catalog.length,
      next_offset: null,
      toolsets,
    }),
  "probe.echo": (id, params) => respond(id, { value: params.value }),
  "probe.slow": (id, params) =>
    setTimeout(() => respond(id, { value: params.value }), params.delay_ms),
  "probe.large": (id, params) => respond(id, { text: "x".repeat(params.bytes) }),
  "probe.noise": (id, params) => {
    process.stderr.write("native diagnostic ".repeat(Math.ceil(params.bytes / 18)) + "\n");
    respond(id, { written: params.bytes });
  },
  "probe.stdout_noise": () => process.stdout.write("native diagnostic on the protocol output\n"),
  "probe.crash": () => process.exit(70),
};

function answer(request) {
  const handler = handlers[request.method];
  if (handler) return handler(request.id, request.params ?? {});
  const error = { code: "method_not_found", message: "Unknown method", details: {} };
  process.stdout.write(JSON.stringify({ id: request.id, error }) + "\n");
}

/** Write an executable that runs this fake native worker, as the adapter requires a file.
 *
 * Returns:
 *   The canonical scratch directory to remove afterwards, a workspace inside it and the
 *   executable's path.
 */
export async function fakeNativeExecutable() {
  const directory = await realpath(await mkdtemp(join(tmpdir(), "gz-fake-native-")));
  const workspace = join(directory, "workspace");
  await mkdir(workspace);
  const executable = join(directory, "fake-native");
  const script = fileURLToPath(import.meta.url);
  await writeFile(executable, `#!/bin/sh\nexec '${process.execPath}' '${script}' "$@"\n`);
  await chmod(executable, 0o755);
  return { directory, workspace, executable };
}

if (process.argv[1] && process.argv[1] === fileURLToPath(import.meta.url)) {
  await acceptControlChannel();
  const log = process.env["FAKE_NATIVE_LOG"];
  if (log) appendFileSync(log, "started\n");
  createInterface({ input: process.stdin }).on("line", (line) => {
    const request = JSON.parse(line);
    if (log) appendFileSync(log, request.method + "\n");
    answer(request);
  });
}
