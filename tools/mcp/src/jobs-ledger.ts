import { isObject } from "#mcp/errors";

export const LEDGER_ENTRIES = 256;
const TERMINAL = new Set(["completed", "failed", "canceled", "deadline_exceeded", "unknown"]);
const SUMMARY_FIELDS = [
  "job_id",
  "kind",
  "lane",
  "document_id",
  "source_revision",
  "status",
  "terminal",
  "timing",
];

export type LossCause =
  | "worker_exit"
  | "request_timeout"
  | "client_disconnect"
  | "protocol_failure";

/** How this session's worker was lost, as the adapter observed it. */
export interface WorkerLoss {
  cause: LossCause;
  adapter_killed_worker: boolean;
  exit_code: number | null;
  signal: string | null;
}

interface Entry {
  record: Record<string, unknown>;
  observedMs: number;
  originRequestCancelled: boolean;
}

type JobRecord = Record<string, unknown>;

function isJobRecord(value: unknown): value is JobRecord {
  return (
    isObject(value) &&
    typeof value["job_id"] === "string" &&
    typeof value["kind"] === "string" &&
    typeof value["status"] === "string"
  );
}

/** Every job record or job.list row in a native result. */
function jobRecords(value: unknown): JobRecord[] {
  if (!isObject(value)) return [];
  if (isJobRecord(value)) return [value];
  if (isJobRecord(value["job"])) return [value["job"]];
  const rows: unknown = value["jobs"];
  return Array.isArray(rows) ? rows.filter((row: unknown) => isJobRecord(row)) : [];
}

function isTerminal(record: JobRecord): boolean {
  return TERMINAL.has(String(record["status"]));
}

function summary(record: JobRecord): JobRecord {
  return Object.fromEntries(SUMMARY_FIELDS.map((key) => [key, record[key] ?? null]));
}

/** A per-session record of the jobs this adapter has seen, answering for a lost worker.
 *
 * Only the adapter observes worker loss, so after it `job.status` and `job.list` read
 * the jobs last seen running as `unknown`. MCP request cancellation is recorded here and
 * never becomes native cancellation.
 */
export class JobLedger {
  private readonly entries = new Map<string, Entry>();

  /** Create an empty ledger.
   *
   * Args:
   *   capacity: Entries kept; the oldest terminal entry is evicted first.
   *   clock: Milliseconds for last_observed_ms.
   */
  constructor(
    private readonly capacity = LEDGER_ENTRIES,
    private readonly clock: () => number = () => Date.now(),
  ) {}

  /** Record every job a native result names.
   *
   * Args:
   *   value: A native tool result.
   *   originRequestCancelled: The MCP request that produced it was cancelled.
   */
  observe(value: unknown, originRequestCancelled = false): void {
    for (const record of jobRecords(value)) this.record(record, originRequestCancelled);
  }

  get size(): number {
    return this.entries.size;
  }

  /** Mark job.list rows whose job started from a cancelled MCP request. */
  annotate(value: unknown): unknown {
    if (!isObject(value) || !Array.isArray(value["jobs"])) return value;
    const jobs = value["jobs"].map((row: unknown) =>
      isJobRecord(row) && this.entries.get(row["job_id"] as string)?.originRequestCancelled
        ? { ...row, origin_request_cancelled: true }
        : row,
    );
    return { ...value, jobs };
  }

  /** The job.status answer for a lost worker, or undefined when the job was never seen. */
  status(jobId: string, loss: WorkerLoss): JobRecord | undefined {
    const entry = this.entries.get(jobId);
    return entry ? this.lost(entry, loss) : undefined;
  }

  /** The job.list answer for a lost worker, from the ledger only. */
  list(params: Record<string, unknown>, loss: WorkerLoss): JobRecord {
    const rows = [...this.entries.values()]
      .reverse()
      .map((entry) => this.lost(entry, loss))
      .filter((record) => listed(record, params))
      .map((record) => ({
        ...summary(record),
        observed_by: "adapter_ledger",
        ...(record["origin_request_cancelled"] ? { origin_request_cancelled: true } : {}),
      }));
    const offset = typeof params["offset"] === "number" ? params["offset"] : 0;
    const limit = typeof params["limit"] === "number" ? params["limit"] : 32;
    const page = rows.slice(offset, offset + limit);
    const next = offset + page.length;
    return {
      jobs: page,
      total: rows.length,
      offset,
      next_offset: next < rows.length ? next : null,
      worker: this.worker(),
      limits: { retained_render_jobs: 32, retained_analysis_jobs: 32, active_per_lane: 1 },
      worker_loss: loss,
    };
  }

  // The lost worker's identity, from the last full record that named it.
  private worker(): unknown {
    for (const entry of [...this.entries.values()].reverse()) {
      if (isObject(entry.record["worker"])) return entry.record["worker"];
    }
    return null;
  }

  private record(record: JobRecord, originRequestCancelled: boolean): void {
    const id = record["job_id"] as string;
    const existing = this.entries.get(id);
    if (!existing && !this.makeRoom()) return;
    const full = "worker" in record || !existing;
    this.entries.set(id, {
      record: full ? record : { ...existing.record, ...summary(record) },
      observedMs: this.clock(),
      originRequestCancelled: originRequestCancelled || Boolean(existing?.originRequestCancelled),
    });
  }

  // A full ledger evicts its oldest terminal entry; when every entry is still running
  // it stops recording rather than refusing the call.
  private makeRoom(): boolean {
    if (this.entries.size < this.capacity) return true;
    for (const [id, entry] of this.entries) {
      if (!isTerminal(entry.record)) continue;
      this.entries.delete(id);
      return true;
    }
    return false;
  }

  private lost(entry: Entry, loss: WorkerLoss): JobRecord {
    const record: JobRecord = {
      ...entry.record,
      last_observed_ms: entry.observedMs,
      ...(entry.originRequestCancelled ? { origin_request_cancelled: true } : {}),
    };
    if (isTerminal(entry.record)) return record;
    return {
      ...record,
      status: "unknown",
      terminal: true,
      worker_loss: loss,
      ...(loss.adapter_killed_worker
        ? { termination: { mode: "forced", forced_processes: ["worker"] } }
        : {}),
    };
  }
}

function listed(record: JobRecord, params: Record<string, unknown>): boolean {
  if (params["document_id"] !== undefined && record["document_id"] !== params["document_id"]) {
    return false;
  }
  if (params["kind"] !== undefined && record["kind"] !== params["kind"]) return false;
  const state = params["state"] ?? "any";
  return state === "any" || (state === "terminal") === Boolean(record["terminal"]);
}
