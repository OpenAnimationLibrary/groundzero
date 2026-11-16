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

Movie rendering requires an explicit `--media-tools /opt/groundzero/media` startup
argument naming a canonical directory containing regular executable `ffmpeg` and
`ffprobe` files (`.exe` on Windows); symlinks are rejected. This optional grant is
fixed for the session and does not add an input root. Without it, media helpers
are disabled; the worker does not discover them through `PATH`.

Ask the MCP host to list tools to see the capabilities of that binary. Tool input
schemas describe their exact arguments and bounds. Frames start at zero. Native
drawing numbers range from 1 to 65535 for vector levels and from 1 to 999999 for
RGBA8 and Toonz raster levels; lowercase ASCII suffixes are preserved. Writes
require an `operation_id`; changes to an existing document also require its latest
`expected_revision`. Keep the original operation ID when checking a retry within
the same worker. Receipts and document handles expire when that worker exits.

The native profile supports detached scenes, PNG imports, exposures, camera and
peg animation, vector strokes and fills, RGBA8 and Toonz raster pixels, and solid
palette styles. Its FX catalog admits six builtins: color card, linear and radial
gradients, over, blur, and glow. Consult the running catalog for parameters and
limits. There is no live editor connection or undo history in this profile.

PNG inputs support up to 8 bits per channel, 8192 pixels per axis, and 16,777,216
pixels per image. Vector drawings are limited to 4096 strokes and 32768 control
points, with coordinates within ±32768 and radii up to 255 native units. Palettes
support up to 4096 unlinked solid styles, without reference images or studio
palette links. Style names cannot contain NUL or start with `|`. Palette keyframes
use frames 0–65535 for vector levels and 0–999999 for Toonz raster levels. Consult
the running catalog for the remaining operation-specific bounds.

`scene.save_copy` writes a new package directory containing `scene.tnz`, its
artwork, and a hash manifest. It checks every saved drawing through the native
reader and compares palette styles, pages, and keys before publishing. Existing
destinations are rejected. Each artwork level must contain at least one drawing;
an empty scene is allowed. Packages are limited to 256 MiB; before artwork grows,
the worker checks limits of 256 levels, 4096 drawings, and a separate 256 MiB
aggregate estimate covering raster and vector frames plus conservative overhead
for frames, palette styles, and keyframes. This estimate is an admission limit,
not a bound on process memory.
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

The worker allows at most 16 pending calls, 256 KiB per request, and 8 MiB per
response. MCP responses are limited to 9 MiB including envelope headroom. Metadata above
64 KiB uses a brief text summary and remains complete in `structuredContent`;
image content retains its original bytes. A response timeout after 120 seconds,
malformed output, or worker exit closes the worker and fails pending calls.
A lost response can have an unknown outcome:
inspect saved workspace state before starting another worker and replaying edits.
Diagnostics go to stderr; stdout is reserved for MCP. No network listener is used.

On POSIX systems, media-enabled workers run under a private supervisor that
terminates their process group when the connection closes, times out, or loses
its parent. Windows workers contain media helpers in a native job object.
Forcibly killing the POSIX supervisor itself can bypass its cleanup.

Repository checks run in CI: `pnpm lint`, `pnpm format:check`, `pnpm typecheck`,
`pnpm test`, and `pnpm build`. `pnpm test:native` requires a built native executable
in `GROUNDZERO_EXECUTABLE` and exercises legacy and MCP 2026-07-28 stdio connections,
document creation, revision rejection, and decoded color-card preview pixels. These
tests establish SDK interoperability on the CI platform; host-specific display
behavior still depends on the MCP application.
