import assert from "node:assert/strict";

/** The example paths the README's client configurations use, by role. */
export const placeholders = {
  cli: "/opt/groundzero-mcp/dist/cli.js",
  executable: "/opt/groundzero/bin/Groundzero",
  workspace: "/work/animation",
  inputRoot: "/work/reference",
};

const SERVER_NAME = "groundzero";
const PLACEHOLDER_PREFIXES = ["/opt/", "/work/"];

function section(readme, marker, language) {
  const pattern = new RegExp(`<!-- ${marker} -->\\n\`\`\`${language}\\n([\\s\\S]*?)\\n\`\`\``, "u");
  const match = pattern.exec(readme);
  assert(match?.[1], `README has no ${language} block after "<!-- ${marker} -->"`);
  return match[1];
}

function allowedKeys(entry, allowed, label) {
  for (const key of Object.keys(entry)) {
    assert(allowed.includes(key), `${label} uses "${key}", which the README check does not model`);
  }
}

function claudeConfig(text) {
  const entry = JSON.parse(text)?.mcpServers?.[SERVER_NAME];
  assert(entry, `The Claude Code snippet must define mcpServers.${SERVER_NAME}`);
  allowedKeys(entry, ["command", "args"], "The Claude Code snippet");
  return { command: entry.command, args: entry.args };
}

function tomlAssignments(text) {
  const lines = text.split("\n");
  assert.equal(lines.shift(), `[mcp_servers.${SERVER_NAME}]`, "Unexpected Codex table header");
  const assignments = new Map();
  let key = "";
  let value = "";
  for (const line of lines) {
    const start = /^([a-z_]+) = (.*)$/u.exec(line);
    if (start && value === "") [, key, value] = start;
    else value += line;
    const open = (value.match(/\[/gu) ?? []).length;
    const close = (value.match(/\]/gu) ?? []).length;
    if (value !== "" && open === close) {
      assignments.set(key, JSON.parse(value.replace(/,\s*\]/gu, "]")));
      value = "";
    }
  }
  assert.equal(value, "", "Unterminated array in the Codex snippet");
  return assignments;
}

function codexConfig(text) {
  const assignments = tomlAssignments(text);
  allowedKeys(Object.fromEntries(assignments), ["command", "args", "tool_timeout_sec"], "Codex");
  return {
    command: assignments.get("command"),
    args: assignments.get("args"),
    toolTimeoutSec: assignments.get("tool_timeout_sec"),
  };
}

/** Extract the client configurations and shell commands the README documents.
 *
 * Args:
 *   readme: The text of `tools/mcp/README.md`.
 * Returns:
 *   For each client its launch command and arguments with the README's example paths, the
 *   raw snippet text, and the `claude mcp add` or `codex mcp add` command line.
 */
export function documentedClients(readme) {
  const claudeText = section(readme, "client-config: claude", "json");
  const codexText = section(readme, "client-config: codex", "toml");
  return {
    claude: {
      ...claudeConfig(claudeText),
      text: claudeText,
      addCommand: section(readme, "client-command: claude", "sh"),
    },
    codex: {
      ...codexConfig(codexText),
      text: codexText,
      addCommand: section(readme, "client-command: codex", "sh"),
    },
  };
}

/** Replace the README's example paths in text, for example a whole configuration file. */
export function substituteText(text, paths) {
  let result = text;
  for (const [role, example] of Object.entries(placeholders)) {
    assert(paths[role], `No path given for ${role}`);
    result = result.replaceAll(
      JSON.stringify(example).slice(1, -1),
      JSON.stringify(paths[role]).slice(1, -1),
    );
  }
  return result;
}

/** Replace the README's example paths in launch arguments and refuse any left over. */
export function substituteArguments(args, paths) {
  const byExample = new Map(
    Object.entries(placeholders).map(([role, path]) => [path, paths[role]]),
  );
  return args.map((arg) => {
    assert(
      byExample.has(arg) || !PLACEHOLDER_PREFIXES.some((prefix) => arg.startsWith(prefix)),
      `Unknown example path in the README: ${arg}`,
    );
    return byExample.get(arg) ?? arg;
  });
}
