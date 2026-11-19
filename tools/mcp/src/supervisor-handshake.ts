import type { ChildProcess } from "node:child_process";
import { randomBytes } from "node:crypto";
import { Duplex } from "node:stream";

/** Time from launching the native worker to its verified handshake. The clock starts before the
 * operating system loads the executable and its libraries, so a cold start on a loaded machine
 * spends most of it there; the native side bounds only its own exchange at five seconds. */
export const HANDSHAKE_LIMIT_MS = 30_000;

/** Verify the private native channel and retain it until the worker exits.
 *
 * Args:
 *   child: The direct native child of this process-group anchor.
 *   stop: Terminate the owned process group after channel or ownership failure.
 *   limitMs: How long the whole handshake may take, measured from now.
 */
export function superviseNativeChannel(
  child: ChildProcess,
  stop: () => void,
  limitMs = HANDSHAKE_LIMIT_MS,
): void {
  const candidate = child.stdio[3];
  const pid = child.pid;
  if (!(candidate instanceof Duplex) || pid === undefined) {
    process.stderr.write("Native helper supervision requires its private duplex channel\n");
    stop();
    return;
  }
  const channel = candidate;
  const frame = Buffer.alloc(44);
  frame.writeUInt32LE(0x475a4153, 0);
  frame.writeUInt32LE(1, 4);
  frame.writeUInt32LE(process.pid, 8);
  randomBytes(32).copy(frame, 12);
  const started = performance.now();
  const deadline = started + limitMs;
  const timeout = () =>
    `Native supervisor handshake timed out after ${Math.round(performance.now() - started)} ms ` +
    `(limit ${limitMs} ms); the worker did not answer in time`;
  let received: Buffer = Buffer.alloc(0);
  let verified = false;
  let failed = false;
  const timer = setTimeout(() => fail(timeout()), limitMs);
  function fail(message: string): void {
    if (failed) return;
    failed = true;
    clearTimeout(timer);
    process.stderr.write(`${message}\n`);
    channel.destroy();
    stop();
  }
  channel.on("data", (chunk: Buffer) => {
    if (failed) return;
    if (performance.now() >= deadline) {
      fail(timeout());
      return;
    }
    if (verified || received.length + chunk.length > frame.length) {
      fail("Native supervisor channel sent unexpected bytes");
      return;
    }
    received = Buffer.concat([received, chunk]);
    if (received.length !== frame.length) return;
    if (!received.equals(frame) || !sameChild(child, pid)) {
      fail("Native supervisor handshake did not match its child and nonce");
      return;
    }
    verified = true;
    channel.write(Buffer.from([1]), (error: Error | null | undefined) => {
      if (error) fail(`Cannot acknowledge native supervisor handshake: ${error.message}`);
      else if (performance.now() >= deadline) fail(timeout());
      else clearTimeout(timer);
    });
  });
  channel.once("error", (error: Error) =>
    fail(`Native supervisor channel failed: ${error.message}`),
  );
  channel.once("end", () => {
    if (!verified) fail("Native supervisor channel ended before its handshake");
    else stop();
  });
  child.once("exit", () => clearTimeout(timer));
  channel.write(frame, (error: Error | null | undefined) => {
    if (error) fail(`Cannot start native supervisor handshake: ${error.message}`);
  });
}

function sameChild(child: ChildProcess, pid: number): boolean {
  return child.pid === pid && child.exitCode === null && child.signalCode === null;
}
