# Native raster brush portability

Regular raster brush Export writes a versioned `.gzbrush` portable package.
It carries native settings and resource identities; it does not certify Krita,
Photoshop or Kyle behavioral parity. TLV and MyPaint presets are unchanged.

## Export and repair

The export dialog inventories every primary tip, secondary tip and texture.
Each resource has a pixel hash, dimensions, color/mask interpretation, an
explicit redistribution declaration and an optional license/attribution note.
Declarations are user-supplied metadata, not a license verification service.
Importing an asset does not imply permission to redistribute it.

The packaging choices are:

- **Manifest only:** settings and resource hashes, without image bytes. This is
  the default if any resource has unknown or restricted redistribution rights.
- **Include resources declared permitted:** embeds only resources explicitly
  declared permitted; any others remain references requiring repair.
- **Private local copy:** explicitly embeds all resources without changing their
  declarations. This choice is a local backup workflow, not a redistribution
  permission claim.

If all resources are declared permitted, the dialog initially selects the
permitted-resource option. Changing declarations affects the exported copy;
editing a declaration is not a claim that the application checked its accuracy.

Importing a package with missing or checksum-damaged embedded resources opens
Relink Brush Resources. Select existing brush packages or PNG copies. Foreign
formats and PNG files are decoded by the existing cancellable import worker.
For PNG files, select the same color, alpha-mask or luminance-mask interpretation
used to create the preset. Exact matching hashes are required; filenames and
similar-looking images never substitute for identity. Selected brush packages
can supply tip sequences, secondary tips and textures together.

Cancel or an incomplete repair installs nothing. A fully repaired package goes
through worker validation before the existing atomic collection installation.
The installed native preset contains its resolved resources, so moving or
removing the repair source does not break subsequent drawing, undo or reopening.
Byte-checksum failures open repair directly. If worker validation rejects a
complete package (including a pixel-hash or image decoding failure), Retry
discards embedded copies and relinks against the unchanged declared identities.
A forged identity cannot be replaced with a different hash. Malformed schemas
and conflicting metadata are rejected; invalid brush settings remain invalid
after resource replacement.

## Format and identity

The envelope has `format: groundzero-raster-brush-portable`, `version: 1`, a
native version-1 `package`, and a `resources` object keyed by native PNG-byte
asset IDs. The manifest describes every referenced resource exactly once with
`hash`, `width`, `height`, `color`, `redistribution` and `license` fields. The
native package's `assets` object contains only the chosen embedded subset.

The stable identity is SHA-256 over big-endian 32-bit width and height followed
by straight RGBA8 pixels and the ASCII suffix `color` or `mask`. Mask RGB bytes
are zero. Premultiplied native color is converted using the same quantization
as native PNG export. Hash repair therefore survives different lossless PNG
encodings. Relinking rewrites all native byte-ID references and preserves the
manifest's declarations; a donor package cannot grant new permissions.

Optional `resourceRights` metadata survives native save and private worker
transport. Older native files default to unknown declarations. Portable export
removes arbitrary provenance and all original free-form diagnostics, retains bounded
technical source hashes/container versions, and clears source-verification
claims. License/attribution notes containing local-path forms (including UNC and extended
Windows paths) are omitted; ordinary license names and web links are preserved.
The brush name and other deliberately authored preset text remain user content.

Inspection parses bounded JSON and checks byte identities without invoking an
image decoder. Native decoding checks every embedded resource's actual pixel
identity, dimensions and color interpretation. Limits remain 64 MiB encoded,
512 manifest resources, 8192 pixels per dimension, 2 million pixels per
resource and 256 MiB aggregate decoded pixel storage. Existing stricter tip
sequence limits still apply. A missing resource never becomes a selectable
preset. Inspection and repair do not grant a source-reference-verified status.

## Qualification

`rasterbrushpreset_test` covers all packaging choices, declaration retention,
private metadata filtering, missing/wrong/corrupt resources, alternate PNG byte
IDs, tampered hashes, geometry limits, native/worker round trips and unresolved
save rejection. Original manifest and embedded packages seed the parser fuzzer.

The real-application raster brush integration fixture exports the combined
native resource preset as a manifest, rejects it without changing the raster
or undo stack, decodes recipient copies in the worker, repairs by hash and
continues through actual tool drawing, input dynamics, undo/redo, cancellation
and raster save/reopen. Wet and dry cases share this lifecycle. These fixtures
exercise native portability; foreign-engine reference fidelity remains a
separate acceptance requirement. CI results, rather than the existence of the
fixtures, determine qualification.

### Native random-stream policy

Native presets expose **Vary random seed for each stroke** in the editor.
It is off by default, including when loading older packages without the field:
the saved 64-bit seed then repeats the same random stream for identical input.
When enabled, the application mixes that base seed with a process-wide stroke
sequence once per logical stroke. Replicated tracks share the resolved seed;
new strokes consume new streams even after cancellation or undo. This is a
session variation policy, not a promise that a new session resumes the sequence.

The renderer always consumes an explicit seed and remains deterministic.
Undo/redo restores stored pixels without rerendering or allocating a new stream.
Interactive scratchpad strokes use the same policy; the automatic sample uses
the saved seed so editing comparisons remain repeatable. Portable packages retain
the policy and base seed, not transient session counters. Source profiles reject
this native-only policy until a separate source input contract is qualified.
