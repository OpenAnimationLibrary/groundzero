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

/** Validate launch grants before spawning the native worker.
 *
 * Args:
 *   args: CLI arguments without the Node executable and script.
 * Returns:
 *   Canonical executable and filesystem roots fixed for this process lifetime.
 */
export async function parseOptions(args: string[]): Promise<WorkerOptions> {
  const { values, tokens } = parseArgs({
    args,
    options: {
      executable: { type: "string" },
      workspace: { type: "string" },
      "input-root": { type: "string", multiple: true },
      "media-tools": { type: "string" },
    },
    strict: true,
    allowPositionals: false,
    tokens: true,
  });
  if (
    tokens.filter((token) => token.kind === "option" && token.name === "media-tools").length > 1
  ) {
    throw new Error("Specify --media-tools at most once");
  }
  if (!values.executable || !values.workspace) {
    throw new Error("Usage: groundzero-mcp --executable ABSOLUTE_FILE --workspace ABSOLUTE_DIR");
  }
  if (!isAbsolute(values.executable)) throw new Error("--executable must be an absolute path");
  const executable = await realpath(values.executable);
  if (!(await lstat(executable)).isFile()) throw new Error("--executable must name a regular file");
  await access(executable, constants.X_OK);
  const workspace = await directory(values.workspace);
  const inputRoots = await Promise.all((values["input-root"] ?? []).map(directory));
  const options: WorkerOptions = { executable, workspace, inputRoots: [...new Set(inputRoots)] };
  if (values["media-tools"] !== undefined) {
    options.mediaToolsDirectory = await mediaDirectory(values["media-tools"]);
  }
  return options;
}
