import { Client } from "@modelcontextprotocol/client";
import { StdioClientTransport } from "@modelcontextprotocol/client/stdio";
import { execFileSync, spawnSync } from "node:child_process";
import { mkdir, readFile, rm } from "node:fs/promises";
import { dirname, join } from "node:path";
import { setTimeout as pause } from "node:timers/promises";
import { fileURLToPath } from "node:url";
import { afterEach, expect, test } from "vitest";

interface DocumentedClient {
  command: string;
  args: string[];
  text: string;
  addCommand: string;
  toolTimeoutSec?: number;
}

interface Fixtures {
  fake: { fakeNativeExecutable: () => Promise<{ directory: string; executable: string }> };
  readme: {
    documentedClients: (readme: string) => Record<"claude" | "codex", DocumentedClient>;
    substituteArguments: (args: string[], paths: Record<string, string>) => string[];
  };
}

const fixtures = {
  fake: await import(new URL("../fixtures/fake-native.mjs", import.meta.url).href),
  readme: await import(new URL("../fixtures/readme-configs.mjs", import.meta.url).href),
} as Fixtures;
const readmeText = await readFile(new URL("../README.md", import.meta.url), "utf8");
const documented = fixtures.readme.documentedClients(readmeText);
const cli = fileURLToPath(new URL("../dist/cli.js", import.meta.url));
const posix = test.skipIf(process.platform === "win32");
const scratch: string[] = [];

afterEach(async () => {
  for (const directory of scratch.splice(0)) await rm(directory, { recursive: true, force: true });
});

interface Grants {
  paths: Record<string, string>;
  directory: string;
}

async function grants(): Promise<Grants> {
  const { directory, executable } = await fixtures.fake.fakeNativeExecutable();
  scratch.push(directory);
  const workspace = join(directory, "animation");
  const inputRoot = join(directory, "reference");
  await mkdir(workspace);
  await mkdir(inputRoot);
  return { directory, paths: { cli, executable, workspace, inputRoot } };
}

function processesMentioning(directory: string): string[] {
  const listing = execFileSync("/bin/ps", ["-A", "-o", "command="], { encoding: "utf8" });
  return listing.split("\n").filter((line) => line.includes(directory));
}

async function stopped(directory: string): Promise<boolean> {
  const deadline = performance.now() + 5_000;
  while (performance.now() < deadline) {
    if (processesMentioning(directory).length === 0) return true;
    await pause(50);
  }
  return false;
}

function textOf(result: Awaited<ReturnType<Client["callTool"]>>): string {
  const [first] = result.content as { type: string; text?: string }[];
  return first?.text ?? "";
}

async function useConfiguration(client: DocumentedClient, granted: Grants): Promise<string[]> {
  const args = fixtures.readme.substituteArguments(client.args, granted.paths);
  const transport = new StdioClientTransport({
    command: client.command,
    args,
    env: { PATH: `${dirname(process.execPath)}:${process.env["PATH"] ?? ""}` },
    stderr: "pipe",
  });
  const stderr: string[] = [];
  transport.stderr?.on("data", (chunk: Buffer) => stderr.push(String(chunk)));
  const session = new Client({ name: "readme-check", version: "0" });
  await session.connect(transport);
  const names = (await session.listTools()).tools.map((tool) => tool.name);
  expect(textOf(await session.callTool({ name: "probe.echo", arguments: { value: 7 } }))).toBe(
    '{"value":7}',
  );
  expect(textOf(await session.callTool({ name: "toolsets.list", arguments: {} }))).toContain(
    '"audio"',
  );
  await session.close();
  expect(stderr.join("")).toBe("");
  return names;
}

posix.each(["claude", "codex"] as const)(
  "the %s configuration in the README starts a working server",
  async (name) => {
    const granted = await grants();
    const names = await useConfiguration(documented[name], granted);
    expect(names).toContain("probe.echo");
    expect(names).toContain("probe.sound");
    expect(names).toContain("probe.joint");
    expect(names).toContain("toolsets.list");
    expect(await stopped(granted.directory)).toBe(true);
  },
);

test("the two README configurations launch the same server", () => {
  expect(documented.codex.command).toBe(documented.claude.command);
  expect(documented.codex.args).toEqual(documented.claude.args);
  expect(documented.codex.toolTimeoutSec).toBe(120);
});

test.each(["claude", "codex"] as const)("the %s add command matches its configuration", (name) => {
  const client = documented[name];
  const tokens = client.addCommand.replaceAll("\\\n", " ").trim().split(/\s+/u);
  expect(tokens.slice(0, 4)).toEqual([name, "mcp", "add", "groundzero"]);
  expect(tokens[4]).toBe("--");
  expect(tokens.slice(5)).toEqual([client.command, ...client.args]);
});

const documentedFailures: [string[], string][] = [
  [
    ["--workspace /work/animation"],
    '"--workspace /work/animation" is one argument, but --workspace and its value must be ' +
      "separate array elements",
  ],
  [["--executable", "/bin/sh"], "Usage: groundzero-mcp --executable ABSOLUTE_FILE"],
  [
    ["--executable", "/bin/sh", "--workspace", "relative"],
    "Invalid --workspace directory: Directory must be absolute",
  ],
];

posix.each(documentedFailures)(
  "the README message for %j is what the server prints",
  (args, message) => {
    const run = spawnSync(process.execPath, [cli, ...args], { encoding: "utf8", input: "" });
    expect(run.stderr).toContain(message);
    expect(readmeText).toContain(message);
  },
);

test("the README names error codes the adapter still reports", async () => {
  const sources = await Promise.all(
    ["worker.ts", "server.ts"].map((name) =>
      readFile(new URL(`./${name}`, import.meta.url), "utf8"),
    ),
  );
  for (const code of ["WORKER_UNAVAILABLE", "INCOMPATIBLE_WORKER"]) {
    expect(readmeText).toContain(code);
    expect(sources.join("")).toContain(code);
  }
});
