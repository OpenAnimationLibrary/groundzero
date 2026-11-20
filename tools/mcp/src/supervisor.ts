import { execFile, spawn } from "node:child_process";
import { isAbsolute } from "node:path";
import { setTimeout as pause } from "node:timers/promises";
import { HANDSHAKE_LIMIT_MS, superviseNativeChannel } from "#mcp/supervisor-handshake";

const KILL_GRACE_MS = 2_000;
const GROUP_CHECK_DELAYS_MS = [10, 40, 100, 250, 500];

function supervise(): void {
  const executable = process.argv[2];
  if (
    process.platform === "win32" ||
    !process.connected ||
    !executable ||
    !isAbsolute(executable)
  ) {
    throw new Error(
      "The helper supervisor requires a private POSIX launch and absolute executable",
    );
  }
  let stopping = false;
  let reported: Promise<void> | undefined;
  const stop = () => {
    if (stopping) return;
    stopping = true;
    // This process remains the group leader until the group is empty or the final signal
    // kills it too. Keeping its PID allocated prevents delayed signaling of a reused group ID.
    signalOwnedGroup("SIGTERM");
    const kill = setTimeout(() => signalOwnedGroup("SIGKILL"), KILL_GRACE_MS);
    void releaseWhenAlone(kill, () => reported);
  };
  process.on("SIGTERM", stop);
  process.on("SIGINT", stop);
  process.on("disconnect", stop);
  process.on("message", (message: unknown) => {
    if (message !== "stop") process.stderr.write("Invalid private supervisor control message\n");
    stop();
  });
  const child = spawn(executable, process.argv.slice(3), {
    stdio: ["inherit", "inherit", "inherit", "pipe"],
    env: { ...process.env, GROUNDZERO_AUTOMATION_CONTROL_FD: "3" },
    shell: false,
  });
  child.once("error", (error: Error) => {
    process.stderr.write(`Cannot launch native worker: ${error.message}\n`);
    stop();
  });
  child.once("exit", (code, signal) => {
    reported = new Promise((resolve) => {
      if (!process.connected) return resolve();
      process.send?.({ type: "native-exit", code, signal }, (error: Error | null) => {
        if (error) process.stderr.write(`Cannot report native worker exit: ${error.message}\n`);
        resolve();
      });
    });
    stop();
  });
  const override = Number(process.env["GROUNDZERO_SUPERVISOR_HANDSHAKE_MS"]);
  superviseNativeChannel(
    child,
    stop,
    Number.isSafeInteger(override) && override > 0 ? override : HANDSHAKE_LIMIT_MS,
  );
}

/** Send the final signal as soon as the native worker has exited and reported and nothing
 * else is left in the group, instead of after the whole grace period.
 *
 * Args:
 *   kill: The pending final SIGKILL, left to run while anything else is alive.
 *   reported: The native exit report, undefined until that exit is observed.
 */
async function releaseWhenAlone(
  kill: ReturnType<typeof setTimeout>,
  reported: () => Promise<void> | undefined,
): Promise<void> {
  for (const delay of GROUP_CHECK_DELAYS_MS) {
    await pause(delay);
    const report = reported();
    if (report === undefined || (await groupHasOthers())) continue;
    clearTimeout(kill);
    await report;
    signalOwnedGroup("SIGKILL");
  }
}

/** Whether a process other than this one is in its group; true when it cannot be told. */
function groupHasOthers(): Promise<boolean> {
  return new Promise((resolve) => {
    const lister = execFile(
      "/bin/ps",
      ["-A", "-o", "pid=,pgid="],
      { env: {}, timeout: 1_000 },
      (error, stdout) => {
        if (error) {
          resolve(true);
          return;
        }
        const members = stdout
          .trim()
          .split("\n")
          .map((line) => line.trim().split(/\s+/).map(Number))
          .filter(([, group]) => group === process.pid)
          .map(([pid]) => pid);
        resolve(members.some((pid) => pid !== process.pid && pid !== lister.pid));
      },
    );
  });
}

function signalOwnedGroup(signal: NodeJS.Signals): void {
  try {
    process.kill(-process.pid, signal);
  } catch {
    process.exit(1);
  }
}

try {
  supervise();
} catch (error) {
  process.stderr.write(`Cannot start helper supervisor: ${String(error)}\n`);
  process.exitCode = 1;
}
