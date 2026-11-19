import { spawn } from "node:child_process";
import type { ChildProcess } from "node:child_process";
import type { ChildProcessWithoutNullStreams } from "node:child_process";
import { randomUUID } from "node:crypto";
import { fileURLToPath } from "node:url";
import { WorkerError, isObject } from "#mcp/errors";
import type { LossCause, WorkerLoss } from "#mcp/jobs-ledger";
import type { WorkerOptions } from "#mcp/options";

export const MAX_FRAME_BYTES = 8 * 1024 * 1024;
const MAX_REQUEST_BYTES = 256 * 1024;
const MAX_PENDING_REQUESTS = 16;
// Longer than the native worker's own 3 s shutdown deadline after end of input, so the
// worker ends itself, having removed what it must, before any signal is sent.
export const CLOSE_GRACE_MS = 4_000;
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
  private readonly child: ChildProcessWithoutNullStreams;
  private buffer: Buffer = Buffer.alloc(0);
  private readonly pending = new Map<string, PendingRequest>();
  private failure: WorkerError | undefined;
  private shutdownTimer: ReturnType<typeof setTimeout> | undefined;
  private nativeExit: string | undefined;
  private lossCause: LossCause | undefined;
  private killed = false;
  private exitCode: number | null = null;
  private exitSignal: string | null = null;

  /** Own a started worker or supervisor, the leader of its own process group.
   *
   * Args:
   *   child: The process this adapter started.
   *   timeoutMs: Upper bound for one native response.
   *   supervised: Whether the child is the POSIX helper supervisor.
   */
  constructor(
    child: ChildProcess,
    private readonly timeoutMs = 120_000,
    private readonly supervised = false,
  ) {
    requirePipes(child);
    this.child = child;
    child.stdout.on("data", (chunk: Buffer) => this.receive(chunk));
    child.stdout.on("error", (error: Error) => this.fail(error.message));
    child.stdin.on("error", (error: Error) => this.fail(error.message));
    child.on("error", (error: Error) => this.fail(error.message, "worker_exit"));
    child.on("message", (message: unknown) => {
      if (!isObject(message) || message["type"] !== "native-exit") return;
      this.nativeExit =
        `Native worker exited (code ${String(message["code"])}, ` +
        `signal ${String(message["signal"])})`;
    });
    child.on("exit", () => {
      if (this.shutdownTimer) clearTimeout(this.shutdownTimer);
    });
    child.on("close", (code, signal) => {
      this.exitCode = code;
      this.exitSignal = signal;
      this.fail(
        this.nativeExit ?? `Worker exited (code ${String(code)}, signal ${String(signal)})`,
        "worker_exit",
      );
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
        () => this.fail(`Timed out waiting for ${method}`, "request_timeout"),
        this.timeoutMs,
      );
      this.pending.set(id, { resolve, reject, timeout });
      this.child.stdin.write(frame, (error) => {
        if (error) this.fail(`Cannot write native request: ${error.message}`);
      });
    });
  }

  /** Close the private worker when the MCP client disconnects.
   *
   * The worker sees the end of its input, as when run directly, so it can cancel renders and
   * remove their partial output itself; it is stopped by signal only after a short grace period.
   */
  close(): void {
    this.fail("MCP connection closed", "client_disconnect", true);
  }

  /** How the worker was lost, once it was; undefined while it serves. */
  loss(): WorkerLoss | undefined {
    if (!this.lossCause) return undefined;
    return {
      cause: this.lossCause,
      adapter_killed_worker: this.killed,
      exit_code: this.exitCode,
      signal: this.exitSignal,
    };
  }

  private fail(message: string, cause: LossCause = "protocol_failure", graceful = false): void {
    if (this.failure) return;
    this.lossCause = cause;
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
    if (!graceful) {
      this.stop();
      return;
    }
    this.child.stdin.end();
    this.shutdownTimer = setTimeout(() => this.stop(), CLOSE_GRACE_MS);
    this.shutdownTimer.unref();
  }

  private stop(): void {
    if (this.supervised) {
      this.stopSupervisor();
      return;
    }
    this.signal("SIGTERM");
    this.shutdownTimer = setTimeout(() => this.signal("SIGKILL"), 2_000);
    this.shutdownTimer.unref();
  }

  private stopSupervisor(): void {
    if (!this.child.connected) {
      this.signal("SIGTERM");
      return;
    }
    this.killed = true;
    this.child.send("stop", (error: Error | null) => {
      if (error) this.signal("SIGTERM");
    });
  }

  // Signals go only through the child's handle and only before it is reaped: an unreaped
  // child keeps its process ID, and so the ID of the group it leads, from being reused.
  // Node reaps a child and records its exit status in one step, so there is no window.
  private signal(name: NodeJS.Signals): void {
    if (this.child.exitCode !== null || this.child.signalCode !== null) return;
    this.killed = true;
    this.child.kill(name);
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

function requirePipes(child: ChildProcess): asserts child is ChildProcessWithoutNullStreams {
  if (!child.stdin || !child.stdout || !child.stderr) {
    throw new Error("The native worker requires private stdio pipes");
  }
}

/** Return the native worker arguments that fix every filesystem grant at launch.
 *
 * Args:
 *   options: Validated canonical workspace, input, media and shared-root grants.
 * Returns:
 *   Arguments after the executable: the launch authority, always explicit, then the roots,
 *   read-only shared roots unless granted for writing.
 */
export function workerArguments(options: WorkerOptions): string[] {
  const args = ["--automation-worker", "--workspace", options.workspace];
  args.push("--grants", options.grants.join(","), "--domains", options.domains.join(","));
  for (const root of options.inputRoots) args.push("--input-root", root);
  if (options.mediaToolsDirectory) args.push("--media-tools", options.mediaToolsDirectory);
  for (const root of options.sharedRoots ?? []) {
    args.push("--shared-root", `${root.role}=${root.path}`);
  }
  for (const root of options.sharedRoots ?? []) {
    if (root.writable) args.push("--shared-write", root.role);
  }
  return args;
}

/** Spawn the configured executable with filesystem grants fixed in its arguments.
 *
 * Args:
 *   options: Validated executable and canonical workspace and input directories.
 * Returns:
 *   A private connection owned by the caller until close.
 */
export function launchWorker(options: WorkerOptions): WorkerConnection {
  const args = workerArguments(options);
  const supervised = process.platform !== "win32";
  const command = supervised ? process.execPath : options.executable;
  const launchArgs = supervised
    ? [fileURLToPath(new URL("./supervisor.js", import.meta.url)), options.executable, ...args]
    : args;
  const child = spawn(command, launchArgs, {
    cwd: options.workspace,
    stdio: supervised ? ["pipe", "pipe", "pipe", "ipc"] : ["pipe", "pipe", "pipe"],
    detached: supervised,
    windowsHide: true,
    shell: false,
  });
  requirePipes(child);
  child.stderr.pipe(process.stderr, { end: false });
  return new WorkerConnection(child, 120_000, supervised);
}
