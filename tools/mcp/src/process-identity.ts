import { execFileSync } from "node:child_process";

/** Return a process's start time, or undefined when no such process exists. */
export type StartProbe = (pid: number) => string | undefined;

// Windows keeps a process ID allocated while its handle is open, and Node holds the
// handle until exit, so the ID itself identifies the process there.
const WINDOWS_HANDLE = "held process handle";

/** Read a POSIX process's start time from ps; undefined when it is gone or unreadable. */
export const processStart: StartProbe = (pid) => {
  if (process.platform === "win32") return WINDOWS_HANDLE;
  try {
    const started = execFileSync("/bin/ps", ["-o", "lstart=", "-p", String(pid)], {
      encoding: "utf8",
      env: {},
      timeout: 1_000,
    }).trim();
    return started || undefined;
  } catch {
    return undefined;
  }
};

/** A process started by this adapter, known by its ID and start time. */
export class ProcessIdentity {
  private constructor(
    readonly pid: number,
    private readonly started: string,
  ) {}

  /** Record a just-started process; undefined when it cannot be identified, so it is
   * never signalled.
   *
   * Args:
   *   pid: The child's process ID.
   *   probe: Reads a process's start time.
   * Returns:
   *   The identity, or undefined.
   */
  static of(pid: number | undefined, probe: StartProbe): ProcessIdentity | undefined {
    if (pid === undefined) return undefined;
    const started = probe(pid);
    return started === undefined ? undefined : new ProcessIdentity(pid, started);
  }

  /** Whether the process with this ID is still the one recorded, not a recycled ID. */
  isCurrent(probe: StartProbe): boolean {
    return probe(this.pid) === this.started;
  }
}
