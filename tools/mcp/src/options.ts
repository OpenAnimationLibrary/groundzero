import { constants } from "node:fs";
import { access, lstat, realpath } from "node:fs/promises";
import { isAbsolute, join, resolve, sep } from "node:path";
import { parseArgs } from "node:util";

export interface SharedRoot {
  role: string;
  path: string;
  writable: boolean;
}

export interface WorkerOptions {
  executable: string;
  workspace: string;
  inputRoots: string[];
  mediaToolsDirectory?: string;
  sharedRoots?: SharedRoot[];
}

const SHARED_ROLES = new Set(["studio_palettes", "project_palettes", "library"]);

export interface DetachedOptions extends WorkerOptions {
  toolsets: string[];
  /** Refuse results outside their output schema instead of flagging them (tests). */
  strictResults: boolean;
}

export interface LiveOptions {
  executable: string;
  liveSession: string;
  strictResults: boolean;
}

export type AdapterOptions = DetachedOptions | LiveOptions;

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
      "shared-root": { type: "string", multiple: true },
      "shared-write": { type: "string", multiple: true },
      "live-session": { type: "string" },
      toolsets: { type: "string" },
      "strict-results": { type: "boolean" },
    },
    strict: true,
    allowPositionals: false,
    tokens: true,
  });
  for (const name of ["media-tools", "live-session", "toolsets"]) {
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

const DETACHED_ONLY = [
  "workspace",
  "input-root",
  "media-tools",
  "shared-root",
  "shared-write",
  "toolsets",
] as const;

function liveOptions(executable: string, values: LaunchArguments): LiveOptions {
  if (DETACHED_ONLY.some((name) => values[name] !== undefined)) {
    throw new Error(
      "--live-session cannot be combined with workspace, input-root, media-tools, " +
        "shared-root, shared-write, or toolsets",
    );
  }
  const liveSession = values["live-session"];
  if (liveSession === undefined || !isAbsolute(liveSession) || liveSession.includes("\0")) {
    throw new Error("--live-session must be an absolute session-file path");
  }
  return { executable, liveSession, strictResults: values["strict-results"] === true };
}

function toolsetNames(value: string | undefined): string[] {
  if (value === undefined) return ["core"];
  const names = value.split(",");
  if (
    names.length > 64 ||
    names.some((name) => !/^[a-z][a-z0-9-]{0,31}$/.test(name)) ||
    new Set(names).size !== names.length
  ) {
    throw new Error("--toolsets must be a comma-separated list of distinct toolset names, or all");
  }
  return names;
}

function overlapping(first: string, second: string): boolean {
  const within = (path: string, root: string) =>
    path === root || path.startsWith(root.endsWith(sep) ? root : root + sep);
  return within(first, second) || within(second, first);
}

async function granted(flag: string, check: Promise<string>): Promise<string> {
  try {
    return await check;
  } catch (error) {
    const reason = error instanceof Error ? error.message : String(error);
    throw new Error(`Invalid ${flag} directory: ${reason}`, { cause: error });
  }
}

async function sharedRootGrants(values: LaunchArguments): Promise<Map<string, string>> {
  const roots = new Map<string, string>();
  for (const grant of values["shared-root"] ?? []) {
    const separator = grant.indexOf("=");
    const role = grant.slice(0, Math.max(separator, 0));
    if (!SHARED_ROLES.has(role)) {
      throw new Error(
        "--shared-root requires ROLE=ABSOLUTE_DIR with ROLE studio_palettes, " +
          "project_palettes or library",
      );
    }
    if (roots.has(role)) throw new Error(`Specify --shared-root ${role} at most once`);
    roots.set(role, await granted(`--shared-root ${role}`, directory(grant.slice(separator + 1))));
  }
  return roots;
}

async function sharedRoots(
  values: LaunchArguments,
  workspace: string,
): Promise<SharedRoot[] | undefined> {
  const roots = await sharedRootGrants(values);
  const writes = values["shared-write"] ?? [];
  if (new Set(writes).size !== writes.length || writes.some((role) => !roots.has(role))) {
    throw new Error("--shared-write must name a configured --shared-root role at most once");
  }
  const paths = [workspace, ...roots.values()];
  for (const [index, path] of paths.entries()) {
    if (paths.slice(index + 1).some((other) => overlapping(path, other))) {
      throw new Error("--shared-root must not overlap the workspace or another shared root");
    }
  }
  if (roots.size === 0) return undefined;
  return [...roots].map(([role, path]) => ({ role, path, writable: writes.includes(role) }));
}

async function detachedOptions(
  executable: string,
  values: LaunchArguments,
): Promise<DetachedOptions> {
  if (!values.workspace) throw new Error("--workspace must be an absolute directory");
  const workspace = await granted("--workspace", directory(values.workspace));
  const inputRoots = await Promise.all(
    (values["input-root"] ?? []).map((root) => granted("--input-root", directory(root))),
  );
  const options: DetachedOptions = {
    executable,
    workspace,
    inputRoots: [...new Set(inputRoots)],
    toolsets: toolsetNames(values.toolsets),
    strictResults: values["strict-results"] === true,
  };
  if (values["media-tools"] !== undefined) {
    options.mediaToolsDirectory = await granted(
      "--media-tools",
      mediaDirectory(values["media-tools"]),
    );
  }
  const shared = await sharedRoots(values, workspace);
  if (shared !== undefined) options.sharedRoots = shared;
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
