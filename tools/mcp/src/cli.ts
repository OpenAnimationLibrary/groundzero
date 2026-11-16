#!/usr/bin/env node
import { serveStdio } from "@modelcontextprotocol/server/stdio";
import { parseOptions } from "#mcp/options";
import { createServer } from "#mcp/server";
import { launchWorker } from "#mcp/worker";

async function main(): Promise<void> {
  const options = await parseOptions(process.argv.slice(2));
  const workers = new Set<ReturnType<typeof launchWorker>>();
  const closeWorkers = () => {
    for (const worker of workers) worker.close();
    workers.clear();
  };
  const handle = serveStdio(async () => {
    const worker = launchWorker(options);
    workers.add(worker);
    const close = () => {
      worker.close();
      workers.delete(worker);
    };
    try {
      return await createServer({ request: worker.request.bind(worker), close });
    } catch (error) {
      close();
      throw error;
    }
  });
  const stop = () => {
    closeWorkers();
    void handle.close().catch((error: unknown) => {
      process.stderr.write(`Cannot close MCP transport: ${String(error)}\n`);
      process.exitCode = 1;
    });
  };
  process.once("SIGTERM", stop);
  process.once("SIGINT", stop);
  process.once("exit", closeWorkers);
  process.stdin.once("end", closeWorkers);
}

void main().catch((error: unknown) => {
  process.stderr.write(`Groundzero MCP startup failed: ${String(error)}\n`);
  process.exitCode = 1;
});
