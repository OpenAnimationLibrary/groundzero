#!/usr/bin/env node
import { serveStdio } from "@modelcontextprotocol/server/stdio";
import { WorkerError } from "#mcp/errors";
import { parseOptions } from "#mcp/options";
import type { DetachedOptions, WorkerOptions } from "#mcp/options";
import { launchLive } from "#mcp/live";
import { createLiveServer, createServer } from "#mcp/server";
import type { NativeApi } from "#mcp/server";
import { guardedStdioTransport } from "#mcp/stdio-guard";
import { launchWorker } from "#mcp/worker";
import type { WorkerConnection } from "#mcp/worker";

/** A worker launched by its first request, so a session that never needs one starts none. */
function deferredWorker(options: WorkerOptions): NativeApi {
  let worker: WorkerConnection | undefined;
  let closed = false;
  return {
    request: (method, params) => {
      if (closed) {
        return Promise.reject(new WorkerError("WORKER_UNAVAILABLE", "MCP connection closed"));
      }
      worker ??= launchWorker(options);
      return worker.request(method, params);
    },
    close: () => {
      closed = true;
      worker?.close();
    },
    loss: () => worker?.loss(),
  };
}

function reportUnavailable(error: WorkerError): void {
  process.stderr.write(
    `Groundzero MCP native worker unavailable: ${error.code}: ${error.message}\n`,
  );
}

async function main(): Promise<void> {
  const options = await parseOptions(process.argv.slice(2));
  const workers = new Set<NativeApi>();
  const closeWorkers = () => {
    for (const worker of workers) worker.close();
    workers.clear();
  };
  const handle = serveStdio(
    async () => {
      const live = "liveSession" in options ? launchLive(options) : undefined;
      const worker = live ?? deferredWorker(options as DetachedOptions);
      workers.add(worker);
      const close = () => {
        worker.close();
        workers.delete(worker);
      };
      try {
        const api = {
          request: worker.request.bind(worker),
          close,
          ...("loss" in worker ? { loss: () => worker.loss?.() } : {}),
        };
        if (live) return await createLiveServer(api, await live.ready, options.strictResults);
        const toolsets = "toolsets" in options ? options.toolsets : ["core"];
        return createServer(api, toolsets, reportUnavailable, options.strictResults);
      } catch (error) {
        close();
        throw error;
      }
    },
    { transport: guardedStdioTransport() },
  );
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
