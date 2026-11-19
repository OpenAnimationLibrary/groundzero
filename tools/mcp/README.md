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

## Connect a client

The adapter is a local stdio server that the MCP client starts itself, so the same
command works in any client that can launch a stdio server. These two examples are
the same server configured for Claude Code and for Codex; replace each example path
with its absolute installed path.

<!-- client-config: claude -->
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
        "/work/animation",
        "--input-root",
        "/work/reference",
        "--toolsets",
        "audio,rigging"
      ]
    }
  }
}
```

<!-- client-config: codex -->
```toml
[mcp_servers.groundzero]
command = "node"
args = [
  "/opt/groundzero-mcp/dist/cli.js",
  "--executable", "/opt/groundzero/bin/Groundzero",
  "--workspace", "/work/animation",
  "--input-root", "/work/reference",
  "--toolsets", "audio,rigging",
]
tool_timeout_sec = 120
```

- `command` and the first argument start the adapter with Node.js 22.23.5. Give the
  absolute path of `node` if the client does not start with your shell's `PATH`.
- `--executable` is the Groundzero application binary from the same build as the
  adapter. On macOS it is the file inside the bundle's `Contents/MacOS` directory.
- `--workspace` is the existing directory the session writes to.
- `--input-root` adds one more existing directory the session may read; repeat it for
  each directory.
- `--toolsets` names the optional toolsets to advertise. The default is `core`.
- `tool_timeout_sec` is Codex's limit on one tool call, 60 seconds unless set. The
  adapter itself gives a native call up to 120 seconds.

Each flag and its value are separate list items. Neither client needs an
environment variable for the server.

**Claude Code.** Save the first snippet as `.mcp.json` in the project directory
(Claude Code asks once whether to trust a project's servers), or save it anywhere and
start `claude --mcp-config ./groundzero.mcp.json`. The equivalent command adds the
server to your own Claude Code configuration instead:

<!-- client-command: claude -->
```sh
claude mcp add groundzero -- node /opt/groundzero-mcp/dist/cli.js --executable /opt/groundzero/bin/Groundzero --workspace /work/animation --input-root /work/reference --toolsets audio,rigging
```

`claude mcp list` starts each configured server and prints `groundzero: … ✔ Connected`
when the handshake worked. Inside a session, `/mcp` lists the server and its tools.

**Codex.** Put the second snippet in `~/.codex/config.toml`, or in a trusted
project's `.codex/config.toml`. The equivalent command writes the `command` and `args`
entries to that file; add `tool_timeout_sec` yourself afterwards:

<!-- client-command: codex -->
```sh
codex mcp add groundzero -- node /opt/groundzero-mcp/dist/cli.js --executable /opt/groundzero/bin/Groundzero --workspace /work/animation --input-root /work/reference --toolsets audio,rigging
```

`codex mcp list` and `codex mcp get groundzero` show the configuration Codex read;
they do not start the server. Inside a session, `/mcp verbose` starts it and lists its
tools.

To check the connection end to end, ask the client to call `automation.list_documents`;
a new session answers `{"documents":[]}`. Clients may rewrite the dot in a tool name,
so read the exact names from `/mcp`.

A start that fails prints one of these lines on the server's standard error, which
the client shows in its server log or error message. Run the same command in a
terminal to see it directly.

| Message | Meaning |
| --- | --- |
| `"--workspace /work/animation" is one argument, but --workspace and its value must be separate array elements` | A flag and its value share one list item. Give them as two items: `"--workspace", "/work/animation"`. |
| `Usage: groundzero-mcp --executable ABSOLUTE_FILE` | `--executable` or `--workspace` is missing. |
| `Invalid --workspace directory: Directory must be absolute` | A directory is relative, missing, or a symlink such as `/tmp` on macOS; the message names the canonical path to use. |
| `WORKER_UNAVAILABLE: Native worker exited` | `--executable` starts but is not the Groundzero binary. The adapter reports this when the worker is first needed, such as when the client lists tools, not at startup. |
| `INCOMPATIBLE_WORKER` | The binary and the adapter are not from the same build. |

Any other MCP client that can start a stdio server takes the same command and
arguments; only the file format differs.

## Detached sessions

The options above set the filesystem grants of a detached session. The workspace
must already exist and use a canonical directory path without
symlinks. Add `--input-root /work/reference` for each additional existing directory
the worker may read. Writes stay inside the workspace. Choose a new dedicated
workspace for a session; these roots are fixed at launch and cannot be broadened
by a tool call. A single-valued option such as `--workspace` or `--media-tools`
given twice is refused with the flag named. Reads admit a hard link inside a root
like the file it names, since the two are indistinguishable; outputs are
published without replacing anything, so a hard link never lets a tool change a
file outside the root. A destination file system that can neither create a name
exclusively nor hard-link a finished file (FAT and exFAT) is refused with
`publication_unsupported` and nothing is written. The executable must include native automation support and its
normal libraries, codecs, and application resources.

Movie rendering requires an explicit `--media-tools /opt/groundzero/media` startup
argument naming a canonical directory containing regular executable `ffmpeg` and
`ffprobe` files (`.exe` on Windows); symlinks are rejected. This optional grant is
fixed for the session and does not add an input root. Without it, media helpers
are disabled; the worker does not discover them through `PATH`.

Shared studio palettes and the Library are reachable only through explicit
`--shared-root ROLE=DIR` arguments, at most one per role: `studio_palettes`,
`project_palettes` or `library`. Each directory must be canonical, must not be a
symlink, and must not overlap the workspace or another shared root; input roots
may overlap. Shared roots are read-only. `--shared-write ROLE` records a write
grant for a configured role; the current tools only read. The worker never uses
the editor's own STUDIOPALETTE or LIBRARY folders. Without a grant, the tools
stay listed and return `root_unavailable`.

What a session may do is fixed at launch, with no prompts and no tool that changes
it. `--grants LIST` names grant classes, comma-separated:

- `inspect`: reads, `scene.open_read_only`, discarding a read-only document, and
  job reads (`job.status`, `job.list`, `job.result`). Always required.
- `draft`: create, change and discard editable documents, including the private
  copies imports make in the workspace.
- `file`: write a new file or directory at a path the caller names
  (`scene.save_copy`, `raster.export_png`, `audio.mixdown`, `fx.preset.save`).
- `job`: start or cancel jobs: renders, neural previews and analysis jobs.

`--domains LIST` names the toolsets the session may use, or `all`; `core` is always
granted. The defaults, `--grants inspect,draft,file,job --domains all`, are the
full authority, so the configuration above needs neither option. For example,
`--grants inspect --domains core` gives a review session that can open scenes
read-only, read them and read jobs, and nothing more. Each tool needs the grants
its effect declaration lists (`tool.describe`); a call outside them returns
`grant_denied`, whose details give the `tool`, the classes it is `missing`, its
`domain`, and the session's `granted` classes and `granted_domains`. Nothing is
recorded for a denied call: no receipt, no revision change, no file. An unknown
class, a list without `inspect`, a repeated name, `all` with other names, or a
repeated option stops the adapter at startup; an unknown toolset name stops the
worker, so the first tool call reports `WORKER_UNAVAILABLE`.
`automation.capabilities` returns the session's `authority`: its grants and
domains, the workspace, input roots, shared roots and media tools, and their
SHA-256 `digest`. Write receipts are bound to that digest.

Native tools are grouped into toolsets, so a session only carries the tool
definitions it needs. The `core` toolset is always enabled. Add others at launch
with `--toolsets audio,rigging`, or use `--toolsets all` for every granted toolset.
Unknown names, and toolsets outside `--domains`, stop the adapter at startup.
During a session:

- `toolsets.list` reports each toolset, what it covers, its tool count, whether
  the launch grants it and whether it is enabled.
- `toolsets.enable` and `toolsets.disable` take `{"names": [...]}`. They change
  the advertised tool list and send `notifications/tools/list_changed`. Both are
  idempotent, and `core` cannot be disabled. Enabling a toolset outside
  `--domains` returns `GRANT_REQUIRED`, naming them in its details' `toolsets`, and
  changes nothing.
- Calling a tool whose toolset is disabled returns `TOOLSET_DISABLED`, with the
  toolset in `details.toolset`. Nothing reaches the worker.
- A session exposes at most 256 native tools. A request that would exceed this
  returns `TOOLSET_LIMIT` and leaves the enabled toolsets unchanged.

Every successful tool result carries its JSON as a text block as well as structured
content, so a client that shows the model only text still has the data. A result and
its structured copy must together fit 9 MiB, or the call fails with `RESULT_TOO_LARGE`.
`automation.capabilities` describes the tools a page at a time (`limit` up to 64,
default 16, and `offset`); repeat with `next_offset` until it is null. A descriptor
carries its `effects`, `errors` and `outputSchema` only when the call passes
`declarations: true`, which keeps the default page small. The adapter reads every page
itself.

The adapter waits up to 30 seconds from launching the native worker for its private
handshake, since a cold start on a loaded machine spends most of that loading the
executable. A worker that does not answer in time fails the call with a message that
says the handshake timed out and after how many milliseconds.

The adapter requires native automation protocol version 3. An older or newer
worker is refused with `INCOMPATIBLE_WORKER`.

Ask the MCP host to list tools to see the capabilities of that binary. Tool input
schemas describe their exact arguments and bounds. Frames start at zero. Native
drawing numbers range from 1 to 65535 for vector levels and from 1 to 999999 for
RGBA8 and Toonz raster levels; lowercase ASCII suffixes are preserved. Writes
require an `operation_id`; changes to an existing document also require its latest
`expected_revision`. Operation IDs must be unique within a workspace (use UUIDs):
every write's admission and outcome are recorded in the workspace journal, so an
exact retry returns the recorded result even in a later worker, and never runs
twice. Document handles expire when their worker exits; a document ID names the
worker that issued it (`document_<8 hex>_<uuid>`), and an earlier worker's ID is
refused with `document_not_found`, `details.reason` `other_worker` (with
`worker_instance_id` and `worker_alive`) or `not_issued`.

### The workspace journal and durable receipts

The worker keeps one journal per workspace in `<workspace>/.groundzero/journal`,
shared by every worker on that workspace: append-only JSON-lines segments of at
most 16 MiB, each line prefixed with the SHA-256 of its JSON, results over 32 KiB in
`blobs/`, and damaged or torn data moved to `quarantine/`. Appends hold an
operating-system file lock for one write; every record but a release is flushed to
the disk before the step that depends on it, and a write's reply is sent only after
its outcome is flushed. No tool reads or writes under `.groundzero`: such paths are
refused with `path_denied`, `details.reason` `reserved`.

- An exact retry of a write the journal holds returns the recorded result with
  `receipt {source: "journal", recorded_by_worker, recorded_at_ms,
  handles_expired}` (`handles_expired` is true when another worker recorded it, so
  its document, job and object IDs are no longer usable), or the recorded error
  with the same `receipt` in its details. A retry under different arguments or launch grants is
  `operation_conflict` (`details.reason` `grant_changed` for the grants).
- An operation another live worker admitted and has not finished is
  `operation_in_progress`. One whose worker died before recording its outcome is
  never run again: a retry is `operation_interrupted` with a `verdict`:
  `lost_with_worker` (its change lived in that worker's memory), `not_published` or
  `published`/`published_changed` (a file publication, checked against the digest
  its intent recorded before the final rename), or `job_unknown`.
- `operation.status {operation_id}` reports what the journal holds for one ID and
  what a retry would do; `operation.list` lists operations newest first (default:
  unresolved, meaning in progress or interrupted), paged with `before_seq`;
  `journal.status` reports the format, size, retention, writers and whether they
  are alive, the last start-up recovery and any damage. All three only read.
- At start a worker reads the journal under its lock: a torn last line is cut off
  into `quarantine/`, a segment with a damaged line is copied there and rewritten
  without it (`state: "damaged"`; a lookup that finds nothing is then
  `unknown_damaged`), operations and jobs of dead workers are reconciled (a
  package found published is registered), and old settled operations beyond the
  newest 16,384 are compacted to stubs, whose retry
  is `receipt_expired`.
- Between requests, once a second, a worker records the end of every job it has
  seen end, its scene hosts' included, and takes one bounded retention step that
  rewrites at most one sealed segment: the stub rule above, job records beyond the
  newest 4,096, package registrations beyond the newest 4,096 or 512 MiB of
  manifests (an expired mark stays), and past 1 GiB the oldest settled
  operations.
- When the journal cannot be written (`unsupported_volume` for a network file
  system, `newer_format`, `lock_unavailable`, `io_error`, `flush_failed`) every
  write is refused with `journal_unavailable` and reads still answer;
  `automation.capabilities` reports `journal {available, state, format_version,
  path}`. A write that cannot get the lock within 5 s is `journal_busy` and admits
  nothing.

The tool list stays small: each tool carries its name, description, input schema
and annotations. The annotations follow from the tool's effects: `readOnlyHint`
for reads, `destructiveHint` for writes that can overwrite, `idempotentHint`
always, and `openWorldHint: false` because no tool reaches a network or an outside
service. `tool.describe` with `{"names": [...]}` (1 to 16 tool names, the toolset
tools included) returns each tool's full declaration; `automation.capabilities` with
`declarations: true` returns them a page at a time. A declaration gives:

- `effects`: the class (`read`, `document_write`, `document_lifecycle`,
  `job_start`, `job_control` or `session`), the effect on documents, the files read
  or written, the jobs and helper processes started, whether existing state can be
  changed or removed (`overwrites`), and the grants needed. Writes keep no undo
  history, never claim a rollback, replay their receipt on retry, keep the
  revision on an exact no-op, and report `partially_applied` when they fail after
  a change began.
- `errors`: every error code the tool can return, the adapter's own included. An
  unknown name returns `unknown_tool`, which lists the unknown names in its details.
- `outputSchema`: the structured content (the result without its image) or the
  error envelope `{error: {code, message, details}}` that every error result
  carries, with messages of at most 2,048 characters.

The adapter checks every successful result against its tool's output schema. A
result that does not match is still returned, because its work may already have
happened: the reply carries `_meta["groundzero/output_mismatch"]: true`, and the
adapter writes one line to stderr naming the tool and the failing paths, never the
values. Launched with `--strict-results`, as the acceptance tests do, the adapter
instead returns `INVALID_RESULT` with the tool, `outcome` (`applied` for a write,
`none` for a read), the `operation_id` and at most 16 failing paths.

Workers retain up to 2,048 ordinary write receipts within a 64 MiB serialized
budget. The first cancellation for each retained job, at most 64, has separate
reserved receipt capacity, capped at 16 KiB per result. That cancellation
and its exact retry remain available when ordinary receipt capacity is exhausted.
Further cancellation requests with new operation IDs use ordinary capacity;
reuse the first ID to retrieve its receipt. Status queries remain available,
and reusing an operation ID for different arguments always conflicts.

The native profile supports detached scenes, PNG imports, exposures, camera and
peg animation, vector strokes and fills, RGBA8 and Toonz raster pixels, and solid
palette styles. Its FX catalog, profile `builtin-2d-v2`, admits 31 builtins: color
card, linear and radial gradients, over, blur, glow, brightness/contrast, HSV
adjustment, multiply, screen, inside/outside matte, RGB inversion and cross-dissolve,
plus the finishing filters bloom, light wrap, backlit, body highlight, light spot,
target spot, directional and local blur, posterize, solarize, sharpen, emboss,
erode/dilate, matte choke, channel mixer, gamma and exposure. Consult the running
catalog for exact native type, port and parameter names, ranges and versions. Many
admitted ranges are narrower than the native ones: blur and glow radii are limited
to 0–200, HSV percentage scales to 0–400 and multiply offsets to −255–255; multiply
truncates fractional offsets toward zero in its native integer kernel. Parameters
with native menu choices accept integers only, and a color without a native matte
requires alpha 255. Every admitted effect uses its catalog `native_fx_version` and
scalar linear animation keys. These bounds also apply to presets, macro members,
scene packages and render snapshots, so a scene saved with a value outside them does
not reopen. Each catalog entry also lists `extent_parameters`, `constraints`,
`required_inputs`, `seed_parameters` and `replay_class`. There is no live editor
connection or undo history in this profile.

A linear gradient's start and end points must be equal or at least 0.001 apart at
every frame, keys included. A violation fails with `PARAMETER_CONSTRAINT`, naming
the constraint, effect, parameters and frame. Before a render starts,
`render.preview` and `render.submit` scale each extent parameter by the largest
render scale its effect can receive at each requested frame: the output size over
the camera size, the camera placement and the placement of every column upstream.
A job whose padding would pass the parameter's `max_px` (1024 pixels for most) fails
with `fx_preflight_failed` and details `{reason, fx_id, parameter, frame, extent_px,
max_px}`; erode/dilate also requires an unmirrored placement (`positive_determinant`).
A render job that is still running ten minutes after submission is stopped as
cancellation would stop it and ends with status `deadline_exceeded`: its writers are
closed, it publishes no artifacts, and the worker keeps serving. A job stops between
frames and tiles, so the status can read `cancellation_requested` for a while first.
Render jobs report `fx_provenance`: the type, native version, replay class and seeds
of every effect the output renders. `fx.graph` reports `REQUIRED_INPUT_MISSING` while
backlit has no Light, light wrap no Background or local blur no Reference input.

`fx.recipe.list`, in the `fx-finishing` toolset, returns static recipes in the categories
of the FX recipes (light wrap, backlit rim light, exposure grade, cross dissolve, iris
wipe) and `shot` (dolly shot assembly), `rig` (pendulum rig) and `delivery` (frame
sequence delivery). A recipe lists its brief, its required effect types and whether they
are available, its `required_capabilities` (the tools its steps call, their toolsets and
the grants they need: compare them with `automation.capabilities`, since the list does not
know your session), its inputs with defaults, preconditions, ownership (what it creates and
edits), steps as ordinary tool calls, controls, seeds and frame units, deliverables each
labelled `editable`, `approximated` or `baked`, checks, a `review` and `limits`, what it
does not do. An argument whose whole value is `${input.<name>}` or `${steps.<id>.<path>}`
(dotted field names and array indexes) takes that JSON value; a step with `poll` is
repeated until its field is terminal. Supply `expected_revision` and `operation_id`
wherever a tool asks for them. A recipe never runs on the worker: run its steps yourself,
so a recipe that stops halfway leaves identifiable committed operations. A review of kind
`preview` names the render and the steps that read it: do not report the recipe done until
you have run them and looked at the image; the worker cannot see what you read. The
delivery recipe stops at rendered frames held as job artifacts, because nothing publishes
them to a project or shared folder yet.

`cast.list`, `cast.usage`, `scene.dependencies`, and `scene.validate` read the
current detached document. Supply its `document_id` and latest `expected_revision`
on every page, with optional `offset` and `limit`; every paged read returns a null
`next_offset` (`next_index` for `rig.inspect`) on its last page. Check `traversal_complete` and
`reasons`: paging an incomplete result cannot finish the omitted traversal or
establish that omitted artwork is unused. Reads inspect at most 100,000 root-sheet
cells and retain at most 4096 records; each response is limited to one MiB.
Dependency reads inspect authorized file metadata without loading images or
expanding sequences, sidecars, or nested sheets. A dependency's `availability` is
`present` (with `size_bytes`), `missing`, `unreadable` (the file exists but cannot
be opened for reading, or a folder on its path denies access; no size is
reported), `forbidden`, `unsupported`, `not_checked`, `unresolved` or `unsaved`. Linked Library origins are outside
this profile. These checks do not certify package or render compatibility.

`fx.macro.create` groups 2–64 connected over, blur, or glow effects into a native
macro. Members must be top-level effects with one output root and no non-root
consumers or terminal links outside the selection. Generators, nested macros,
and linked parameter groups are rejected. Creation retires the selected handles;
`fx.macro.expand` returns fresh handles for the members. Exact retries retain the
original mappings, including after those handles retire.

`fx.graph` reports macro members and exposed input mappings. Connect, disconnect,
and terminal operations can edit the macro boundary; expand before editing member
parameters, names, enable state, presets, or deleting the macro. A macro exposes
at most 128 inputs, and its container and members count toward the 4096-node graph
limit. Scene packages and render snapshots verify member parameter state and
boundary connections through native save/reopen before publication.

PNG inputs support up to 8 bits per channel, 8192 pixels per axis, and 16,777,216
pixels per image. Vector drawings are limited to 4096 strokes and 32768 control
points, with coordinates within ±32768 and radii up to 255 native units. Palettes
support up to 4096 unlinked solid styles, without reference images or studio
palette links. Style names cannot contain NUL or start with `|`. Palette keyframes
use frames 0–65535 for vector levels and 0–999999 for Toonz raster levels. Consult
the running catalog for the remaining operation-specific bounds.

`vector.structure` reports stroke order and native group spans, including nested
groups, with at most 4096 strokes/groups, depth 16, and 1 MiB of metadata. Spans
expire when the document revision changes; native stroke IDs are not persistent
across reopening PLI files. `vector.group.create`, `vector.group.remove`, and
`vector.order.move` edit flat groups at the root context. They require explicit
`all_exposures` ownership and reject nested drawings, partial groups, destinations
inside another group, and edits that would change painted boundaries. A move's
`before_index` refers to the original order; the stroke count means the front/end.
Exact no-ops preserve the revision. These detached tools do not change editor
selection or create native undo entries.

`text.fonts` lists installed font families and styles on the worker host.
`vector.text.create` creates one new drawing of editable native vector outlines,
using the application's plain-text layout and converter. Supply an exact installed
family/style, point size, native baseline and existing solid palette style. The
operation does not overwrite drawings or add exposures. It rejects font fallback,
missing or bitmap-only glyphs, and outlines whose native regions cannot be classified.
Text is bounded to 1024 UTF-16 units and 32 lines, plus the drawing geometry limits.
The result includes resolved font runs, bounds and a source recipe; saved PLI contains
ordinary strokes, groups and style references, not a semantic text object. Keep the
recipe separately for later regeneration. These detached tools have been qualified
on macOS ARM64; installed fonts and accepted outlines can differ on other hosts.

Palette pages can be inspected, created, renamed, reordered, and removed when
empty. A palette admits up to 128 pages; page zero and transparent style zero
stay first. Style moves preserve numeric IDs and artwork references. Style
renames and copies preserve all animation keys; a copy receives a new ID.
Page names use 1–128 characters without NUL; vector page zero cannot begin with
`refimage`, which the native PLI reader reserves. Physical style deletion and
unpaged styles are not supported.

The `rigging` toolset holds the hook, handle, pivot, offset-layer and rig tools below;
the `object.parent`, `animation.*` and `drawing.*` behavior they rely on is in `core`.
`rig.inspect` and `rig.evaluate` read the actual stage hierarchy: parents, handles,
stored pivots, hook bindings for each exposed level, offset-layer summaries, and stage
placements in inches at integer frames. Supply the latest `expected_revision`.
`rig.inspect` pages at most 512 objects or 1 MiB; `rig.evaluate` takes at most 512
object-frame pairs. Every scene is limited to 4096 stage objects and 64 parent edges.

`hook.inspect`, `hook.set`, and `hook.delete` edit level hooks `H1`–`H99`. Hooks belong
to the level, so an edit moves every column that exposes it: results list
`columns_affected` and `users`, and edits are refused while any of those columns is
locked. Positions are inches on unsuffixed drawings within ±1000 inches; vector levels
use 53.33333 native units per inch and raster levels use pixels at the level DPI.
Native hook files keep about six significant digits, so `hook.set` canonicalizes
through the native writer and rejects a displacement above `max_coordinate_error_inch`.
A `b` within one native unit of `a` is rejected rather than snapped. A level holds at
most 99 hooks and 8192 keys, a scene 65,536 keys. Removing a hook's last key is refused
while objects use it. `drawing.renumber` moves hook keys with their drawings and refuses
suffixed IDs for them, `drawing.remove` removes them, and `drawing.clone` does not copy
them; each reports the count. Scene packages carry each level's native
`<level>_hooks.xml` sidecar in the manifest, and reopening compares it with the source.

`object.handles.set` sets an object's own handle (its pivot origin) and parent handle
(its attachment point) to a peg-hole letter or a column hook, without compensation. As
in the editor, an own hook handle resets the stored center and offset; `pivot_reset`
reports their previous values. `object.pivot.set` sets the stored center in inches and
either preserves stage placement at one frame or keeps the offset; center and offset
are canonicalized like hooks. Both return `world_changes` for up to 64 `report_frames`,
where `preserved` means within 1e-6 inch and 1e-9 in the linear terms. `object.parent`
requires a `placement_policy`. `preserve_local` keeps local keys, so the object may
move. `preserve_world` keeps the object where it is by rewriting its own channels: when
everything between the old and new parent and their common ancestor is static and the
change is a translation, or a rotation and uniform scale of a static position, it adds
constants to the curves (and scales the global scale curve), which is exact at every
frame (`world_preservation.method: "exact_rewrite"`). Otherwise it is refused with
`unsupported_target`, a `reason`, and a `bake_proposal` you can send back as
`world_policy: {kind: "bake", start_frame, end_frame, new_key_interpolation}` to key x,
y, rotation and scale (and z or stacking order where they change) at every frame of
that range of at most 10,000 frames; a bake is exact at those integer frames only, keeps
keys outside the range, and refuses objects with offset layers, shear or tilt. Both
verify the result and report the measured deviation. `object.parent` also accepts an
optional `parent_handle` and `report_frames`, and refuses a hook handle under a parent
that is not a column.

`object.duplicate` copies a pegbar (`scope: "object"`), or a pegbar or level column
with everything below it (`scope: "subtree"` with `level_sharing: "shared"`), as the
stage schematic's copy and paste does: curves and offset layers are independent, copied
columns are appended to the sheet and show the same levels and cells, effects fed only by
the copied columns are cloned, and the copy keeps the source's parent and handles. The
result maps every source object to its copy. Objects that follow a motion path, cameras
and non-level columns are refused. `object.rename` renames a pegbar or column; names
need not be unique. `rig.dependents` lists what depends on an object (children, the hook
parent handles they hold, motion paths evaluated in its space, pose entries), a hook (its
users) or a layer. `object.delete` removes a pegbar after the same scan:
`children_policy: "refuse"` refuses any child, `"reattach_to_parent"` moves the children
to its parent with their local keys and reports their `world_changes`; pose entries
always refuse. Columns are deleted with `column.delete`. `object.group`,
`object.group.rename`, `object.ungroup` and `object.groups` manage the stage schematic's
named groups by `group_number`: grouping changes no parent or placement, only whole
existing groups can be grouped again, and only an outermost group can be dissolved.
Groups and their names are saved in the scene.

`offset_layer.create`, `offset_layer.update`, and `offset_layer.delete` manage additive
layers on pegbars, columns, and cameras that do not follow a path: at most 16 per object
and 1024 per scene, with unique names and weights within ±10. An enabled layer adds
weight × curve to each channel; scale offsets add to the scale factors. Edit layer curves
with `animation.keys`, `animation.set_keys`, and `animation.delete_keys` by passing
`layer_id`, using channel `weight` for the layer weight. `animation.sample` reports
`base_value`, `offset_value`, and the effective `value`; with `layer_id` it reports the
curve value and `weighted_value`, which is 0 while the layer is disabled. Layer handles
survive renames and reordering, expire on deletion, and are reissued when a package is
reopened. Whole-curve copy and retime still refuse objects with layers. IK, pins,
published channels, Plastic deformation, and hook tracker regions are outside this
profile, and these tools have no live-editor or undo counterpart.

`animation.insert_breakdown` is the editor's Set Breakdown Key on a stage base channel
(not `path_position`, weights or offset layers): it adds a key at an integer `frame`
strictly between two keys, with either `favour_percent` (-100 to 200) of the way from the
previous key's value to the next one's, or an explicit `value`. The new key takes the
segment's interpolation and step. A speed segment is cut so its shape is kept before the
value is applied; an ease segment gives the new key no ease and clamps its neighbours' eases
to the new span. Only the two segment ends' facing handles can change; every other key and
every sample outside the split segment is bit-identical. A frame that is already a key, lies
outside the keys, or sits on a curve with expression, file, similar-shape or cyclic content
is refused and nothing changes.

`animation.move_keys` and `animation.duplicate_keys` take `frames` (existing key frames),
`frame_offset` (whole frames, negative allowed) and `value_offset` on a stage base channel.
A move keeps every moved key's interpolation, step and handles and changes only its frame
and value; it keeps the key order, so it is refused when a key would land on or cross
another key (`key_exists`, `order_changed`), leave its frames or values out of bounds, or
leave a handle outside its changed segment (`controls`). A duplicate writes verbatim copies
of the named keys; it never replaces a key, and it is refused with `incoming_kind_changed`
when a copy or the key after it would read an incoming handle of a different kind than it
has now (use `animation.set_keys` to state such a segment). Both tools leave every other
key bit-identical, and the samples of every segment that touches no moved or created key
are bit-identical before and after, and after `scene.save_copy` and `scene.open`. The
reason is in the `reason` detail of the error.

`animation.set_cycle` turns the editor's Cycle on or off for a stage base channel curve
(not `path_position`, weights or offset layers). A cyclic curve repeats its key span past
its last key and adds the last key's value minus the first key's each repeat; `cycle`
appears in `animation.keys`, the edit results and the `animation.channels` descriptors
(with `cycle_supported`). On those channels `animation.set_keys`, `animation.delete_keys`,
`animation.insert_breakdown`, `animation.move_keys`, `animation.duplicate_keys`,
`animation.copy_curve` (which copies the flag with the curve) and `animation.retime_curve`
accept a cyclic curve and keep its flag; samples at the endpoints, at subframes, before the
first key and past the last key match the native evaluator after `scene.save_copy` and
`scene.open`. `animation.sample` on a cyclic curve refuses with `resource_limit` when its
requested frames would need more than 2000000 repeats in total. Cyclic offset-layer,
weight and path curves are still refused as before.

`animation.channels` publishes every curve target with its `kind`, a `target` record to pass
back, and `edit_via`, the tool that edits it. Name exactly one of `object_id` (stage
channels; with `layer_id` that offset layer's channels and weight), `fx_id` (every numeric
parameter component of the effect, named parameter-dot-component for pixel and point
parameters, such as the Red component of a color), or `column_id` (the `time_remap`
curve of a level column, with `remap_enabled`). `animation.keys` and `animation.sample`
read the same targets by passing `fx_id` or `column_id` with the descriptor's `channel` and
`unit`. Effect parameter and remap curves are read only here (`writable` is false): edit an
effect parameter with `fx.parameter.set_key`; remap curves have no edit tool. A curve with
expression, file or similar-shape keys is listed with its keys marked unsupported, without
their text, and `animation.sample` refuses it with `unsupported_interpolation`.

`scene.properties.inspect` reports the stored output and preview blocks (range,
effective range at the current frame count, step, shrink, bpp, aspect export, frame rate),
the color-settings sync flag, column filters, background color, field guide, markers and
ruler guides in inches, and one `settings_scope` row per setting: whether it is scene data,
a project default, machine tuning or editor view state (safe area, viewer aspect guides,
camera box), and which tool edits it. `scene.output.set` changes only the blocks named in
`apply_to`; 128 bpp and linear color are not settable. Worker renders take their own
explicit settings and never read these stored ones: `render.preview`, `render.submit` and
`render.preflight` take `aspect_export` (`none`, or `letterbox`/`crop` with a ratio and an
optional fill color), and the preflight reports the written `output_width` and
`output_height`. `scene.guides.set` replaces the field
guide, markers or each ruler guide list; guides change no rendered pixel. `scene.create`
and `scene.settings` take an explicit `background_color`. Guides, the field guide ratio
and the aspect export ratio are saved at full precision.

`camera.list` pages through the sheet's cameras with their output and preview roles;
`camera.inspect` and `camera.update` take an optional `camera_id` (the output camera
otherwise) and report DPI per axis, square pixels and the interest rectangle. A
resolution change while an interest rectangle is set needs `interest_rect` (null clears
it), since the native resolution setter would drop it. `camera.create` takes an explicit
name, resolution and size; names are unique and never `Camera<digits>`, which native
code ties to the camera number. `camera.duplicate` copies curves, cycle flags and offset
layers independently. `camera.select` sets the output (render) and preview roles.
`camera.remove` refuses the last camera, needs a replacement for each role the camera
holds and `children_policy: "reattach_to_parent"` for stage children, and keeps camera
numbers packed: higher cameras are renumbered with their handles, curves and children,
and one with no stored name keeps the name it showed. Without packing, a gap reloads as
a phantom camera.

`column.create` appends a column or inserts it before `position`; `column.move` moves a
block of columns, in their current order, before a position of the original order. In
both, every column keeps its handle, stage object, parent, curves and FX links.
`column.update` sets `visible` (both flags) or `preview_visible` (renders) and
`camstand_visible` (viewer) separately, and `color_filter_id` (one of the scene's
filters). `column.delete` refuses a column whose FX feeds an effect until `fx_policy` is
`delete_exclusive` (removes the effects fed only by the column) or `disconnect` (keeps
them with an empty port), and a column with stage children until `children_policy` is
`reattach_to_parent` (children move to the column's parent with their local keys;
`report_frames` returns their world change). `column.duplicate` copies a level column in
its sheet: cells share the same levels, the stage object, its curves and offset layers
are copied independently, the parent is kept or set to the table, and
`fx_policy: "exclusive_branch"` also copies the effects fed only by the source; a macro
among those effects is refused with `unsupported_target` (expand it first), for
`delete_exclusive` too.
`column.kinds` lists every native column kind with the tools that create and delete it.

The cell tools are the `sheet` toolset (start the adapter with `--toolsets sheet`, or
enable it with `toolsets.enable`). They edit up to 64 level columns per call, all or nothing; rows are
zero-based frames valid at the call's `expected_revision`. `cells.set` writes explicit
runs of drawings and blanks without shifting (`overwrite: false` refuses occupied rows
with `occupied_cells`). `cells.clear` blanks an interval and keeps later frame numbers;
`cells.remove` deletes the interval and moves later cells up; `cells.insert` adds blank
rows and moves later cells down, leaving a column with nothing at or after the frame
unchanged. `cells.copy` copies exposure references (never drawings) to as many
destination columns, inserting or replacing, and `source_after: "clear"` makes it a move.
`cells.retime` applies one native timing function to the same interval of each column
(reverse, swing, step, each, reframe, increase, decrease or reset step, repeat, roll up or
down, time stretch); later cells of a column move with it. `cells.replace` swaps one level
for another, or explicit drawing pairs, inside an interval without changing timing. Rows
0 to 999999 are admitted everywhere, the native sheet's 1,000,000 rows: `column.expose`,
`column.cells` and the cell tools refuse a later row with `invalid_params` and
`details.reason: "frame_range"`, and a package saves any exposure inside it. Each call checks every column first: a locked, sound,
generator or sub-xsheet column, a missing drawing, a cell past frame 999999 or more than
1,000,000 stored cells refuses the whole call. The worker computes the result with its
own cell model, applies it natively, and compares every touched column with the model; a
difference is `partially_applied`. Results list each column's `cells_changed`,
`rows_delta` and cell range before and after. A non-empty `sheet_path` is refused with
`unsupported_target` until nested sheets become editable. `column.expose` uses the same
service.

`level.inspect` reports a vector, Toonz raster or RGBA level's interpretation `settings`:
`dpi_policy`, the image size, the native `dirty` flag, `hook_keys`, and for each of `dpi`,
`premultiply`, `white_transparent`, `antialias_softness`, `subsampling` and
`color_space_gamma` its `value`, whether it is `applicable` to the format, whether it is
`writable` now, and the `reason` when not (`not_applicable_to_format`,
`linear_raster_only`, `unsaved_level`, `read_only_level`, `locked_exposure`,
`read_only_document`). The rules are those of the editor's Level Settings: DPI, antialias
and subsampling for raster and Toonz raster, premultiply and white transparency for RGBA
only, colour-space gamma for linear (EXR) raster only, which no worker level is;
subsampling and gamma only on a level with no unsaved changes. With `include_usage: true`
it also lists the level's exposures in every sheet, nested sub-xsheets included: `cells`,
`columns`, `sheets` and up to 256 rows (`sheet_level_id` null for the top sheet, the
column, its lock and its first and last frame). The scan stops at 1,000,000 cells or 128
sheets and then says `complete: false`; no write relies on an incomplete scan.

The level lifecycle tools are in the `sheet` toolset. `level.settings.set` changes
settings all or nothing. A setting outside the format or the dirty rule is refused with
`unsupported_target` and `details {setting, format, reason}`, a read-only level with
`locked`, and any change while a column of any sheet that exposes the level is locked with
`locked`, listing those columns. Premultiply and white transparency are never both on:
turning one on while the other is on needs `coupling: "clear_other"` (`invalid_params`,
`coupling_required` otherwise; asking for both is `both_on`), and the result names it in
`coupled_cleared`. A DPI is always a custom DPI; it changes the level's size on stage
(`stage_size_in_before` and `_after`), never its pixels or the camera (`pixels_resampled`
and `camera_changed` are false); the result also gives the settings before and after,
`changed`, the `exposures` count and exposing `columns`. Setting the values a level has
changes nothing and keeps the revision. `level.rename` keeps the level's handle, cast id
and exposures, needs a name no other level has (`name_conflict`; matching is exact), and is
admitted under a locked exposure since no picture changes; renaming a level to its own name
keeps the revision. `level.duplicate` makes an
independent level: every drawing copied, the palette cloned, every setting, the hooks,
tags and cover copied, a new cast id, no collection, exposed nowhere (retarget exposures
with `cells.replace`); unlike the editor's Clone Level it loses no setting.
`level.remove` takes a required `exposure_policy`: `refuse` removes only a level nothing
exposes and otherwise answers `in_use` with the usage in its details; `clear` blanks every exposure
in every sheet without moving later cells; `replace` shows the same drawings of
`replacement_level_id` there, keeping timing, and refuses a replacement that lacks an
exposed drawing (`missing_drawing`) or has another format unless `allow_type_change` is
true (`unsupported_target`, reason `type_change`). Locked exposures refuse with `locked`,
and an object attached to a hook of an exposing column refuses `clear`, or a replacement
without that hook, with `has_dependents`. Removal takes the level out of the cast only:
no file is deleted (`files_deleted` is 0), collection memberships stay dormant as in the
editor, and its handle stops resolving. `drawing.remove` takes the same kind of plan:
`exposure_policy` `{kind: "refuse"}` (the default), `{kind: "clear"}`, or
`{kind: "remap", drawing_id}` naming another drawing of the same level; the result lists
the exposures it rewrote and their columns. In short: `cells.clear` keeps
frame numbers and `cells.remove` shifts them; `drawing.renumber` renames drawings and moves
their exposures, while showing other drawings at some frames is `cells.replace`;
`level.remove` removes a cast entry, never a file; `level.duplicate` and `drawing.clone`
copy, while repeated exposures share one level.

`scene.save_copy` writes a new package directory containing `scene.tnz`, its
artwork, and a hash manifest. It checks every saved drawing through the native
reader and compares palette styles, pages, and keys before publishing. Existing
destinations are rejected. Each artwork level must contain at least one drawing;
an empty scene is allowed. Packages are limited to 256 MiB; before artwork grows,
the worker checks limits of 256 levels, 4096 drawings, and a separate 256 MiB
aggregate estimate covering raster and vector frames plus conservative overhead
for frames, palette pages, styles, and keyframes. This estimate is an admission limit,
not a bound on process memory.
The manifest also records the saved sheet: each saved column's kind, name,
preview and camstand flags, lock, opacity, colour filter, parent and a digest of
its cell runs; every camera's size, resolution, interest rectangle, parent,
output and preview roles and a digest of its curves; the output and preview
settings, background, field guide, markers, ruler guides and colour filters; and
each level's interpretation settings. The package is reopened before it is
published and any difference is refused with `invalid_package`, whose details
name the `section` and the first differing `path`. The native saver writes no
empty column after the last used one; `trailing_empty_columns_dropped` lists the
handles of those columns, which the reopened scene does not have. Camera sizes in
inches are written at full double precision, so a size set through `scene.settings` or
`camera.update` reads back exactly after a reopen.
Native PLI encoding quantizes stroke coordinates and widths, and PNG encoding
can change premultiplied RGB values by one code value. These files are editable
native representations, not bitwise copies of in-memory drawings. Saved raster
levels use explicit custom DPI to preserve authored physical size, including
fractional DPI that PNG metadata alone cannot represent exactly.

The `shared-library` toolset reads granted shared roots and never writes to them.
`shared.roots` reports each role's grant, availability and native layout.
`studio_palette.list` pages one folder (at most 8192 entries, 256 per page):
subfolders, then `.tpl` palettes with the palette id read from the first 64 KiB.
`studio_palette.inspect` reports a palette's id, `sha256:` file revision,
lock and read-only state, pages and up to 128 styles per page with their link
state (`source`, `on`, `off` or `malformed`). `studio_palette.find` scans the
palette roots for an id within 20,000 entries, depth 16, 8192 files, 256 MiB and
30 seconds; a scan that hits a bound or skips a symlink reports `partial`, and
duplicate ids are listed rather than resolved. `library.list` and
`library.inspect` report each template or pose set's sidecar identity:
`not_recorded` when there is none, never an invented id; `malformed`,
`kind_mismatch` and `duplicate_id` otherwise. `library.inspect` lists every
dependency file with its location (`item_folder`, `library`, `outside_library`
or `unresolved`) and digest, plus a `dependency_digest`; files outside the
Library are never read. `library.thumbnail` returns a template icon or one pose
thumbnail re-encoded as PNG of at most 512 pixels. Listings carry a
`listing:` revision; folders that change while listed return
`revision_conflict` with `details.scope: "listing"`. Names and paths are
root-relative with forward slashes, NFC, and segments of 1–120 characters;
hidden entries, symlinks and junctions are skipped and counted, never followed.

Every studio palette, template and pose-set file is hashed, copied privately and
parsed by a short-lived helper process of the same executable, never in the
worker. Palettes are limited to 4 MiB, 4096 styles and 128 pages; templates to
16 MiB and 256 levels; pose sets to 4 MiB and 1024 poses. A helper that crashes,
runs past 10 seconds or exceeds 1 GiB of memory is stopped; the call returns
`unsupported_palette` or `invalid_library_item` with `details.cause` set to
`crashed`, `timeout` or `memory`, and the worker and its documents remain
available. A file the native parser rejects returns the same code with
`details.cause: "rejected"`. Each parse adds about half a second.

User scenes open read-only in the `core` toolset. `project.list` lists the read
roots (the workspace and each `--input-root`) and the native projects under them,
at most depth 4, 20,000 entries and 10 seconds per call, paged at 64. A folder is
a project when the native project-file search finds a file in it; the row names
that file, its format (`otprj`, `prj63ml`, `prj6`, `prj` or `tahoma`), a `sha256:`
revision (files over 1 MiB are listed unhashed with `status: "too_large"`) and up
to 8 `alternates`. Project files are never parsed by the listing. Hidden entries
and symlinks are skipped and counted, and the workspace's runtime, render and
worker project folders are not listed. `project_id` hashes the canonical project
file path; a worker accepts only ids it returned. Listings carry a `listing:`
revision and refuse a stale `expected_listing_revision` with `revision_conflict`.
The editor's project roots and `TOONZPROJECTS` are never consulted.

`scene.open_read_only` opens a `.tnz` of at most 64 MiB from a read root with an
explicit `project_id`, or `standalone` for none. It finds the scene's `scenes.xml`
by walking up to, never beyond, the read root that holds the scene. By default the
selected project must match that native binding (`standalone` matches when there
is no `scenes.xml` within the roots); otherwise the call returns
`project_mismatch` with `details.native_binding` (`differs`, `outside_roots`,
`unresolved_name`, `unreadable` or `none_within_roots`) and the native project's id
when there is one. `project_binding: "override"` opens with the selected project
anyway and reports `binding: "override"`. The scene is copied and loaded in its own
scene host process: the same executable, holding only that document, with the
project's file-path rules, no media tools and no shared roots. The result reports
the source revision and `source_fingerprint`, the project folders with their
resolved paths (`null` with `authority: "outside_roots"` for a folder outside the
roots, which is never read), counts, every unsupported item in `content` and the
render admission in `render`. A worker holds at most 4 read-only documents; each
also counts toward the 32-document limit. Opening is a write: retry with the same
`operation_id` to get the same document.

The host loads each level itself and records why one did not load: `missing`,
`forbidden` (outside the roots), `partial`, `unsupported_samples`, `failed` (a
reader error or a first frame over 16,384 pixels per axis or 268,435,456 pixels),
`creator_forbidden`, or `not_applicable` for formats this profile does not load
(PSD, movies, EXR, SVG and mesh levels). A scene is refused with `scene_limit` past
16 nested sub-xsheets, 1024 sheets, 9999 columns per sheet, 4096 levels,
1,000,000 drawings, 4096 effects per sheet or 16,384 per scene (column FX are bounded
by the column limit instead), 16,384 stage objects per sheet or 65,536 files read;
`invalid_scene` with `details.cause: "rejected"` carries the native parser's message,
for example for a cell past row 999,999 or an effect id this build does not know.
Scene loading has one known performance limit: the native reader lists a sequence
level's whole folder for every such level, so thousands of sequence levels sharing
one folder load in quadratic time (about 4000 of them pass the 60 second open
deadline, while the same levels in folders of their own open in seconds). A host that
crashes, takes more than 60 seconds to open or 30 seconds to answer a read, or
exceeds 2 GiB of memory is stopped: an open returns `invalid_scene` with
`details.cause` `crashed`, `timeout` or `memory`; afterwards the document stays
listed with `host_state: "lost"` and every call on it returns `scene_host_failed`
until `scene.discard` removes it. The worker and its other documents keep serving.
A host ends with its worker: on macOS and Linux it exits when its parent process
goes, and on Windows the worker holds it in a job object that closes with the worker.

A read-only document's revision is always `"0"`. `scene.summary` (with optional
`include_drawings: false`), `camera.inspect`, `level.inspect`, `cast.list`,
`cast.usage`, `scene.dependencies`, `scene.validate`, `fx.graph`, `scene.inventory`,
`scene.source_status`, `column.cells`, `drawing.inspect`, `palette.inspect`,
`palette.pages`, `fx.parameter.get`, `animation.channels`, `animation.keys`,
`animation.sample`, `vector.inspect`, `vector.structure`, `vector.opacity.inspect`,
`raster.read_pixels`, `render.preview` and `render.submit` run in its host; the job and
artifact tools follow the job. Reads that decode drawings first re-check that level's
pinned files and return `source_changed` if one changed.
Production reads report `profile: "native-read-only-v1"`; cast rows add `kind`,
`load_status` and `cover_status`, dependency rows add `pinned`, and `scene.validate`
adds `unsupportedColumn`, `externalCodeFx`, `fileResourceFx`, `missingDependency`,
`forbiddenDependency`, `levelLoadFailed`, `unrecognizedSceneData`, and the info
advisories `authoringRangeExceeded`, `frameRateMismatch` and `resolutionMismatch`
(the scene's frame rate or camera differs from its project's scene defaults).
`scene.dependencies` also lists sound files (`sound`), hook files (`hook_sidecar`),
texture images (`texture`; shipped library textures are `unsupported`), the project's
raster drawing palette (`raster_drawing_palette`) and FX file resources
(`fx_file_resource`). `fx.graph` gives every effect an `admission`:
`class`, whether it renders, and the authoring ranges a builtin effect exceeds;
effects outside `builtin-2d-v2` are described without parameters; `sheet_path`
describes a sub-xsheet instance. Every other tool returns `read_only_document` before
it reaches the host: `details.reason` is `mutation` for writes, including
`scene.save_copy`, and `not_qualified` for reads not yet available, with
`details.slice`.

`scene.inventory` pages through a document's `sheets`, `columns` or `stage_objects`,
128 rows a page and at most 65,536 rows a section (`reasons: ["row_limit"]` past
that). Every row carries `sheet_path`, the instance path of `{column_id, level_id}`
steps from the top sheet; a sub-xsheet exposed twice is two sheet rows with one
`level_id` and `instance_count: 2`. With `sheet_path` the columns and stage objects of
that instance are listed, and the sheets below it. Names repeated within a sheet are
`duplicate_name`. Child-sheet column handles name inventory rows; column tools accept
top-sheet columns only. It works on editable documents too and never creates objects.

Every tool that takes a name states its limit in its input schema, and the dispatcher refuses
a longer or malformed name as `invalid_params` before the tool runs: 1 to 128 Unicode
characters without NUL for scenes, levels, columns, pegs and sound sources (as for palette
pages and styles); 1 to 256 without NUL or control characters for effects, macros and offset
layers; 1 to 256 without NUL for motion paths. The native level set can list one level twice
when a scene repeats a level's name; `cast.list`, `scene.dependencies`, `cast.usage` and the
counts of `scene.open_read_only` list each level once, by its handle, and two levels may share
a name.
`scene.source_status` re-checks every file pinned at open and reports each change as
`modified`, `replaced` (a new file in its place), `missing` or `unreadable`, with
`state: "missing"` when the scene file itself is gone; editable documents return
`unsupported_target`. Its `verification` says what was compared:
`content_checked_files` were compared by content as well, `metadata_only_files` by size,
time and file id only, with the first 64 `metadata_only_paths`. On volumes with coarse
timestamps (HFS+: one second, exFAT: ten milliseconds) a same-size rewrite in place
within one tick is detected for content-checked files only.

`project.inspect` with a `project_id` from `project.list` parses that project file in
a parse helper (10 seconds, 1 GiB) and returns its format, version, file-path rules,
scene defaults, alternates and folders with `authority` (`workspace`, `input_root`,
`outside_roots` with no path, `unresolved`, or `scene_dependent`); the file is hashed
before and after. `project.scenes` lists the folders and `.tnz` files under the
project's scenes folder, or a `folder` inside it, at most depth 8, 20,000 entries and
10 seconds, 128 a page, with a `listing:` revision. A scenes folder that is
scene-dependent, outside the roots or missing gives `scenes_folder: null` and an empty
page with `reasons: ["unreadable"]`. `standalone` has no project file and returns
`invalid_params` from both.

Read-only renders produce PNG only (MP4 returns `render_unavailable`). A render is
refused with `render_blocked`, listing at most 64 `blockers`, when the scene holds an
external program, an effect that reads files, or a texture outside the roots
anywhere, or when the current output reaches an unsupported level, column or effect,
or a shipped library texture; a camera without a finite positive size and DPI also
blocks. A rendered level whose file is missing or did not load fails the
`dependencies` check first and returns `missing_dependency`. `render.preflight`
reports all of these for a read-only document, with the worker's own lane and
retention checks. Every file the scene reads is pinned at open by size, nanosecond
modification time and file id; each regular file up to 64 MiB is also pinned by a
SHA-256 of its content while the scene's content-pinned total stays within 512 MiB (the
open result's `content_pinned_file_count`). A render re-checks the pins before it starts
and before it publishes, and a read that decodes drawings re-checks that level's pins;
each returns or fails with `source_changed` when a source changed. Results report
`snapshot_inputs: "linked-source-stat-pinned"` and the `source_fingerprint`.
`scene.discard` stops the host and removes its private copy and outputs; it is
refused with `render_busy` while one of the document's jobs is running.
`automation.list_documents` reports each document's `access` and `host_state`.

`scene.save_copy` registers each package in the workspace journal by the SHA-256
of its exact `manifest.json` (`package_id: "package:<hex>"`, `registered`). `scene.open`
and `scene.package_info` admit a package by that registered content, wherever it
now lies inside the workspace or an input root and whichever worker of the
workspace saved it: the manifest must hash to a registered id, and every file must
match the registered manifest. `scene.open` copies the files into the worker's
private storage, checking each while copying, and loads the copy, which goes with
the document. The result says how it was admitted (`admission`: `this_worker` or
`workspace_registry`), `package_id`, `relocated` (moved since it was saved),
`published_by {worker_instance_id, current}` and `provenance`. Editing the
manifest cannot authorize a changed scene. Refusals are `unregistered_package`
with `details.reason` `not_registered` (another workspace, or never saved),
`manifest_changed` (another manifest is registered at that place) or
`registry_expired` (beyond the newest 4,096 registrations), `details.next:
"scene.import_editable"` and `details.hint: "scene.open_read_only"`; a changed,
missing or unlisted file is `invalid_package`. Saved files remain available for
normal application use after the worker exits. Render jobs use separate copies
of the scene and artwork. This process isolation is not an operating system
sandbox for native image readers, and the workspace must not be modified
concurrently by another process.

The `audio` toolset imports sound and places it on the timeline. `audio.import`
copies a WAV or AIFF file into the workspace as a sound source after checking its
structure without the native reader: 8-, 16- or 24-bit PCM or 32-bit float WAV, 8-,
16- or 24-bit AIFF, mono or stereo, 8000–96000 Hz, at most 160 MiB, 600 seconds and
128 MiB decoded. Float samples must be finite and within ±2.0. Extensible WAV,
AIFC, 32-bit integer PCM and 32-bit AIFF return `unsupported_format`;
damaged files return `invalid_asset`. A document admits 64 sources, 32 sound
columns and 1024 clips (256 per column). Imports, clip placement and mixdown
require a frame rate that the scene file stores exactly, such as 23.976, 24 or
29.97, and return `timing_conflict` otherwise. `scene.settings` refuses a frame
rate change while any clip exists, because the native change rounds clip
positions.

`audio.column.create` appends a sound column with an explicit `volume_permille`
(0–1000); `audio.column.set_volume` changes it. `audio.clip.place` puts source
frames `[source_start_frame, source_start_frame + frame_count)` at
`timeline_start_frame`. A clip is addressed by `{column_id, timeline_start_frame}`
at the current revision. Overlaps return `audio_overlap` with the conflicting
clip keys; nothing is trimmed. A clip that would continue an adjacent clip of the
same source is refused, because the native column would join the two.
`audio.clip.update` replaces a clip's placement and `audio.clip.remove` keeps the
source in the cast. `column.update` mutes a sound column with `visible: false`
and `column.delete` removes it with its clips; sound columns have no opacity.
`column.cells` and `column.expose` refuse sound columns and sources.
`scene.summary` reports each column's `kind` and sound sources as format `sound`.
`audio.inspect` reports sources with their format, digests and native frame
count, columns with clips and source sample ranges, and the columns a mix
includes or excludes.

`audio.mixdown` writes the native mix of frames `[start_frame, end_frame]` to a
new `.wav` file in the workspace without using an audio device. The first
included column's first clip sets the format, 8-bit mixes are written as 16-bit,
integer samples saturate and float samples can exceed full scale (see `peak`).
Muted and empty columns, later columns at zero volume, and columns whose clips
span a single frame (empty natively) are excluded. A clip ending at the first
mixed frame, or starting just after the last one, can contribute one boundary
sample, as in the native mix. Mixdowns are limited to 600 seconds and 64 MiB of
PCM, and a worker writes at most 128 files and 256 MiB. The written file is
decoded again and compared with the mix before it is published.

Packages store each sound source as a byte-exact copy, and `scene.open` checks
every source, clip, volume, mute and lock state and the frame rate. Saving refuses
an empty sound column that is locked or muted, and a last sound column whose clips
span a single frame, since native scene files drop them. Trailing empty columns
are not saved. Renders mute sound columns, so movies stay video-only.

The `storyboard` toolset edits storyboard panels, which are the root sheet's
navigation tags. A panel starts at `panel_frame` and runs to the frame before the
next panel; the last runs to the scene's last frame or its own frame, whichever is
later. `storyboard.panels` lists each panel's span, caption, notes, color, and the
drawing it shows: the topmost visible level column's drawing at the panel frame.
Render that frame with `render.preview` for a thumbnail. `storyboard.panel.create`
adds a panel; a frame that already starts one returns `already_exists`.
`storyboard.panel.update` changes the caption, notes or color, or moves where the
panel starts with `new_panel_frame`; cells and timing do not move.
`storyboard.panel.delete` removes the boundary only. A document admits 4096
panels, 256-character captions and 4096-character notes. Captions cannot contain
control characters; notes can contain line feeds but no other control characters.
Panel colors are stored per panel and do not change the editor's default tag
color. Notes need a newer reader: `older_reader_compatible` is false whenever any
panel has notes. Packages record a digest of the panels and `scene.open` compares
it.

The `lipsync` toolset applies mouth timing to a drawing column. `lipsync.import`
reads a Papagayo `.dat` file (`MohoSwitch1` header, `<frame> <shape>` lines with
1-based, strictly increasing frames, Preston Blair shapes with `other` as `etc`) or
a Rhubarb TSV or JSON file (seconds with at most six decimals, shapes A–F, or A–H
and X for `rhubarb_extended`). Parsing is strict: any other line, a decreasing
time, a gap between JSON cues or an unknown shape returns `invalid_asset` with the
line. A DAT import needs its `frame_rate`; Rhubarb imports must not give one. Files
are limited to 1 MiB and 20,000 cues, and a worker keeps 64 immutable analyses.
`source_link` binds an analysis to a sound source's PCM digest; `lipsync.inspect`
pages its cues.

`lipsync.apply` replaces the cells of one unlocked level column with drawings of a
vector, Toonz raster or RGBA level, mapped per shape. `expected_frame_rate` must
equal the scene rate, and a DAT applies only at its own rate (`timing_conflict`).
DAT frame `f` lands on `start_frame + f - 1`. A Rhubarb cue at `t` microseconds
lands on `start_frame + floor(t * fps / 1e6)`, computed exactly from the decimal
rate the scene file stores, such as 23.976. Each cue runs until the next one; the
last runs to its end time, for one frame without one, or to `hold_until_frame`.
Cues that collapse to no frame are dropped and listed. Unmapped shapes return
`invalid_params` with `unmapped_shapes` unless `use_drawing` names a fallback,
missing drawings return `missing_drawing`, and a changed or closed linked source
returns `stale_analysis`. An application writes at most 100,000 frames, and the
result reports the range, runs and their digest.

To inspect an image, start `render.preview`, poll `job.status` until completed,
read `job.result`, and pass an artifact ID to `artifact.read`. The final call
returns MCP image content and source metadata. Inline PNG images are limited to
4 MiB and 2048 pixels per dimension. Job failure and cancellation are reported
explicitly; an artifact is available only after successful writer finalization.

Every job tool works on any job kind. `render.preview`, `render.submit` and
`assistance.neural.preview` return the job record; `job.status` returns it again,
`job.list` lists the worker's retained jobs newest first (filter by `document_id`,
`kind` and `state` `active`, `terminal` or `any`; `offset` and `limit` up to 64),
`job.cancel` requests cancellation, and `job.result` returns `{job, artifacts}` for a
completed render job. A record carries `job_id`, `kind`, `lane` (`render` or
`analysis`), `executor` (`worker` or `scene_host`), `status` and `terminal`, the
owning `worker` (`instance_id` and build identity), `plugin_policy`
(`builtin_only`), `limits`, `inputs` (the request as admitted, the snapshot
package's manifest SHA-256, and a read-only document's source fingerprint),
`progress`, `cancellation` (`requested`, `requested_ms`, `effective`),
`termination` (`mode` and `forced_processes`), `failure` (`null` or `{code, phase,
message}`), `stalled`, `timing` in milliseconds since the worker started, and
`outputs`, plus the render settings and the `effective` output that `render.preflight`
reports. Statuses are `preparing`, `running`,
`cancellation_requested` and `finalizing`, then terminal `completed`, `failed`,
`canceled` or `deadline_exceeded`. A stopped job's termination `mode` is `forced`
when its encoder, movie probe, Neural helper or scene host was killed, and
`forced_processes` names which; otherwise it is `cooperative`. A job whose writers
are still open 30 seconds after a stop request reports `stalled: true` and stays
non-terminal while native work may still run; `job.status` and `job.cancel` keep
answering, and a new worker is the recovery. `job.status`, and `job.cancel` for a
job in a read-only document's scene host, answer without waiting for the process's
main thread: the record is as fresh as the job's state when that thread last went
idle or answered a request, and a cancellation reaches the render threads at once.
When a read-only document's scene host ends, its running jobs read `failed` with
failure `code` `scene_host_failed` and a forced `scene_host`. A render that finds no
usable OpenGL context in the worker fails with failure `code` `gl_unavailable`. Each
lane runs one job at a time: a second
render while one runs is refused with `render_busy` and creates nothing, and a
worker retains at most 32 jobs per lane (`render_limit` for renders). A job id
names the worker that issued it; an id a worker does not hold is refused with
`unknown_job` and `details.reason` `other_worker`, `expired` (its read-only
document was discarded) or `not_issued`. An exact retry of a job-starting call
returns the same job, in a later worker too (from the journal, with a `receipt`),
and starts nothing. A job of an earlier worker that the journal records is answered
by `job.status` and `job.list` with `observed_by: "journal"`: its last recorded
state, or `unknown` with `worker_loss {cause: "worker_exit_unobserved",
outputs_present}` when its worker died first; `job.result` for it is
`artifacts_unavailable` or `result_unavailable` with `details.reason`
`previous_worker`.

A render job of an editable draft renders from a private snapshot of the draft's scene and
artwork. The snapshot exists while the job runs, even if the draft is discarded meanwhile.
When the job ends, whatever its outcome, the scene and artwork are removed; only
`snapshot/manifest.json`, whose SHA-256 the record names, stays with the artifacts that
`job.result` serves until the worker ends. A job that failed or was canceled keeps nothing. A
later worker removes everything a dead worker left.

`vector.inspect` returns each stroke's art `role` (`line_art`, `colour_art`, `underlay`
or `overlay`, derived with the editor's rule: a stroke whose control points all have
thickness 0 is Colour Art, and Line Art below the first or above the last Colour Art
stroke is Underlay or Overlay only when Line Art lies between them), `zero_thickness`,
`opacity_per_mille` and `opacity_knots`, and a `summary` of the whole drawing on every
page: `style_usage` (strokes per style, `defined` false for a style the palette lacks),
`roles`, `colour_range` and `regions: "not_computed"`; it never builds regions. An optional
`expected_revision` that is stale returns `stale_revision`. `vector.select` adds a `lasso`
query (3 to 256 points; the closed polygon is refused with `invalid_params` and
`details.reason` `self_intersecting` or `degenerate`), a `roles` filter and a
`selection_id` handle with the `rule` applied; `contained` follows the editor's freehand
selection, so it compares each stroke's thick bounding box. A handle is valid until the
document's revision changes or 256 newer selections exist (`stale_selection` with
`details.reason` `revision_changed`, `evicted` or `other_drawing`; `unknown_selection`
for an id this document never issued). `vector.opacity.set` and
`vector.opacity.profile.set` take `selection_id` instead of `stroke_indices`, never both.
`drawing.analysis.submit` starts a `drawing_analysis` job on the analysis lane: `check`
`style_usage` (strokes, regions and, on Toonz raster levels, ink and paint pixels per
style), `regions` or `roles`. The record's `analysis` reports the `estimate`, the running
`cost`, the `budget` and `complete` with its `reasons`; `job.result` pages the rows (up
to 256 per page, 65,536 per job) with `next_offset` null on the last page. Regions are
built on detached clones, copied and built on a worker thread, so the document is never
changed and the application thread keeps answering: its slices (one drawing's style count,
a batch of at most two million raster pixels, or some milliseconds of the size estimate)
stay under 50 ms apart from image loading. Submit estimates at most 32 drawings; the job
estimates the rest first and ends `failed` with `resource_limit` if the whole estimate is
over budget. A cancellation ends the job at once and a rebuild in flight is abandoned; the
worker does not wait for it at exit. A write
to the document ends the job `failed` with `revision_changed`; `job.cancel` ends it
`canceled` with the rows gathered so far. A second analysis while one runs is refused with
`analysis_busy`. A read-only document refuses `drawing.analysis.submit` with
`read_only_document` and `details.reason` `not_qualified`.

`scene.check.submit` starts one scene check as an analysis job (`kind` `scene_check`,
`lane` `analysis`). `check: "unused_styles"` lists the palette styles that no drawing of
the used vector and Toonz raster levels uses (optionally only for `level_ids`); the first
two styles of the first page and style 0 are never listed, and vector regions are built
on a copy of each drawing. `check: "inventory"` lists every row of `scene.dependencies`,
each with the `sheet_path` of the first sheet that exposes its level, up to 65,536 rows.
`check: "compatibility"` lists what a legacy OpenToonz reader drops, may refuse or may not
support: one row per finding of the native compatibility report, with a `code` naming
which of its 19 rules fired, a `certainty` (`confirmed_loss`, `refusal_risk` or
`unknown_support`), `severity`, `message`, `sheet_path`, `level_id`, `column_index` and
`frame`. It loads every vector drawing, so `scene.validate` does not report it.
`job.result` pages the rows (up to 256 per page) with a `summary`, `complete` and
`reasons`. The work runs on the worker's main thread in steps that stop once 20 ms have
passed, after the drawing or level in progress; `job.cancel` ends a check at once and
keeps its rows with `complete: false` and `reasons: ["canceled"]`, and a check that
reaches `deadline_ms` (default 300,000) keeps them with `["deadline"]`. A write to the
document fails a running check with `revision_changed`; a read-only document's check runs
in its scene host and fails with `source_changed` when a pinned source file changed. One
analysis job runs at a time per worker and its hosts (`analysis_busy`), and a worker
retains 32 (`resource_limit`, `details.limit: retained_analysis_jobs`).

`render.submit` accepts `format: "png"` (the default) or `format: "mp4"` when the
configured media tools and native movie writer are available. MP4 requires even
dimensions up to 2048 pixels, `step: 1`, and at most 120 frames within the render
storage budget. Its output has no audio or alpha; transparent areas use a black
matte. `job.result` returns a movie's `workspace_relative_path`, SHA-256,
size, dimensions, timing, and observed codec metadata after verification. Movie
bytes stay in that workspace file; `artifact.read` remains PNG-only and rejects
movie IDs with `unsupported_artifact_kind`.

`runtime.inspect` reports what this worker can actually run: its identity (the
`instance_id` and build that job records carry) and platform (operating system and version, the machine's and the build's CPU
architecture, whether the process runs translated, Qt versions), graphics readiness, the granted media tools, native and admitted
formats, the built-in-only plugin policy, the private empty stuff folder, fonts and
toolsets. Each capability has `compiled`, `installed`, `configured`, `loaded`,
`verified` and `usable` states, an `activation` that names what enables it
(`--media-tools` for MP4, `toolsets.enable` for a disabled toolset) and short
`reasons`. Without `probes` it starts no process and never-run probes report
`verified: "not_run"`. `probes: ["graphics"]` runs a helper process that builds the
offscreen OpenGL context renders use, clears and reads back a framebuffer, and
reports `GL_VENDOR`, `GL_RENDERER`, `GL_VERSION` and a `renderer_class`;
`probes: ["media"]` hashes the granted `ffmpeg` and `ffprobe`, reads their version
lines, the mp4 muxer's default video codec and whether an encoder for it is
listed. Only the files in the `--media-tools` directory ever run; `PATH` is never
searched. Probe results are cached for the worker's life, except a graphics probe
that does not finish within 20 seconds: that is no verdict, so graphics stays
`verified: "not_run"` with reason `probe_timeout`, the probe's result is `timeout`, and
the next render, preflight or probe request tries again. Native format lists are
not admissions: `formats.admitted` names what each tool accepts.

Every render requires usable graphics. `render.preflight` takes the arguments of
`render.preview` (`kind: "preview"`, `frame`) or `render.submit`
(`kind: "sequence"`, `start_frame`, `end_frame`, `step`, `format`), plus
`expected_revision`, and runs all 15 checks the job tools apply, in order:
`frame_range`, `dimensions`, `scene_numbering`, `fx_graph`, `fx_extent`,
`graphics`, `writer`, `media_tools`, `encoder`, `dependencies`, `storage`, `lane`,
`retention`, `render_admission` and `source_pins`. Every check runs and reports
its status, code and details, so one call lists every problem; `effective`
describes the output (size, preview fit scale, frames, rate, profile, 8-bit
nonlinear color with gamma 2.2, alpha, no audio, camera resolution, size and DPI,
and a full-camera crop). `render.preview`, `render.submit` and
`assistance.neural.preview` refuse with the first failing check's code and
`details.check` before any job, directory or snapshot exists; an `end_frame`
before `start_frame` is `invalid_params` with `details.reason:
"end_before_start"`. Unavailable graphics, writer, media tools and encoder are
`render_unavailable` with `details.cause` `gl_unavailable`,
`png_writer_unavailable`, `mp4_writer_unregistered`,
`media_tools_not_configured`, `media_tools_changed` or `codec_unavailable`. A
granted media tool replaced after its first probe refuses MP4 with
`media_tools_changed`. A sound source whose private copy is missing or changed
fails `dependencies` with `missing_dependency`. Job records carry the same
`effective` object. `render.capabilities` runs the graphics and media probes once,
reports `graphics` and the mp4 `encoder`, `encoder_available` and
`muxer_default_video_codec`, lists no profile when graphics is unusable and lists
`opaque8-mp4` only when the media tools are usable with an encoder.

Call `job.cancel` to request job cancellation, then poll `job.status` until
the job reaches a terminal status. Cancelling an MCP request never cancels native
work: the call is abandoned, and a job it started keeps running and appears in
`job.list` with `origin_request_cancelled: true`, so it can be found and cancelled
with `job.cancel`. Treat an abandoned write as having an unknown outcome and inspect
state before retrying; completed changes are retained.

The adapter records each job it sees, at most 256 per session, evicting the oldest
finished one first. After the worker is lost, `job.status` and `job.list` answer
from that record: jobs last seen running read `unknown`, terminal, with
`worker_loss` `{cause, adapter_killed_worker, exit_code, signal}` (cause
`worker_exit`, `request_timeout`, `client_disconnect` or `protocol_failure`) and,
when the adapter killed the worker, a forced `worker` termination, and
`worker_loss.reconcile_with: "operation.status"`; `job.list` rows say
`observed_by: "adapter_ledger"`. Finished jobs keep their status. Every other
tool returns `WORKER_UNAVAILABLE`, whose details list the writes that were sent and
not answered as `pending_operations [{tool, operation_id}]`.

The worker allows at most 16 pending calls, 256 KiB per request, and 8 MiB per
response. MCP responses are limited to 9 MiB including envelope headroom. Metadata above
64 KiB uses a brief text summary and remains complete in `structuredContent`;
image content retains its original bytes. A response timeout after 120 seconds,
malformed output, or worker exit closes the worker and fails pending calls.
A lost response can have an unknown outcome: do not replay writes in that session;
start a new session on the same workspace and call `operation.status` for each
pending `operation_id`.
Diagnostics go to stderr; stdout is reserved for MCP. No network listener is used.

Each MCP connection gets one native worker, started when the client first lists or
calls tools rather than during protocol negotiation. If it cannot start, that call
and every later one that needs it fail with `WORKER_UNAVAILABLE`, and the reason is
written to stderr. An input line that is not JSON is answered with JSON-RPC error
`-32700` and an invalid request with `-32600`, both without an `id` unless the
request carries a valid one; invalid notifications and responses get no reply. A
line over 10 MiB closes the connection without a reply. The adapter declares no
tasks capability, so a tool call carrying a `task` field is processed normally and
the field is ignored.

On POSIX systems, every adapter-launched worker uses a private process-group
supervisor. The native worker verifies its parent socket credentials and a bounded
startup handshake. A separate native guardian terminates its current group when
that channel disappears or receives unexpected data, including after supervisor
loss. When the MCP connection closes, the worker's input ends, so it cancels its
renders and removes their partial output as when run directly. A worker whose
input has ended exits within 3 seconds: it cancels render jobs and closes their
writers (unfinished output removed, finished output kept), ends its scene hosts
together and clears its image cache, each within its own budget, and a request
still running when input ends gets only what remains of those 3 seconds. Past the
deadline it ends itself with exit code 3, leaving what a killed worker leaves,
which the next worker in that workspace sweeps away. Group cleanup follows if it
is still running four seconds after its input ended. Timeouts trigger group
cleanup at once. Windows startup requires an owned kill-on-close Job Object before
accepting requests. Raw POSIX workers remain usable for existing operations but
have no verified helper containment.

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
Live attachment excludes `--workspace`, `--input-root`, `--media-tools`,
`--shared-root`, `--shared-write`, `--toolsets`, `--grants` and `--domains`; the
editor's own `inspect` or `edit` mode is the live grant.

The editor and the connector must be the same build. The session file, the
authentication and the editor's hello carry the bridge protocol
(`groundzero-live-v2`) and a build identity: `application`, a `version` label,
and `build_digest`, which identifies the native code that runs. It is the
SHA-256 of a manifest with one line per file, `<name>\t<sha256>\n`: first
`executable` and the SHA-256 of the main executable, then, in name order, each
library this project builds (`colorfx`, `image`, `sound`, `tfarm`, `tnzbase`,
`tnzcore`, `tnzext`, `tnzstdfx`, `tnztools`, `toonzlib`, `toonzqt`), read from
where the dynamic loader loaded it. A project library that is not loaded is an
error. Third-party and system libraries are not part of it. `Groundzero
--automation-build-manifest` prints the manifest as one JSON line,
`{"build_digest", "files": [{"name", "path", "sha256"}]}`, so a runner can
recompute the digest over the same files. The editor computes the identity on a
background thread when live automation is configured; the connector computes it
before it reads the session file, about 0.4 s for the 61 MB macOS build. The
connector compares the session file before it connects, so a mismatched pair never sends
the token. A mismatch fails attachment with `LIVE_INCOMPATIBLE`; its details
name the `cause` (`protocol_mismatch` or `build_mismatch`) and the `connector`
and `editor` identities. Start the editor and the adapter from one build. Any
other failure before the hello is `LIVE_UNAVAILABLE` with the connector's refusal
code as `cause`. `live.status` and `live.capabilities` also report `protocol`
and `build`.

Both permission modes advertise exactly `live.status`, `live.inspect`,
`live.stage.set_key_value`, `live.fx.set_scalar_value`, `live.receipt`, and
`live.capabilities`. Inspect mode rejects writes with `permission_denied`.
Edit mode can change an existing camera/pegbar x, y, or rotation key, or the
qualified native Blur scalar. A changed value creates one native undo entry;
same-value edits preserve history. Use the returned schemas and editable-target
fingerprints rather than detached revision fields. Fingerprints cover qualified
target dependencies; pixels, palette content, and asset bytes are excluded.

Fingerprints also follow the native undo history, so an edit prepared before an
undo or redo is refused with `stale_target` even when the undo restored the
earlier values. The refusal's details give a `reason` and the
`current_fingerprint`: `content_changed` (the qualified metadata differs from
what the fingerprint covered, also used for a fingerprint the editor did not
issue), `history_changed` (the metadata is the same but the history moved), or
`target_replaced` (the target's native parameter was replaced). Check the
current value with `live.inspect`, then retry with `current_fingerprint` under a
new operation UUID.

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

## Tool index

<!-- tool-index:begin (generated by ci-scripts/mcp_census.py) -->
Every tool a session can advertise. Worker tools belong to the toolset named here;
the adapter and the live editor bridge add the rest.

- `core` toolset (121):
  `animation.channels`, `animation.copy_curve`, `animation.delete_keys`,
  `animation.duplicate_keys`, `animation.insert_breakdown`, `animation.keys`,
  `animation.move_keys`, `animation.retime_curve`, `animation.sample`, `animation.set_cycle`,
  `animation.set_keys`, `artifact.read`, `assistance.inspect`, `assistance.neural.preview`,
  `automation.capabilities`, `automation.list_documents`, `camera.inspect`, `camera.update`,
  `cast.list`, `cast.usage`, `column.cells`, `column.create`, `column.delete`, `column.expose`,
  `column.update`, `drawing.analysis.submit`, `drawing.clone`, `drawing.create`,
  `drawing.inspect`, `drawing.remove`, `drawing.renumber`, `fx.catalog`, `fx.connect`,
  `fx.create`, `fx.delete`, `fx.disconnect`, `fx.duplicate`, `fx.enabled.set`, `fx.graph`,
  `fx.macro.create`, `fx.macro.expand`, `fx.output.select`, `fx.parameter.get`,
  `fx.parameter.set`, `fx.parameter.set_key`, `fx.preset.apply`, `fx.preset.save`, `fx.rename`,
  `fx.terminal.set`, `job.cancel`, `job.list`, `job.result`, `job.status`, `journal.status`,
  `level.create`, `level.import`, `level.inspect`, `motion_path.create`, `motion_path.delete`,
  `motion_path.duplicate`, `motion_path.inspect`, `motion_path.list`, `motion_path.rename`,
  `motion_path.update`, `object.create`, `object.parent`, `object.path.attach`,
  `object.path.detach`, `operation.list`, `operation.status`, `palette.clone_style`,
  `palette.create_style`, `palette.inspect`, `palette.page.create`, `palette.page.move`,
  `palette.page.remove`, `palette.page.rename`, `palette.pages`, `palette.rename_style`,
  `palette.style.move`, `palette.update_style`, `project.inspect`, `project.list`,
  `project.scenes`, `raster.export_png`, `raster.import_png`, `raster.patch`,
  `raster.read_pixels`, `render.capabilities`, `render.preflight`, `render.preview`,
  `render.submit`, `runtime.inspect`, `scene.check.submit`, `scene.create`,
  `scene.dependencies`, `scene.discard`, `scene.inventory`, `scene.open`,
  `scene.open_read_only`, `scene.package_info`, `scene.save_copy`, `scene.settings`,
  `scene.source_status`, `scene.summary`, `scene.validate`, `text.fonts`, `tool.describe`,
  `vector.edit`, `vector.fill`, `vector.group.create`, `vector.group.remove`, `vector.inspect`,
  `vector.opacity.inspect`, `vector.opacity.profile.set`, `vector.opacity.set`,
  `vector.order.move`, `vector.select`, `vector.structure`, `vector.text.create`,
  `vector.transform`
- `audio` toolset (8):
  `audio.clip.place`, `audio.clip.remove`, `audio.clip.update`, `audio.column.create`,
  `audio.column.set_volume`, `audio.import`, `audio.inspect`, `audio.mixdown`
- `fx-finishing` toolset (1):
  `fx.recipe.list`
- `lipsync` toolset (3):
  `lipsync.apply`, `lipsync.import`, `lipsync.inspect`
- `rigging` toolset (18):
  `hook.delete`, `hook.inspect`, `hook.set`, `object.delete`, `object.duplicate`,
  `object.group`, `object.group.rename`, `object.groups`, `object.handles.set`,
  `object.pivot.set`, `object.rename`, `object.ungroup`, `offset_layer.create`,
  `offset_layer.delete`, `offset_layer.update`, `rig.dependents`, `rig.evaluate`, `rig.inspect`
- `shared-library` toolset (7):
  `library.inspect`, `library.list`, `library.thumbnail`, `shared.roots`, `studio_palette.find`,
  `studio_palette.inspect`, `studio_palette.list`
- `sheet` toolset (22):
  `camera.create`, `camera.duplicate`, `camera.list`, `camera.remove`, `camera.select`,
  `cells.clear`, `cells.copy`, `cells.insert`, `cells.remove`, `cells.replace`, `cells.retime`,
  `cells.set`, `column.duplicate`, `column.kinds`, `column.move`, `level.duplicate`,
  `level.remove`, `level.rename`, `level.settings.set`, `scene.guides.set`, `scene.output.set`,
  `scene.properties.inspect`
- `storyboard` toolset (4):
  `storyboard.panel.create`, `storyboard.panel.delete`, `storyboard.panel.update`,
  `storyboard.panels`
- Adapter:
  `toolsets.disable`, `toolsets.enable`, `toolsets.list`
- Live editor bridge:
  `live.capabilities`, `live.fx.set_scalar_value`, `live.inspect`, `live.receipt`,
  `live.stage.set_key_value`, `live.status`
<!-- tool-index:end -->

## Repository checks

Repository checks run in CI: `pnpm lint`, `pnpm format:check`, `pnpm typecheck`,
`pnpm test`, and `pnpm build`. `pnpm test:native` requires a built native executable
in `GROUNDZERO_EXECUTABLE` and a canonical media directory in
`GROUNDZERO_MEDIA_TOOLS`. It exercises legacy and MCP 2026-07-28 stdio connections,
document creation, revision rejection, decoded color-card preview pixels, and MP4
metadata and file hashes. The PNG cases launch without a media grant. These tests
establish SDK interoperability on the CI platform; host-specific display behavior
still depends on the MCP application.

The client configurations under "Connect a client" are tested from this file: the
unit tests read the marked snippets, start the server exactly as written with
example paths replaced, list its tools and call a read tool, with no client
installed. `pnpm test:native` also checks the full catalog against the limits both
clients document: qualified tool names stay distinct and within 64 characters, and
each input schema is a plain object at its root, with no `oneOf`, `anyOf`, `allOf`,
`not` or `$ref` there. The adapter refuses a native catalog that breaks this rule.

To try the installed clients themselves, run `node fixtures/real-clients.mjs
--executable /opt/groundzero/bin/Groundzero` after `pnpm build`. It is a local
opt-in check and is never run by CI. For Claude Code (`--claude FILE`) and Codex
(`--codex FILE`), each found on `PATH` unless named, it writes the README's
configuration into a throwaway home, so your own configuration is untouched, and
starts the client with it. Claude Code must report the server as connected. Codex
must list its tools through its app server; `--call-tool` also calls
`automation.list_documents`. Neither client is asked for a model response, so Claude
Code's tool list is checked by hand with `/mcp`.
