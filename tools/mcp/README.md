# Groundzero MCP

This local stdio server exposes the typed automation operations implemented by the
Groundzero binary it launches. Each MCP connection owns a separate native process
and detached documents. It does not attach to an artist's open application.

Use Node.js 22.12 or newer and a Groundzero build from the same CI run as this
adapter. CI builds and tests the adapter; the `groundzero-mcp-adapter` artifact
contains `dist`, `package.json`, the resolved lockfile, and installation settings.
The runtime dependency is the official MCP TypeScript server SDK.

Extract the adapter artifact, then install its runtime dependencies from that
directory:

```sh
corepack prepare pnpm@12.21.0 --activate
pnpm audit --prod --audit-level=moderate
pnpm install --prod --frozen-lockfile --ignore-scripts
```

Configure a local MCP host with the equivalent of this command and argument list,
replacing each example path with its absolute installed path:

```json
{
  "mcpServers": {
    "groundzero": {
      "command": "node",
      "args": [
        "/opt/groundzero-mcp/dist/cli.js",
        "--executable",
        "/opt/groundzero/bin/Groundzero",
        "--workspace",
        "/work/animation"
      ]
    }
  }
}
```

The workspace must already exist and use a canonical directory path without
symlinks. Add `--input-root /work/reference` for each additional existing directory
the worker may read. Writes stay inside the workspace. Choose a new dedicated
workspace for a session; these roots are fixed at launch and cannot be broadened
by a tool call. The executable must include native automation support and its
normal libraries, codecs, and application resources.

Ask the MCP host to list tools to see the capabilities of that binary. Tool input
schemas describe their exact arguments and bounds. Frames start at zero, while
native drawing identifiers retain their drawing number and optional suffix. Writes
require an `operation_id`; changes to an existing document also require its latest
`expected_revision`. Keep the original operation ID when checking a retry within
the same worker. Receipts and document handles expire when that worker exits.

To inspect an image, start `render.preview`, poll `render.status` until completed,
list `render.artifacts`, and pass an artifact ID to `artifact.read`. The final call
returns MCP image content and source metadata. Inline PNG images are limited to
4 MiB and 2048 pixels per dimension. Job failure and cancellation are reported
explicitly; an artifact is available only after successful writer finalization.

Native requests are limited to 256 KiB, responses to 8 MiB, and pending calls to
16. A response timeout after 120 seconds, malformed output, or worker exit closes
the worker and fails pending calls. A lost response can have an unknown outcome:
inspect saved workspace state before starting another worker and replaying edits.
Diagnostics go to stderr; stdout is reserved for MCP. No network listener is used.

Repository checks run in CI: `pnpm lint`, `pnpm format:check`, `pnpm typecheck`,
`pnpm test`, and `pnpm build`. `pnpm test:native` requires a built native executable
in `GROUNDZERO_EXECUTABLE` and exercises legacy and MCP 2026-07-28 stdio connections,
document creation, revision rejection, and actual preview image content. These
tests establish SDK interoperability on the CI platform; host-specific display
behavior still depends on the MCP application.
