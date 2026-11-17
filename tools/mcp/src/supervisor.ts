import { spawn } from "node:child_process";
import { isAbsolute } from "node:path";
import { superviseNativeChannel } from "#mcp/supervisor-handshake";

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
  const stop = () => {
    if (stopping) return;
    stopping = true;
    // This process remains the group leader until the final signal kills it too.
    // Keeping its PID allocated prevents delayed signaling of a reused group ID.
    signalOwnedGroup("SIGTERM");
    setTimeout(() => signalOwnedGroup("SIGKILL"), 2_000);
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
    if (process.connected) {
      process.send?.({ type: "native-exit", code, signal }, (error: Error | null) => {
        if (error) process.stderr.write(`Cannot report native worker exit: ${error.message}\n`);
      });
    }
    stop();
  });
  superviseNativeChannel(child, stop);
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
