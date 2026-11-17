# Groundzero MCP

This local stdio server exposes the typed automation operations implemented by
Groundzero. Detached sessions own separate native processes and documents. Live
sessions attach to an editor explicitly launched with live automation enabled.

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

## Detached sessions

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

Workers retain up to 2,048 ordinary write receipts within a 64 MiB serialized
budget. The first cancellation for each of at most 32 retained render jobs has
separate reserved receipt capacity, capped at 16 KiB per result. That cancellation
and its exact retry remain available when ordinary receipt capacity is exhausted.
Further cancellation requests with new operation IDs use ordinary capacity;
reuse the first ID to retrieve its receipt. Status queries remain available,
and reusing an operation ID for different arguments always conflicts.

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

Palette pages can be inspected, created, renamed, reordered, and removed when
empty. A palette admits up to 128 pages; page zero and transparent style zero
stay first. Style moves preserve numeric IDs and artwork references. Style
renames and copies preserve all animation keys; a copy receives a new ID.
Page names use 1–128 characters without NUL; vector page zero cannot begin with
`refimage`, which the native PLI reader reserves. Physical style deletion and
unpaged styles are not supported.

`scene.save_copy` writes a new package directory containing `scene.tnz`, its
artwork, and a hash manifest. It checks every saved drawing through the native
reader and compares palette styles, pages, and keys before publishing. Existing
destinations are rejected. Each artwork level must contain at least one drawing;
an empty scene is allowed. Packages are limited to 256 MiB; before artwork grows,
the worker checks limits of 256 levels, 4096 drawings, and a separate 256 MiB
aggregate estimate covering raster and vector frames plus conservative overhead
for frames, palette pages, styles, and keyframes. This estimate is an admission limit,
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

`render.submit` accepts `format: "png"` (the default) or `format: "mp4"` when the
configured media tools and native movie writer are available. MP4 requires even
dimensions up to 2048 pixels, `step: 1`, and at most 120 frames within the render
storage budget. Its output has no audio or alpha; transparent areas use a black
matte. `render.artifacts` returns a movie's `workspace_relative_path`, SHA-256,
size, dimensions, timing, and observed codec metadata after verification. Movie
bytes stay in that workspace file; `artifact.read` remains PNG-only and rejects
movie IDs with `unsupported_artifact_kind`.

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

## Live editor sessions

Launch the normal application with an explicit permission mode and a new session
file in an existing private directory:

```sh
/opt/groundzero/bin/Groundzero --automation-live inspect \
  --automation-live-session /work/private/editor-session.json
```

Use `edit` in place of `inspect` to permit the two qualified native edits. The
directory and grant must belong to the current user: POSIX requires directory
mode `0700` and file mode `0600`; Windows requires protected ACLs allowing only
the user and SYSTEM. The application creates the grant exclusively. Treat this
file as a secret; do not paste its contents into a prompt or log.

Attach the MCP host using the trusted application executable and that file:

```sh
node /opt/groundzero-mcp/dist/cli.js \
  --executable /opt/groundzero/bin/Groundzero \
  --live-session /work/private/editor-session.json
```

The adapter starts a non-GUI connector to the selected editor. Native code checks
the session-file security and operating-system peer identity before authenticating.
Live attachment excludes `--workspace`, `--input-root`, and `--media-tools`.

Both permission modes advertise exactly `live.status`, `live.inspect`,
`live.stage.set_key_value`, `live.fx.set_scalar_value`, `live.receipt`, and
`live.capabilities`. Inspect mode rejects writes with `permission_denied`.
Edit mode can change an existing camera/pegbar x, y, or rotation key, or the
qualified native Blur scalar. A changed value creates one native undo entry;
same-value edits preserve history. Use the returned schemas and editable-target
fingerprints rather than detached revision fields. Fingerprints cover qualified
target dependencies; pixels, palette content, and asset bytes are excluded.

The editor accepts two authenticated clients sharing handles and operation
receipts. Each connection permits four pending requests, with 64 KiB requests
and 1 MiB native responses. Busy GUI work is rejected with `gui_busy`;
`live.status` and `live.receipt` remain available with their documented cached
semantics. There is no live rendering, filesystem operation, or generic undo tool.

Disconnecting an adapter leaves the editor and admitted edits running. After a
lost reply, reconnect to the same instance and query `live.receipt` with the
original operation UUID. Exact retries return the retained outcome; changed
arguments conflict. Receipts expire when the editor stops live automation or
exits, and a new instance requires fresh inspection. Use the editor's visible
`Stop Live Automation` action to revoke the session and close its connections.

## Repository checks

Repository checks run in CI: `pnpm lint`, `pnpm format:check`, `pnpm typecheck`,
`pnpm test`, and `pnpm build`. `pnpm test:native` requires a built native executable
in `GROUNDZERO_EXECUTABLE` and a canonical media directory in
`GROUNDZERO_MEDIA_TOOLS`. It exercises legacy and MCP 2026-07-28 stdio connections,
document creation, revision rejection, decoded color-card preview pixels, and MP4
metadata and file hashes. The PNG cases launch without a media grant. These tests
establish SDK interoperability on the CI platform; host-specific display behavior
still depends on the MCP application.
