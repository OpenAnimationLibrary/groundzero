import { constants } from "node:fs";
import { access, lstat, realpath } from "node:fs/promises";
import { isAbsolute, join, resolve } from "node:path";
import { parseArgs } from "node:util";

export interface WorkerOptions {
  executable: string;
  workspace: string;
  inputRoots: string[];
  mediaToolsDirectory?: string;
}

export interface LiveOptions {
  executable: string;
  liveSession: string;
}

export type AdapterOptions = WorkerOptions | LiveOptions;

async function directory(path: string): Promise<string> {
  if (!isAbsolute(path)) throw new Error(`Directory must be absolute: ${path}`);
  const info = await lstat(path);
  if (!info.isDirectory() || info.isSymbolicLink()) {
    throw new Error(`Directory must exist and must not be a symlink: ${path}`);
  }
  const canonical = await realpath(path);
  if (canonical !== resolve(path)) {
    throw new Error(`Use the canonical directory path: ${canonical}`);
  }
  return canonical;
}

async function mediaDirectory(path: string): Promise<string> {
  const canonical = await directory(path);
  const extension = process.platform === "win32" ? ".exe" : "";
  for (const name of ["ffmpeg", "ffprobe"]) {
    const tool = join(canonical, name + extension);
    const info = await lstat(tool);
    if (!info.isFile() || info.isSymbolicLink() || (await realpath(tool)) !== tool) {
      throw new Error(`Media tool must be a canonical regular executable file: ${tool}`);
    }
    await access(tool, constants.X_OK);
  }
  return canonical;
}

function launchArguments(args: string[]) {
  const { values, tokens } = parseArgs({
    args,
    options: {
      executable: { type: "string" },
      workspace: { type: "string" },
      "input-root": { type: "string", multiple: true },
      "media-tools": { type: "string" },
      "live-session": { type: "string" },
    },
    strict: true,
    allowPositionals: false,
    tokens: true,
  });
  for (const name of ["media-tools", "live-session"]) {
    if (tokens.filter((token) => token.kind === "option" && token.name === name).length > 1) {
      throw new Error(`Specify --${name} at most once`);
    }
  }
  if (
    !values.executable ||
    (values.workspace === undefined && values["live-session"] === undefined)
  ) {
    throw new Error(
      "Usage: groundzero-mcp --executable ABSOLUTE_FILE " +
        "(--workspace ABSOLUTE_DIR | --live-session ABSOLUTE_SESSION_FILE)",
    );
  }
  return values;
}

type LaunchArguments = ReturnType<typeof launchArguments>;

function liveOptions(executable: string, values: LaunchArguments): LiveOptions {
  if (
    values.workspace !== undefined ||
    values["input-root"] !== undefined ||
    values["media-tools"] !== undefined
  ) {
    throw new Error("--live-session cannot be combined with workspace, input-root, or media-tools");
  }
  const liveSession = values["live-session"];
  if (liveSession === undefined || !isAbsolute(liveSession) || liveSession.includes("\0")) {
    throw new Error("--live-session must be an absolute session-file path");
  }
  return { executable, liveSession };
}

async function detachedOptions(
  executable: string,
  values: LaunchArguments,
): Promise<WorkerOptions> {
  if (!values.workspace) throw new Error("--workspace must be an absolute directory");
  const workspace = await directory(values.workspace);
  const inputRoots = await Promise.all((values["input-root"] ?? []).map(directory));
  const options: WorkerOptions = { executable, workspace, inputRoots: [...new Set(inputRoots)] };
  if (values["media-tools"] !== undefined) {
    options.mediaToolsDirectory = await mediaDirectory(values["media-tools"]);
  }
  return options;
}

/** Validate the selected native worker or existing-editor connector launch.
 *
 * Args:
 *   args: CLI arguments without the Node executable and script.
 * Returns:
 *   Fixed detached filesystem grants or a trusted executable and explicit live session path.
 */
export async function parseOptions(args: string[]): Promise<AdapterOptions> {
  const values = launchArguments(args);
  if (!values.executable || !isAbsolute(values.executable)) {
    throw new Error("--executable must be an absolute path");
  }
  const executable = await realpath(values.executable);
  if (!(await lstat(executable)).isFile()) throw new Error("--executable must name a regular file");
  await access(executable, constants.X_OK);
  if (values["live-session"] !== undefined) return liveOptions(executable, values);
  return detachedOptions(executable, values);
}
