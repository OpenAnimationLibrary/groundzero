import { spawn } from "node:child_process";
import type { ChildProcessWithoutNullStreams } from "node:child_process";
import { randomUUID } from "node:crypto";
import { WorkerError, isObject } from "#mcp/errors";
import type { WorkerOptions } from "#mcp/options";

export const MAX_FRAME_BYTES = 8 * 1024 * 1024;
const MAX_REQUEST_BYTES = 256 * 1024;
const MAX_PENDING_REQUESTS = 16;
const decoder = new TextDecoder("utf-8", { fatal: true });

interface PendingRequest {
  resolve: (value: unknown) => void;
  reject: (error: Error) => void;
  timeout: ReturnType<typeof setTimeout>;
}

function nativeError(value: unknown): WorkerError {
  if (!isObject(value) || typeof value["code"] !== "string") {
    throw new Error("Expected a structured native error");
  }
  if (typeof value["message"] !== "string" || !isObject(value["details"])) {
    throw new Error("Expected an error message and details object");
  }
  return new WorkerError(value["code"], value["message"], value["details"]);
}

/** A bounded, private NDJSON connection to exactly one native worker process. */
export class WorkerConnection {
  private buffer: Buffer = Buffer.alloc(0);
  private readonly pending = new Map<string, PendingRequest>();
  private failure: WorkerError | undefined;
  private shutdownTimer: ReturnType<typeof setTimeout> | undefined;

  constructor(
    private readonly child: ChildProcessWithoutNullStreams,
    private readonly timeoutMs = 120_000,
  ) {
    child.stdout.on("data", (chunk: Buffer) => this.receive(chunk));
    child.stdout.on("error", (error: Error) => this.fail(error.message));
    child.stdin.on("error", (error: Error) => this.fail(error.message));
    child.on("error", (error: Error) => this.fail(error.message));
    child.on("exit", () => {
      if (this.shutdownTimer) clearTimeout(this.shutdownTimer);
    });
    child.on("close", (code, signal) => {
      this.fail(`Worker exited (code ${String(code)}, signal ${String(signal)})`);
    });
  }

  /** Send one request without retrying an operation whose outcome may be unknown.
   *
   * Args:
   *   method: Exact native operation name.
   *   params: JSON arguments validated against the operation's schema.
   * Returns:
   *   The native result, or a rejected promise carrying a structured WorkerError.
   */
  request(method: string, params: Record<string, unknown> = {}): Promise<unknown> {
    if (this.failure) return Promise.reject(this.failure);
    if (this.pending.size >= MAX_PENDING_REQUESTS) {
      return Promise.reject(new WorkerError("BUSY", "Too many pending native operations"));
    }
    const id = randomUUID();
    const frame = `${JSON.stringify({ id, method, params })}\n`;
    if (Buffer.byteLength(frame) > MAX_REQUEST_BYTES) {
      return Promise.reject(new WorkerError("LIMIT_EXCEEDED", "Request exceeds 256 KiB"));
    }
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(
        () => this.fail(`Timed out waiting for ${method}`),
        this.timeoutMs,
      );
      this.pending.set(id, { resolve, reject, timeout });
      this.child.stdin.write(frame, (error) => {
        if (error) this.fail(`Cannot write native request: ${error.message}`);
      });
    });
  }

  /** Close the private worker when the MCP client disconnects. */
  close(): void {
    this.fail("MCP connection closed");
  }

  private fail(message: string): void {
    if (this.failure) return;
    this.failure = new WorkerError("WORKER_UNAVAILABLE", message, {
      outcome_unknown: this.pending.size > 0,
      recovery: "Inspect saved workspace state before restarting; do not replay writes",
    });
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timeout);
      pending.reject(this.failure);
    }
    this.pending.clear();
    this.buffer = Buffer.alloc(0);
    if (this.child.exitCode !== null || this.child.signalCode !== null) return;
    this.child.kill("SIGTERM");
    this.shutdownTimer = setTimeout(() => this.child.kill("SIGKILL"), 2_000);
    this.shutdownTimer.unref();
  }

  private receive(chunk: Buffer): void {
    if (this.failure) return;
    this.buffer = Buffer.concat([this.buffer, chunk]);
    let newline = this.buffer.indexOf(10);
    while (newline >= 0) {
      if (newline > MAX_FRAME_BYTES) return this.fail("Native response exceeds 8 MiB");
      const frame = this.buffer.subarray(0, newline);
      this.buffer = this.buffer.subarray(newline + 1);
      try {
        this.respond(JSON.parse(decoder.decode(frame)) as unknown);
      } catch (error) {
        return this.fail(`Invalid native response: ${String(error)}`);
      }
      if (this.failure) return;
      newline = this.buffer.indexOf(10);
    }
    if (this.buffer.length > MAX_FRAME_BYTES) this.fail("Native response exceeds 8 MiB");
  }

  private respond(value: unknown): void {
    if (!isObject(value) || typeof value["id"] !== "string") {
      throw new Error("Expected an object with a string request id");
    }
    const pending = this.pending.get(value["id"]);
    if (!pending) throw new Error("Unknown or repeated native request id");
    if ("result" in value === "error" in value) {
      throw new Error("Expected exactly one of result and error");
    }
    if ("error" in value) {
      pending.reject(nativeError(value["error"]));
    } else {
      pending.resolve(value["result"]);
    }
    clearTimeout(pending.timeout);
    this.pending.delete(value["id"]);
  }
}

/** Spawn the configured executable with filesystem grants fixed in its arguments.
 *
 * Args:
 *   options: Validated executable and canonical workspace and input directories.
 * Returns:
 *   A private connection owned by the caller until close.
 */
export function launchWorker(options: WorkerOptions): WorkerConnection {
  const args = ["--automation-worker", "--workspace", options.workspace];
  for (const root of options.inputRoots) args.push("--input-root", root);
  const child = spawn(options.executable, args, {
    cwd: options.workspace,
    stdio: ["pipe", "pipe", "pipe"],
    windowsHide: true,
    shell: false,
  });
  child.stderr.pipe(process.stderr, { end: false });
  return new WorkerConnection(child);
}
