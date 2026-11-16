import { mkdtemp, realpath, rmdir, symlink, unlink } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, expect, test } from "vitest";
import { parseOptions } from "#mcp/options";

const directories: string[] = [];
const links: string[] = [];

async function workspace(): Promise<string> {
  const path = await realpath(await mkdtemp(join(tmpdir(), "groundzero-options-")));
  directories.push(path);
  return path;
}

afterEach(async () => {
  for (const path of links.splice(0)) await unlink(path);
  for (const path of directories.splice(0).reverse()) await rmdir(path);
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
