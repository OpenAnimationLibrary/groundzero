import { Client, InMemoryTransport } from "@modelcontextprotocol/client";
import { afterEach, expect, test, vi } from "vitest";
import { WorkerError } from "#mcp/errors";
import { JobLedger } from "#mcp/jobs-ledger";
import type { WorkerLoss } from "#mcp/jobs-ledger";
import { createServer } from "#mcp/server";

const exit: WorkerLoss = {
  cause: "worker_exit",
  adapter_killed_worker: false,
  exit_code: null,
  signal: "SIGKILL",
};
const timeout: WorkerLoss = {
  cause: "request_timeout",
  adapter_killed_worker: true,
  exit_code: null,
  signal: "SIGTERM",
};

function record(id: string, status: string, extra: Record<string, unknown> = {}) {
  return {
    job_id: id,
    kind: "render_preview",
    lane: "render",
    document_id: "document_a",
    source_revision: "1",
    status,
    terminal: ["completed", "failed", "canceled", "deadline_exceeded"].includes(status),
    timing: { submitted_ms: 1, started_ms: null, ended_ms: null },
    worker: { instance_id: "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa" },
    termination: { mode: null, forced_processes: [] },
    ...extra,
  };
}

test("reports unknown after worker loss and keeps terminal outcomes", () => {
  const ledger = new JobLedger(256, () => 42);
  ledger.observe(record("job_running", "running"));
  ledger.observe({ job: record("job_done", "completed"), artifacts: [] });
  expect(ledger.status("job_running", exit)).toMatchObject({
    status: "unknown",
    terminal: true,
    worker_loss: exit,
    last_observed_ms: 42,
    termination: { mode: null, forced_processes: [] },
  });
  expect(ledger.status("job_done", exit)).toMatchObject({ status: "completed" });
  expect(ledger.status("job_never_seen", exit)).toBeUndefined();
  expect(ledger.status("job_running", timeout)).toMatchObject({
    worker_loss: { cause: "request_timeout", adapter_killed_worker: true },
    termination: { mode: "forced", forced_processes: ["worker"] },
  });
});

test("evicts the oldest terminal entry and stops recording when every entry runs", () => {
  const ledger = new JobLedger(2);
  ledger.observe(record("job_old_done", "completed"));
  ledger.observe(record("job_running", "running"));
  ledger.observe(record("job_new", "running"));
  expect(ledger.status("job_old_done", exit)).toBeUndefined();
  expect(ledger.status("job_new", exit)).toMatchObject({ status: "unknown" });
  ledger.observe(record("job_ignored", "running"));
  expect(ledger.size).toBe(2);
  expect(ledger.status("job_ignored", exit)).toBeUndefined();
  ledger.observe(record("job_running", "completed"));
  expect(ledger.status("job_running", exit)).toMatchObject({ status: "completed" });
});

test("lists ledger rows newest first with filters, paging and their observer", () => {
  const ledger = new JobLedger();
  ledger.observe({ jobs: [record("job_first", "completed")] });
  ledger.observe(record("job_second", "running", { kind: "render_sequence" }));
  const listed = ledger.list({ limit: 1 }, exit);
  expect(listed).toMatchObject({ total: 2, offset: 0, next_offset: 1, worker_loss: exit });
  expect(listed["jobs"]).toEqual([
    {
      job_id: "job_second",
      kind: "render_sequence",
      lane: "render",
      document_id: "document_a",
      source_revision: "1",
      status: "unknown",
      terminal: true,
      timing: { submitted_ms: 1, started_ms: null, ended_ms: null },
      observed_by: "adapter_ledger",
    },
  ]);
  expect(ledger.list({ kind: "render_preview" }, exit)["total"]).toBe(1);
  expect(ledger.list({ document_id: "document_b" }, exit)["total"]).toBe(0);
});

const jobTools = ["render.preview", "job.status", "job.list", "job.cancel", "job.result"].map(
  (name) => ({
    name,
    description: name,
    inputSchema: { type: "object", properties: {}, additionalProperties: true },
    annotations: { readOnlyHint: !name.includes("preview") && name !== "job.cancel" },
    toolset: "core",
    mutates_document: false,
  }),
);
const closers: (() => Promise<void>)[] = [];

afterEach(async () => {
  for (const close of closers.splice(0)) await close();
});

async function jobSession(answer: (method: string) => Promise<unknown>) {
  let loss: WorkerLoss | undefined;
  const request = vi.fn(async (method: string): Promise<unknown> => {
    if (method === "automation.capabilities") {
      return {
        protocol_version: 3,
        tools: jobTools,
        next_offset: null,
        toolsets: [{ name: "core", description: "Core", tools: jobTools.map((t) => t.name) }],
      };
    }
    return answer(method);
  });
  const server = createServer({ request, close: () => {}, loss: () => loss });
  const client = new Client({ name: "ledger-test", version: "1.0.0" });
  const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
  await server.connect(serverTransport);
  await client.connect(clientTransport);
  closers.push(async () => {
    await client.close();
    await server.close();
  });
  return { client, request, lose: (value: WorkerLoss) => (loss = value) };
}

const unavailable = () => Promise.reject(new WorkerError("WORKER_UNAVAILABLE", "Worker exited"));

test("crash recovery answers status and list from the ledger and nothing else", async () => {
  let answer: (method: string) => Promise<unknown> = async () => record("job_a", "running");
  const { client, lose } = await jobSession((method) => answer(method));
  await client.callTool({ name: "render.preview", arguments: {} });
  answer = unavailable;
  lose(exit);
  const status = await client.callTool({ name: "job.status", arguments: { job_id: "job_a" } });
  expect(status.structuredContent).toMatchObject({ status: "unknown", worker_loss: exit });
  const listed = await client.callTool({ name: "job.list", arguments: {} });
  expect(listed.structuredContent).toMatchObject({
    jobs: [{ job_id: "job_a", status: "unknown", observed_by: "adapter_ledger" }],
  });
  const result = await client.callTool({ name: "job.result", arguments: { job_id: "job_a" } });
  expect(result.structuredContent).toMatchObject({ error: { code: "WORKER_UNAVAILABLE" } });
  const unseen = await client.callTool({ name: "job.status", arguments: { job_id: "job_b" } });
  expect(unseen.structuredContent).toMatchObject({ error: { code: "WORKER_UNAVAILABLE" } });
});

test("an MCP-cancelled job start is recorded, listed and never cancelled natively", async () => {
  let release: (value: unknown) => void = () => {};
  const late = new Promise((resolve) => (release = resolve));
  const { client, request } = await jobSession(async (method) => {
    if (method === "render.preview") return late;
    return { jobs: [record("job_late", "running")], total: 1 };
  });
  const abort = new AbortController();
  const call = client.callTool({ name: "render.preview", arguments: {} }, { signal: abort.signal });
  await vi.waitFor(() => expect(request).toHaveBeenCalledWith("render.preview", {}));
  abort.abort("test");
  await expect(call).rejects.toThrow();
  release(record("job_late", "preparing"));
  await vi.waitFor(async () => {
    const listed = await client.callTool({ name: "job.list", arguments: {} });
    expect(listed.structuredContent).toMatchObject({
      jobs: [{ job_id: "job_late", origin_request_cancelled: true }],
    });
  });
  expect(request.mock.calls.map(([method]) => method)).not.toContain("job.cancel");
});
