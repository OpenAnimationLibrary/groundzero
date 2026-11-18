import { spawn } from "node:child_process";
import type { ChildProcess, ChildProcessWithoutNullStreams } from "node:child_process";
import { randomUUID } from "node:crypto";
import { WorkerError, isObject } from "#mcp/errors";
import {
  LIVE_PENDING_REQUESTS,
  LIVE_RECOVERY,
  LIVE_RESPONSE_BYTES,
  decodeLiveJson,
  exactFields,
  liveError,
  liveHello,
  liveRequest,
} from "#mcp/live-protocol";
import type { LiveHello } from "#mcp/live-protocol";
import type { LiveOptions } from "#mcp/options";

interface PendingRequest {
  resolve: (result: unknown) => void;
  reject: (error: Error) => void;
  timeout: ReturnType<typeof setTimeout>;
}

function requirePipes(child: ChildProcess): asserts child is ChildProcessWithoutNullStreams {
  if (!child.stdin || !child.stdout || !child.stderr) {
    throw new Error("The live connector requires private stdio pipes");
  }
}

/** Own one non-GUI connector; the editor and its receipts outlive this connection. */
export class LiveConnection {
  readonly ready: Promise<LiveHello>;
  private readonly child: ChildProcessWithoutNullStreams;
  private readonly pending = new Map<string, PendingRequest>();
  private buffer: Buffer = Buffer.alloc(0);
  private hello: LiveHello | undefined;
  private failure: WorkerError | undefined;
  private resolveHello: ((hello: LiveHello) => void) | undefined;
  private rejectHello: ((error: Error) => void) | undefined;
  private readonly startupTimer: ReturnType<typeof setTimeout>;
  private shutdownTimer: ReturnType<typeof setTimeout> | undefined;

  /** Observe an already-spawned non-GUI connector through private pipes.
   *
   * Args:
   *   child: Trusted native connector process.
   *   timeoutMs: Upper bound for authentication or one native response.
   */
  constructor(
    child: ChildProcess,
    private readonly timeoutMs = 10_000,
  ) {
    requirePipes(child);
    this.child = child;
    this.ready = new Promise((resolve, reject) => {
      this.resolveHello = resolve;
      this.rejectHello = reject;
    });
    this.startupTimer = setTimeout(() => this.fail("Live authentication timed out"), timeoutMs);
    child.stdout.on("data", (chunk: Buffer) => this.receive(chunk));
    child.stdout.on("error", () => this.fail("Cannot read the live connector"));
    child.stdin.on("error", () => this.fail("Cannot write to the live connector"));
    child.on("error", () => this.fail("Cannot start the configured live connector"));
    child.on("exit", () => {
      if (this.shutdownTimer) clearTimeout(this.shutdownTimer);
    });
    // close follows stdout draining, so a final recorded receipt is not discarded on exit.
    child.on("close", (code, signal) => {
      this.fail(`Live connector closed (code ${String(code)}, signal ${String(signal)})`);
    });
  }

  /** Send one catalog operation without retrying edits or canceling a GUI commit.
   *
   * Args:
   *   method: Exact authenticated catalog method.
   *   params: Arguments passed unchanged to native admission.
   * Returns:
   *   The recorded native result, or a structured domain/connection failure.
   */
  request(method: string, params: Record<string, unknown> = {}): Promise<unknown> {
    if (this.failure) return Promise.reject(this.failure);
    if (!this.hello)
      return Promise.reject(new WorkerError("LIVE_NOT_READY", "Await live authentication"));
    if (this.pending.size >= LIVE_PENDING_REQUESTS) {
      return Promise.reject(
        new WorkerError("resource_limit", "Four live requests are already pending", {
          outcome: "not_started",
        }),
      );
    }
    const id = randomUUID();
    let frame: Buffer;
    try {
      frame = liveRequest(id, method, params);
    } catch (error) {
      return Promise.reject(error);
    }
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => this.fail("Live request timed out"), this.timeoutMs);
      this.pending.set(id, { resolve, reject, timeout });
      this.child.stdin.write(frame, (error) => {
        if (error) this.fail("Cannot send a live request");
      });
    });
  }

  /** Disconnect this client; never stop the editor or roll back an admitted edit. */
  close(): void {
    this.fail("MCP live connection closed");
  }

  private receive(chunk: Buffer): void {
    if (this.failure) return;
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (this.buffer.length >= 4) {
      const size = this.buffer.readUInt32BE(0);
      if (size === 0 || size > LIVE_RESPONSE_BYTES) {
        this.fail("Live response exceeds the framed response limit", this.framingCause());
        return;
      }
      if (this.buffer.length < size + 4) return;
      const body = this.buffer.subarray(4, size + 4);
      this.buffer = this.buffer.subarray(size + 4);
      this.accept(body);
      if (this.failure) return;
    }
  }

  private accept(body: Buffer): void {
    try {
      this.respond(decodeLiveJson(body));
    } catch (error) {
      if (!this.hello && error instanceof WorkerError) this.failWith(error);
      else this.fail("Invalid live connector response", this.framingCause());
    }
  }

  private respond(value: unknown): void {
    if (!this.hello) {
      this.hello = liveHello(value);
      clearTimeout(this.startupTimer);
      this.resolveHello?.(this.hello);
      this.resolveHello = undefined;
      this.rejectHello = undefined;
      return;
    }
    if (!isObject(value) || typeof value["id"] !== "string") {
      throw new Error("Expected a live response identity");
    }
    const field = "result" in value ? "result" : "error";
    exactFields(value, ["id", field]);
    const pending = this.pending.get(value["id"]);
    if (!pending) throw new Error("Unknown or repeated live response identity");
    if (field === "error") pending.reject(liveError(value["error"]));
    else pending.resolve(value["result"]);
    clearTimeout(pending.timeout);
    this.pending.delete(value["id"]);
  }

  private framingCause(): Record<string, string> {
    return this.hello ? {} : { cause: "invalid_frame" };
  }

  private fail(message: string, extra: Record<string, unknown> = {}): void {
    this.failWith(
      new WorkerError("LIVE_UNAVAILABLE", message, {
        outcome_unknown: this.pending.size > 0,
        instance_id: this.hello?.instanceId ?? null,
        recovery: LIVE_RECOVERY,
        ...extra,
      }),
    );
  }

  private failWith(failure: WorkerError): void {
    if (this.failure) return;
    this.failure = failure;
    clearTimeout(this.startupTimer);
    this.rejectHello?.(this.failure);
    this.resolveHello = undefined;
    this.rejectHello = undefined;
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
}

/** Attach using the explicitly selected native connector; native code validates the grant and peer.
 *
 * Args:
 *   options: Trusted executable and absolute existing-editor session-file path.
 * Returns:
 *   An authenticated connection promise without starting or owning an editor process.
 */
export function launchLive(options: LiveOptions): LiveConnection {
  const child = spawn(options.executable, ["--automation-live-connect", options.liveSession], {
    stdio: ["pipe", "pipe", "pipe"],
    windowsHide: true,
    shell: false,
  });
  requirePipes(child);
  child.stderr.pipe(process.stderr, { end: false });
  return new LiveConnection(child);
}
