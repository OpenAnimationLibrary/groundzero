# Groundzero MCP

This local stdio server exposes the typed automation operations implemented by the
Groundzero binary it launches. Each MCP connection owns a separate native process
and detached documents. It does not attach to an artist's open application.

Use Node.js 22.23.5 and a Groundzero build from the same CI run as this
adapter. CI builds and tests the adapter; the `groundzero-mcp-adapter` artifact
contains `dist`, `package.json`, the resolved lockfile, and installation settings.
The runtime dependency is the official MCP TypeScript server SDK.

Extract the adapter artifact, then install its runtime dependencies from that
directory:

```sh
corepack prepare pnpm@12.21.0 --activate
corepack pnpm audit --prod --audit-level=moderate
corepack pnpm install --prod --frozen-lockfile --ignore-scripts
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

The native profile supports detached scenes, PNG imports, exposures, camera and
peg animation, vector strokes and fills, RGBA8 and Toonz raster pixels, and solid
palette styles. Its FX catalog admits six builtins: color card, linear and radial
gradients, over, blur, and glow. Consult the running catalog for parameters and
limits. There is no live editor connection or undo history in this profile.

`scene.save_copy` writes a new package directory containing `scene.tnz`, its
artwork, and a hash manifest. It checks every saved drawing through the native
reader and compares palette styles, pages, and keys before publishing. Existing
destinations are rejected. Each artwork level must contain at least one drawing;
an empty scene is allowed. Packages are limited to 256 levels, 4096 drawings, and
256 MiB, with a separate conservative limit on source drawing memory.
Native PLI encoding quantizes stroke coordinates and widths, and PNG encoding
can change premultiplied RGB values by one code value. These files are editable
native representations, not bitwise copies of in-memory drawings. Saved raster
levels use explicit custom DPI to preserve authored physical size, including
fractional DPI that PNG metadata alone cannot represent exactly.

`scene.open` accepts unchanged packages produced by the same running worker.
The worker retains the authoritative manifest privately; editing a public
manifest cannot authorize a changed scene. Arbitrary native scenes and packages
from previous worker sessions are rejected. Saved files remain available for
normal application use after the worker exits. Render jobs use separate copies
of the scene and artwork. This process isolation is not an operating system
sandbox for native image readers, and the workspace must not be modified
concurrently by another process.

To inspect an image, start `render.preview`, poll `render.status` until completed,
list `render.artifacts`, and pass an artifact ID to `artifact.read`. The final call
returns MCP image content and source metadata. Inline PNG images are limited to
4 MiB and 2048 pixels per dimension. Job failure and cancellation are reported
explicitly; an artifact is available only after successful writer finalization.

Call `render.cancel` to request job cancellation, then poll `render.status` until
the job reaches a terminal status. Cancelling an MCP request can leave native work
running. Treat an abandoned write as having an unknown outcome and inspect state
before retrying; completed changes are retained.

Native requests are limited to 256 KiB, responses to 8 MiB, and pending calls to
16. MCP responses are limited to 9 MiB including envelope headroom. Metadata above
64 KiB uses a brief text summary and remains complete in `structuredContent`;
image content retains its original bytes. A response timeout after 120 seconds,
malformed output, or worker exit closes the worker and fails pending calls.
A lost response can have an unknown outcome:
inspect saved workspace state before starting another worker and replaying edits.
Diagnostics go to stderr; stdout is reserved for MCP. No network listener is used.

Repository checks run in CI: `pnpm lint`, `pnpm format:check`, `pnpm typecheck`,
`pnpm test`, and `pnpm build`. `pnpm test:native` requires a built native executable
in `GROUNDZERO_EXECUTABLE` and exercises legacy and MCP 2026-07-28 stdio connections,
document creation, revision rejection, and actual preview image content. These
tests establish SDK interoperability on the CI platform; host-specific display
behavior still depends on the MCP application.
