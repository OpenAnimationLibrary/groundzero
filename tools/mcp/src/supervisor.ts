import { spawn } from "node:child_process";
import { isAbsolute } from "node:path";

function supervise(): void {
  const executable = process.argv[2];
  if (
    process.platform === "win32" ||
    !process.connected ||
    !executable ||
    !isAbsolute(executable)
  ) {
    throw new Error("The media supervisor requires a private POSIX launch and absolute executable");
  }
  let stopping = false;
  const stop = () => {
    if (stopping) return;
    stopping = true;
    // This process remains the group leader until the final signal kills it too.
    // Keeping its PID allocated prevents delayed signaling of a reused group ID.
    process.kill(-process.pid, "SIGTERM");
    setTimeout(() => process.kill(-process.pid, "SIGKILL"), 2_000);
  };
  process.on("SIGTERM", stop);
  process.on("SIGINT", stop);
  process.on("disconnect", stop);
  process.on("message", (message: unknown) => {
    if (message !== "stop") process.stderr.write("Invalid private supervisor control message\n");
    stop();
  });
  const child = spawn(executable, process.argv.slice(3), { stdio: "inherit", shell: false });
  child.once("error", (error: Error) => {
    process.stderr.write(`Cannot launch native media worker: ${error.message}\n`);
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
}

try {
  supervise();
} catch (error) {
  process.stderr.write(`Cannot start media supervisor: ${String(error)}\n`);
  process.exitCode = 1;
}
