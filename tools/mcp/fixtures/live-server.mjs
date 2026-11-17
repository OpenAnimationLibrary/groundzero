import { spawn } from "node:child_process";
import { appendFileSync } from "node:fs";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";
import { serveStdio } from "@modelcontextprotocol/server/stdio";
import { LiveConnection } from "#mcp/live";
import { createLiveServer } from "#mcp/server";

const connector = fileURLToPath(import.meta.resolve("#fixtures/live-connector"));
const connections = new Set();
const observer = createInterface({ input: process.stdin });
observer.on("line", (line) => {
  const message = JSON.parse(line);
  appendFileSync(
    process.env["LIVE_FIXTURE_WIRE"],
    JSON.stringify({ pid: process.pid, method: message.method }) + "\n",
  );
});

const handle = serveStdio(async () => {
  const child = spawn(process.execPath, [connector, "leased"], {
    stdio: ["pipe", "pipe", "pipe"],
  });
  child.stderr.pipe(process.stderr, { end: false });
  const connection = new LiveConnection(child);
  connections.add(connection);
  const close = () => {
    connection.close();
    connections.delete(connection);
  };
  try {
    return await createLiveServer(
      { request: connection.request.bind(connection), close },
      await connection.ready,
    );
  } catch (error) {
    close();
    throw error;
  }
});

function closeConnections() {
  for (const connection of connections) connection.close();
  connections.clear();
}

process.stdin.once("end", closeConnections);
process.once("exit", closeConnections);
process.once("SIGTERM", () => {
  closeConnections();
  void handle.close();
});
