#!/usr/bin/env node
import assert from "node:assert/strict";
import { execFileSync, spawn } from "node:child_process";
import { mkdir, mkdtemp, readFile, realpath, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { delimiter, dirname, join } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";
import { documentedClients, substituteText } from "#fixtures/readme-configs";

const USAGE = `Usage: real-clients.mjs --executable ABSOLUTE_FILE [--claude FILE] [--codex FILE] [--call-tool]

Starts the installed Claude Code and Codex against the README's configurations, in a
throwaway home that never touches your own configuration, and confirms each one connects.
A client is skipped when its binary is not named and is not on PATH.

  Claude Code: \`claude mcp list\` must report the server as connected.
  Codex:       its app server must list the server's tools. With --call-tool it also
               starts a thread and calls automation.list_documents; the thread makes no
               model request but Codex may try to reach its own service.

Neither client is asked to make a model request, and no stored credential is read.`;

const timeoutMs = 60_000;

function parseArguments(argv) {
  const found = { callTool: false };
  for (let index = 0; index < argv.length; index += 1) {
    const flag = argv[index];
    if (flag === "--call-tool") found.callTool = true;
    else if (["--executable", "--claude", "--codex"].includes(flag) && argv[index + 1]) {
      found[flag.slice(2)] = argv[(index += 1)];
    } else throw new Error(USAGE);
  }
  if (!found.executable) throw new Error(USAGE);
  return found;
}

function onPath(name) {
  for (const directory of (process.env.PATH ?? "").split(delimiter)) {
    try {
      const candidate = join(directory, name);
      execFileSync(candidate, ["--version"], { stdio: "ignore", timeout: 10_000 });
      return candidate;
    } catch {
      continue;
    }
  }
  return undefined;
}

async function throwawayHome() {
  const root = await realpath(await mkdtemp(join(tmpdir(), "gz-real-clients-")));
  const paths = {
    cli: fileURLToPath(new URL("../dist/cli.js", import.meta.url)),
    executable: "",
    workspace: join(root, "animation"),
    inputRoot: join(root, "reference"),
  };
  await Promise.all([mkdir(paths.workspace), mkdir(paths.inputRoot), mkdir(join(root, "home"))]);
  const env = {
    PATH: `${dirname(process.execPath)}${delimiter}/usr/bin${delimiter}/bin`,
    HOME: join(root, "home"),
    CLAUDE_CONFIG_DIR: join(root, "claude"),
    CODEX_HOME: join(root, "codex"),
    DISABLE_AUTOUPDATER: "1",
  };
  return { root, paths, env };
}

async function checkClaude(binary, home) {
  const project = join(home.root, "project");
  await mkdir(project);
  await mkdir(home.env.CLAUDE_CONFIG_DIR);
  const { claude } = documentedClients(await readme());
  await writeFile(join(project, ".mcp.json"), substituteText(claude.text, home.paths));
  await writeFile(
    join(home.env.CLAUDE_CONFIG_DIR, "settings.json"),
    JSON.stringify({ enableAllProjectMcpServers: true }),
  );
  const listing = execFileSync(binary, ["mcp", "list"], {
    cwd: project,
    env: home.env,
    encoding: "utf8",
    timeout: timeoutMs,
  });
  assert.match(listing, /groundzero: .*✔ Connected/u, `Claude Code did not connect:\n${listing}`);
  return "claude mcp list: groundzero connected (tool listing needs a model request; not run)";
}

async function readme() {
  return readFile(new URL("../README.md", import.meta.url), "utf8");
}

function appServer(binary, env) {
  const child = spawn(binary, ["app-server", "--disable", "plugins", "--disable", "apps"], {
    env,
    stdio: ["pipe", "pipe", "inherit"],
  });
  const pending = new Map();
  let last = 0;
  createInterface({ input: child.stdout }).on("line", (line) => {
    const message = JSON.parse(line);
    pending.get(message.id)?.(message);
  });
  const request = (method, params) =>
    new Promise((resolve, reject) => {
      const id = (last += 1);
      const timer = setTimeout(
        () => reject(new Error(`Codex did not answer ${method}`)),
        timeoutMs,
      );
      pending.set(id, (message) => {
        clearTimeout(timer);
        if (message.error) reject(new Error(`${method}: ${JSON.stringify(message.error)}`));
        else resolve(message.result);
      });
      child.stdin.write(`${JSON.stringify({ id, method, params })}\n`);
    });
  const notify = (method) => child.stdin.write(`${JSON.stringify({ method })}\n`);
  return { request, notify, stop: () => child.kill("SIGTERM") };
}

async function checkCodex(binary, home, callTool) {
  await mkdir(home.env.CODEX_HOME);
  const { codex } = documentedClients(await readme());
  await writeFile(join(home.env.CODEX_HOME, "config.toml"), substituteText(codex.text, home.paths));
  const configured = JSON.parse(
    execFileSync(binary, ["mcp", "list", "--json"], { env: home.env, encoding: "utf8" }),
  );
  assert.deepEqual(
    configured.map((server) => [server.name, server.enabled]),
    [["groundzero", true]],
  );
  const server = appServer(binary, home.env);
  try {
    await server.request("initialize", { clientInfo: { name: "gz-real-clients", version: "0" } });
    server.notify("initialized");
    const status = await server.request("mcpServerStatus/list", {});
    const tools = Object.keys(
      status.data.find((entry) => entry.name === "groundzero")?.tools ?? {},
    );
    assert(tools.includes("automation.list_documents"), "Codex did not list the core tools");
    assert(tools.includes("audio.import"), "Codex did not list the configured audio toolset");
    if (!callTool) return `codex app-server: ${tools.length} tools listed`;
    const { thread } = await server.request("thread/start", { cwd: home.paths.workspace });
    const call = await server.request("mcpServer/tool/call", {
      server: "groundzero",
      threadId: thread.id,
      tool: "automation.list_documents",
      arguments: {},
    });
    assert.deepEqual(call.structuredContent, { documents: [] });
    return `codex app-server: ${tools.length} tools listed, automation.list_documents answered`;
  } finally {
    server.stop();
  }
}

async function main() {
  const options = parseArguments(process.argv.slice(2));
  const clients = [
    ["Claude Code", options.claude ?? onPath("claude"), checkClaude],
    [
      "Codex",
      options.codex ?? onPath("codex"),
      (binary, home) => checkCodex(binary, home, options.callTool),
    ],
  ];
  let failed = false;
  for (const [name, binary, check] of clients) {
    if (!binary) {
      process.stdout.write(`${name}: skipped, no binary named or on PATH\n`);
      continue;
    }
    const home = await throwawayHome();
    home.paths.executable = options.executable;
    try {
      process.stdout.write(`${name}: ${await check(binary, home)}\n`);
    } catch (error) {
      failed = true;
      process.stderr.write(`${name}: FAILED ${error instanceof Error ? error.message : error}\n`);
    } finally {
      await rm(home.root, { recursive: true, force: true });
    }
  }
  process.exitCode = failed ? 1 : 0;
}

await main();
