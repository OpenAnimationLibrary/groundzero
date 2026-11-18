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

Shared studio palettes and the Library are reachable only through explicit
`--shared-root ROLE=DIR` arguments, at most one per role: `studio_palettes`,
`project_palettes` or `library`. Each directory must be canonical, must not be a
symlink, and must not overlap the workspace or another shared root; input roots
may overlap. Shared roots are read-only. `--shared-write ROLE` records a write
grant for a configured role; the current tools only read. The worker never uses
the editor's own STUDIOPALETTE or LIBRARY folders. Without a grant, the tools
stay listed and return `root_unavailable`.

Native tools are grouped into toolsets, so a session only carries the tool
definitions it needs. The `core` toolset is always enabled. Add others at launch
with `--toolsets audio,rigging`, or use `--toolsets all`. Unknown names stop the
adapter at startup. During a session:

- `toolsets.list` reports each toolset, what it covers, its tool count and
  whether it is enabled.
- `toolsets.enable` and `toolsets.disable` take `{"names": [...]}`. They change
  the advertised tool list and send `notifications/tools/list_changed`. Both are
  idempotent, and `core` cannot be disabled.
- Calling a tool whose toolset is disabled returns `TOOLSET_DISABLED`, with the
  toolset in `details.toolset`. Nothing reaches the worker.
- A session exposes at most 256 native tools. A request that would exceed this
  returns `TOOLSET_LIMIT` and leaves the enabled toolsets unchanged.

The adapter requires native automation protocol version 2. An older or newer
worker is refused with `INCOMPATIBLE_WORKER`.

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

`fx.recipe.list`, in the `fx-finishing` toolset, returns static recipes: light wrap,
backlit rim light, exposure grade, cross dissolve and iris wipe. The effects they
use are ordinary catalog types reached through the `core` FX tools. Each lists its
required effect types and whether they are available, its inputs with defaults, its
steps as ordinary tool calls, named controls and checks. An argument whose whole
value is `${input.<name>}` or `${steps.<id>.<field>}` takes that input's or earlier
result's JSON value. A recipe never runs on the worker: run its steps yourself with
the usual `expected_revision` and `operation_id`, so a recipe that stops halfway
leaves identifiable committed operations.

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
requires `placement_policy: "preserve_local"`: local keys are kept, so the object may
move. It accepts an optional `parent_handle` and `report_frames`, and refuses a hook
handle under a parent that is not a column.

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
`scene.source_status` re-stats every file pinned at open and reports each change as
`modified`, `replaced` (a new file in its place), `missing` or `unreadable`, with
`state: "missing"` when the scene file itself is gone; editable documents return
`unsupported_target`. On volumes with coarse timestamps (HFS+: one second, exFAT: ten
milliseconds) a same-size rewrite in place within one tick is not detected.

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
a level that did not load, or a shipped library texture; a camera without a finite
positive size and DPI also blocks. The render preflight still applies. Every file the
scene reads is stat-pinned at open (size, nanosecond modification time and file id);
a render re-checks the pins before it starts and before it publishes, and returns or
fails with `source_changed` when a source changed. Results report
`snapshot_inputs: "linked-source-stat-pinned"` and the `source_fingerprint`.
`scene.discard` stops the host and removes its private copy and outputs; it is
refused with `render_busy` while one of the document's jobs is running.
`automation.list_documents` reports each document's `access` and `host_state`.

`scene.open` accepts unchanged packages produced by the same running worker.
The worker retains the authoritative manifest privately; editing a public
manifest cannot authorize a changed scene. Other native scenes and packages from
previous worker sessions are rejected with `unregistered_package` and
`details.hint: "scene.open_read_only"`. Saved files remain available for
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
renders and removes their partial output as when run directly; group cleanup
follows if it is still running two seconds later. Timeouts trigger group cleanup
at once. Windows startup requires an owned kill-on-close Job Object before
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
`--shared-root`, and `--shared-write`.

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

- `core` toolset (108):
  `animation.channels`, `animation.copy_curve`, `animation.delete_keys`, `animation.keys`,
  `animation.retime_curve`, `animation.sample`, `animation.set_keys`, `artifact.read`,
  `assistance.inspect`, `assistance.neural.preview`, `automation.capabilities`,
  `automation.list_documents`, `camera.inspect`, `camera.update`, `cast.list`, `cast.usage`,
  `column.cells`, `column.create`, `column.delete`, `column.expose`, `column.update`,
  `drawing.clone`, `drawing.create`, `drawing.inspect`, `drawing.remove`, `drawing.renumber`,
  `fx.catalog`, `fx.connect`, `fx.create`, `fx.delete`, `fx.disconnect`, `fx.duplicate`,
  `fx.enabled.set`, `fx.graph`, `fx.macro.create`, `fx.macro.expand`, `fx.output.select`,
  `fx.parameter.get`, `fx.parameter.set`, `fx.parameter.set_key`, `fx.preset.apply`,
  `fx.preset.save`, `fx.rename`, `fx.terminal.set`, `level.create`, `level.import`,
  `level.inspect`, `motion_path.create`, `motion_path.delete`, `motion_path.duplicate`,
  `motion_path.inspect`, `motion_path.list`, `motion_path.rename`, `motion_path.update`,
  `object.create`, `object.parent`, `object.path.attach`, `object.path.detach`,
  `palette.clone_style`, `palette.create_style`, `palette.inspect`, `palette.page.create`,
  `palette.page.move`, `palette.page.remove`, `palette.page.rename`, `palette.pages`,
  `palette.rename_style`, `palette.style.move`, `palette.update_style`, `project.inspect`,
  `project.list`, `project.scenes`, `raster.export_png`, `raster.import_png`, `raster.patch`,
  `raster.read_pixels`, `render.artifacts`, `render.cancel`, `render.capabilities`,
  `render.preview`, `render.status`, `render.submit`, `scene.create`, `scene.dependencies`,
  `scene.discard`, `scene.inventory`, `scene.open`, `scene.open_read_only`,
  `scene.package_info`, `scene.save_copy`, `scene.settings`, `scene.source_status`,
  `scene.summary`, `scene.validate`, `text.fonts`, `vector.edit`, `vector.fill`,
  `vector.group.create`, `vector.group.remove`, `vector.inspect`, `vector.opacity.inspect`,
  `vector.opacity.profile.set`, `vector.opacity.set`, `vector.order.move`, `vector.select`,
  `vector.structure`, `vector.text.create`, `vector.transform`
- `audio` toolset (8):
  `audio.clip.place`, `audio.clip.remove`, `audio.clip.update`, `audio.column.create`,
  `audio.column.set_volume`, `audio.import`, `audio.inspect`, `audio.mixdown`
- `fx-finishing` toolset (1):
  `fx.recipe.list`
- `lipsync` toolset (3):
  `lipsync.apply`, `lipsync.import`, `lipsync.inspect`
- `rigging` toolset (10):
  `hook.delete`, `hook.inspect`, `hook.set`, `object.handles.set`, `object.pivot.set`,
  `offset_layer.create`, `offset_layer.delete`, `offset_layer.update`, `rig.evaluate`,
  `rig.inspect`
- `shared-library` toolset (7):
  `library.inspect`, `library.list`, `library.thumbnail`, `shared.roots`, `studio_palette.find`,
  `studio_palette.inspect`, `studio_palette.list`
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
