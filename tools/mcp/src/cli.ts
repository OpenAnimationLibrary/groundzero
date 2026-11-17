#!/usr/bin/env node
import { serveStdio } from "@modelcontextprotocol/server/stdio";
import { parseOptions } from "#mcp/options";
import { launchLive } from "#mcp/live";
import { createLiveServer, createServer } from "#mcp/server";
import type { NativeApi } from "#mcp/server";
import { launchWorker } from "#mcp/worker";

async function main(): Promise<void> {
  const options = await parseOptions(process.argv.slice(2));
  const workers = new Set<NativeApi>();
  const closeWorkers = () => {
    for (const worker of workers) worker.close();
    workers.clear();
  };
  const handle = serveStdio(async () => {
    const worker = "liveSession" in options ? launchLive(options) : launchWorker(options);
    workers.add(worker);
    const close = () => {
      worker.close();
      workers.delete(worker);
    };
    try {
      const api = { request: worker.request.bind(worker), close };
      if ("ready" in worker) return await createLiveServer(api, await worker.ready);
      return await createServer(api);
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
