import { chmod, mkdtemp, realpath, rmdir, symlink, unlink, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, expect, test } from "vitest";
import { parseOptions } from "#mcp/options";

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
  expect(options.mediaToolsDirectory).toBe(media);
  expect(options.inputRoots).toEqual([]);
});

test("rejects relative, missing, and incomplete media tool directories", async () => {
  const path = await workspace();
  const args = ["--executable", process.execPath, "--workspace", path, "--media-tools"];
  await expect(parseOptions([...args, "relative"])).rejects.toThrow("absolute");
  await expect(parseOptions([...args, ""])).rejects.toThrow("absolute");
  await expect(parseOptions([...args, join(path, "missing")])).rejects.toThrow();
  await expect(parseOptions([...args, path])).rejects.toThrow();
  const tool = mediaTool(path, "ffmpeg");
  await writeFile(tool, "fixture", { mode: 0o700 });
  files.push(tool);
  await expect(parseOptions([...args, path])).rejects.toThrow();
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
  await expect(parseOptions([...args, alias])).rejects.toThrow("symlink");
  const linked = await workspace();
  const tool = mediaTool(linked, "ffmpeg");
  await symlink(mediaTool(path, "ffmpeg"), tool, "file");
  links.push(tool);
  await expect(parseOptions([...args, linked])).rejects.toThrow("canonical regular");
});

test.skipIf(process.platform === "win32")("rejects non-executable media tools", async () => {
  const path = await workspace();
  await mediaTools(path);
  await chmod(mediaTool(path, "ffprobe"), 0o600);
  await expect(
    parseOptions(["--executable", process.execPath, "--workspace", path, "--media-tools", path]),
  ).rejects.toThrow();
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
  });
});

test("rejects symlink grants and unknown CLI options", async () => {
  const path = await workspace();
  const link = join(path, "alias");
  await symlink(path, link, "dir");
  links.push(link);
  const args = ["--executable", process.execPath, "--workspace", link];
  await expect(parseOptions(args)).rejects.toThrow("symlink");
  await expect(parseOptions([...args, "--allow-everything"])).rejects.toThrow();
});

test("rejects absent directories and relative roots", async () => {
  const path = await workspace();
  const args = ["--executable", process.execPath, "--workspace", path];
  await expect(parseOptions([...args, "--input-root", join(path, "missing")])).rejects.toThrow();
  await expect(parseOptions([...args, "--input-root", "relative"])).rejects.toThrow("absolute");
});
