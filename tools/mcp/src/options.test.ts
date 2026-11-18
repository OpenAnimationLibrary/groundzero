import assert from "node:assert/strict";
import {
  chmod,
  mkdir,
  mkdtemp,
  realpath,
  rmdir,
  symlink,
  unlink,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, expect, test } from "vitest";
import { parseOptions } from "#mcp/options";
import { workerArguments } from "#mcp/worker";

const directories: string[] = [];
const links: string[] = [];
const files: string[] = [];

async function workspace(): Promise<string> {
  const path = await realpath(await mkdtemp(join(tmpdir(), "groundzero-options-")));
  directories.push(path);
  return path;
}

afterEach(async () => {
  for (const path of links.splice(0)) await unlink(path);
  for (const path of files.splice(0)) await unlink(path);
  for (const path of directories.splice(0).reverse()) await rmdir(path);
});

function mediaTool(path: string, name: string): string {
  return join(path, name + (process.platform === "win32" ? ".exe" : ""));
}

async function mediaTools(path: string): Promise<void> {
  for (const name of ["ffmpeg", "ffprobe"]) {
    const tool = mediaTool(path, name);
    await writeFile(tool, "fixture", { mode: 0o700 });
    files.push(tool);
  }
}

test("accepts a fixed canonical media pair without adding filesystem read grants", async () => {
  const path = await workspace();
  const media = await workspace();
  await mediaTools(media);
  const options = await parseOptions([
    "--executable",
    process.execPath,
    "--workspace",
    path,
    "--media-tools",
    media,
  ]);
  assert(!("liveSession" in options));
  expect(options.mediaToolsDirectory).toBe(media);
  expect(options.inputRoots).toEqual([]);
});

test("rejects relative, missing, and incomplete media tool directories", async () => {
  const path = await workspace();
  const args = ["--executable", process.execPath, "--workspace", path, "--media-tools"];
  const invalid = "Invalid --media-tools directory: ";
  await expect(parseOptions([...args, "relative"])).rejects.toThrow(
    `${invalid}Directory must be absolute`,
  );
  await expect(parseOptions([...args, ""])).rejects.toThrow(`${invalid}Directory must be absolute`);
  await expect(parseOptions([...args, join(path, "missing")])).rejects.toThrow(invalid);
  await expect(parseOptions([...args, path])).rejects.toThrow(invalid);
  const tool = mediaTool(path, "ffmpeg");
  await writeFile(tool, "fixture", { mode: 0o700 });
  files.push(tool);
  await expect(parseOptions([...args, path])).rejects.toThrow(invalid);
  await expect(parseOptions([...args, path, "--media-tools", path])).rejects.toThrow(
    "at most once",
  );
});

test("rejects symlink media directories and symlink executables", async () => {
  const path = await workspace();
  await mediaTools(path);
  const args = ["--executable", process.execPath, "--workspace", path, "--media-tools"];
  const alias = join(path, "alias");
  await symlink(path, alias, "dir");
  links.push(alias);
  await expect(parseOptions([...args, alias])).rejects.toThrow(/--media-tools .*symlink/);
  const linked = await workspace();
  const tool = mediaTool(linked, "ffmpeg");
  await symlink(mediaTool(path, "ffmpeg"), tool, "file");
  links.push(tool);
  await expect(parseOptions([...args, linked])).rejects.toThrow(
    /--media-tools .*canonical regular/,
  );
});

test.skipIf(process.platform === "win32")("rejects non-executable media tools", async () => {
  const path = await workspace();
  await mediaTools(path);
  await chmod(mediaTool(path, "ffprobe"), 0o600);
  await expect(
    parseOptions(["--executable", process.execPath, "--workspace", path, "--media-tools", path]),
  ).rejects.toThrow("Invalid --media-tools directory");
});

test("requires explicit executable and workspace grants", async () => {
  await expect(parseOptions([])).rejects.toThrow("Usage:");
  await expect(parseOptions(["--executable", "relative"])).rejects.toThrow();
});

test("canonicalizes executable and deduplicates explicit input roots", async () => {
  const path = await workspace();
  const options = await parseOptions([
    "--executable",
    process.execPath,
    "--workspace",
    path,
    "--input-root",
    path,
    "--input-root",
    path,
  ]);
  expect(options).toEqual({
    executable: await realpath(process.execPath),
    workspace: path,
    inputRoots: [path],
    toolsets: ["core"],
  });
});

test("rejects symlink grants and unknown CLI options", async () => {
  const path = await workspace();
  const link = join(path, "alias");
  await symlink(path, link, "dir");
  links.push(link);
  const args = ["--executable", process.execPath, "--workspace", link];
  await expect(parseOptions(args)).rejects.toThrow(/Invalid --workspace directory: .*symlink/);
  await expect(parseOptions([...args, "--allow-everything"])).rejects.toThrow();
});

test("rejects absent directories and relative roots", async () => {
  const path = await workspace();
  const args = ["--executable", process.execPath, "--workspace", path];
  const invalid = "Invalid --input-root directory: ";
  await expect(parseOptions([...args, "--input-root", join(path, "missing")])).rejects.toThrow(
    invalid,
  );
  await expect(parseOptions([...args, "--input-root", "relative"])).rejects.toThrow(
    `${invalid}Directory must be absolute`,
  );
  await expect(
    parseOptions(["--executable", process.execPath, "--workspace", "relative"]),
  ).rejects.toThrow("Invalid --workspace directory: Directory must be absolute");
});

test("selects the trusted non-GUI connector without reading a session grant", async () => {
  const path = await workspace();
  const session = join(path, "session-native-will-validate.json");
  expect(await parseOptions(["--executable", process.execPath, "--live-session", session])).toEqual(
    { executable: await realpath(process.execPath), liveSession: session },
  );
});

test.each(["--workspace", "--input-root", "--media-tools", "--shared-root", "--shared-write"])(
  "rejects detached grants with live attachment: %s",
  async (flag) => {
    const path = await workspace();
    await expect(
      parseOptions([
        "--executable",
        process.execPath,
        "--live-session",
        join(path, "session.json"),
        flag,
        path,
      ]),
    ).rejects.toThrow("cannot be combined");
  },
);

test("live attachment requires an explicit executable and a single absolute session path", async () => {
  const path = await workspace();
  const session = join(path, "session.json");
  await expect(parseOptions(["--live-session", session])).rejects.toThrow("Usage:");
  const prefix = ["--executable", process.execPath, "--live-session"];
  await expect(parseOptions([...prefix, "relative"])).rejects.toThrow("absolute");
  await expect(parseOptions([...prefix, ""])).rejects.toThrow("absolute");
  await expect(parseOptions([...prefix, session, "--live-session", session])).rejects.toThrow(
    "at most once",
  );
});

test("selects core toolsets by default and accepts a distinct toolset list", async () => {
  const path = await workspace();
  const base = ["--executable", process.execPath, "--workspace", path];
  const defaults = await parseOptions(base);
  assert(!("liveSession" in defaults));
  expect(defaults.toolsets).toEqual(["core"]);
  const chosen = await parseOptions([...base, "--toolsets", "audio,rigging"]);
  assert(!("liveSession" in chosen));
  expect(chosen.toolsets).toEqual(["audio", "rigging"]);
  const all = await parseOptions([...base, "--toolsets", "all"]);
  assert(!("liveSession" in all));
  expect(all.toolsets).toEqual(["all"]);
});

test.each(["", "audio,", "Audio", "audio,audio", "audio rigging", "../core"])(
  "rejects the toolset list %j",
  async (value) => {
    const path = await workspace();
    const args = ["--executable", process.execPath, "--workspace", path, "--toolsets", value];
    await expect(parseOptions(args)).rejects.toThrow("--toolsets");
  },
);

test("rejects repeated toolset lists and toolsets for the live editor", async () => {
  const path = await workspace();
  const base = ["--executable", process.execPath, "--workspace", path];
  await expect(
    parseOptions([...base, "--toolsets", "audio", "--toolsets", "rigging"]),
  ).rejects.toThrow("--toolsets");
  await expect(
    parseOptions([
      "--executable",
      process.execPath,
      "--live-session",
      join(path, "session"),
      "--toolsets",
      "audio",
    ]),
  ).rejects.toThrow("toolsets");
});

test("grants explicit shared roots read-only unless a write role is named", async () => {
  const path = await workspace();
  const studio = await workspace();
  const library = await workspace();
  const options = await parseOptions([
    "--executable",
    process.execPath,
    "--workspace",
    path,
    "--shared-root",
    `studio_palettes=${studio}`,
    "--shared-root",
    `library=${library}`,
    "--shared-write",
    "library",
  ]);
  assert(!("liveSession" in options));
  expect(options.sharedRoots).toEqual([
    { role: "studio_palettes", path: studio, writable: false },
    { role: "library", path: library, writable: true },
  ]);
  expect(workerArguments(options)).toEqual([
    "--automation-worker",
    "--workspace",
    path,
    "--shared-root",
    `studio_palettes=${studio}`,
    "--shared-root",
    `library=${library}`,
    "--shared-write",
    "library",
  ]);
});

test("passes no shared roots unless they are granted", async () => {
  const path = await workspace();
  const options = await parseOptions(["--executable", process.execPath, "--workspace", path]);
  assert(!("liveSession" in options));
  expect(options.sharedRoots).toBeUndefined();
  expect(workerArguments(options)).toEqual(["--automation-worker", "--workspace", path]);
});

test("rejects unsafe, overlapping and ambiguous shared root grants", async () => {
  const path = await workspace();
  const studio = await workspace();
  const inside = join(path, "inside");
  await mkdir(inside);
  directories.push(inside);
  const alias = join(path, "alias");
  await symlink(studio, alias, "dir");
  links.push(alias);
  const cases: [string[], string][] = [
    [["--shared-root", studio], "ROLE=ABSOLUTE_DIR"],
    [["--shared-root", `elsewhere=${studio}`], "ROLE=ABSOLUTE_DIR"],
    [["--shared-root", "library=relative"], "Invalid --shared-root library directory"],
    [["--shared-root", `library=${join(path, "missing")}`], "Invalid --shared-root library"],
    [["--shared-root", `library=${alias}`], "symlink"],
    [["--shared-root", `library=${studio}`, "--shared-root", `library=${studio}`], "at most once"],
    [["--shared-root", `library=${inside}`], "overlap"],
    [
      ["--shared-root", `library=${studio}`, "--shared-root", `studio_palettes=${studio}`],
      "overlap",
    ],
    [["--shared-write", "library"], "configured"],
    [
      [
        "--shared-root",
        `library=${studio}`,
        "--shared-write",
        "library",
        "--shared-write",
        "library",
      ],
      "configured",
    ],
  ];
  for (const [extra, message] of cases) {
    await expect(
      parseOptions(["--executable", process.execPath, "--workspace", path, ...extra]),
    ).rejects.toThrow(message);
  }
});
