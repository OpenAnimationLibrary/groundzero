import { spawn } from "node:child_process";
import { createServer } from "node:net";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";

function descendant() {
  process.on("SIGTERM", () => {});
  const server = createServer((socket) => socket.end("alive"));
  server.listen(0, "127.0.0.1", () => {
    const address = server.address();
    process.send({ port: address.port, pid: process.pid });
  });
}

function worker() {
  const child = spawn(process.execPath, [fileURLToPath(import.meta.url), "descendant"], {
    stdio: ["ignore", "ignore", "inherit", "ipc"],
  });
  child.once("message", (identity) => {
    createInterface({ input: process.stdin }).on("line", (line) => {
      const request = JSON.parse(line);
      if (request.method === "stall") return;
      const response = JSON.stringify({ id: request.id, result: identity }) + "\n";
      process.stdout.write(response, () => {
        if (request.method === "final") process.exit(0);
      });
    });
  });
}

if (process.argv[2] === "descendant") descendant();
else worker();
