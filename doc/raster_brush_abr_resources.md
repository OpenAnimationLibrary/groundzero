# ABR resource adapter evidence and remaining qualification

This document supplements RCP1; it does not reduce its D profile contract or
claim that modern Photoshop brush behavior is implemented.

The independently written reader uses Adobe's [Photoshop File Formats
Specification, November 2019](https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/),
particularly Descriptor structure, Patterns, and Virtual Memory Array List.
Adobe explicitly distinguishes data format from interpretation. The Patterns
layout documents version 1 resources, version 3 VM arrays, image modes, resource
identity, and channel slots, including two mask slots. It does not specify
Photoshop brush texture equations or the mapping of brush setting descriptors
to these resources. Its compression description is insufficient to qualify all
ABR producer encodings against a real Photoshop corpus.

## Implemented resource layer

- Existing descriptor parsing retains exact key/type/class/unit/value trees;
  unknown types are not guessed. Sample IDs are separate from ordinal tip names.
- Pattern resources retain unique ID, Unicode name, mode, point, bounds, indexed
  palette, and ordered channel metadata. Raw and documented VM-ZIP 8/16-bit channels yield
  decoded sample bytes, preserving both big-endian bytes at depth 16. Other
  depth/compression combinations explicitly remain undecoded. Mask slots are preserved without inventing alpha or coverage math.
- Per-file record, metadata, and decoded-pixel budgets include patterns. Native
  tip allocation reserves memory occupied by decoded pattern channels/palettes.
  ZIP output and live inflater workspace are charged before allocation; the
  stream must consume exactly its input and produce exactly its declared size.
- Duplicate resource identities or malformed pattern sections invalidate the
  entire pattern identity collection, including later sections. Missing or
  ambiguous resource identities must never fall back to list-position binding.
- Import provenance records decoded pattern count, structural completeness, and
  explicitly false source texture semantic support. The native stamp conversion
  does not silently enable a texture because a resource happened to decode.

`abrparser_test.cpp` contains original byte-level structural fixtures covering
raw/ZIP 8/16-bit channels, exact checksums and stream lengths, inflater workspace,
unwritten mask slots, unsupported compression, duplicate identities,
truncation, channel limits, and aggregate allocation limits. These fixtures are
parser tests, not source-application exports or rendering references. Execution
belongs to repository CI; no local build or test is needed for this workstream.

## Remaining work and exact evidence dependency

Modern ABR preset-to-sample/pattern association, per-setting dynamics adapters,
texture/dual-tip equations, and P2711 reference verification remain incomplete.
A live Photoshop Action Descriptor property name alone is insufficient evidence
that an ABR `desc` tree has the same schema, scope, defaults, or active-setting
semantics. No unsupported property name is promoted to a supported mapping.

To qualify those adapters, obtain original producer exports with controlled
single-setting changes and recorded application version, plus matching scripted
stroke references from Photoshop 27.11. The corpus must include multiple presets
sharing resources, reordered resources, generated and sampled brushes, texture
and dual-tip links, each active D control, disabled-control defaults, and masks/
compression variants. Use independently created assets with an explicit fixture
license. Neither a named Kyle pack nor proprietary redistribution is required.
A reference runner is required for behavioral parity claims; public format docs
and synthetic fixtures alone cannot establish the source state equations.

This is an evidence/access dependency, not a claim that independently implementing
compatible behavior requires permission to copy an engine. No source engine code
or proprietary brush asset is copied by this resource reader.

## Compression profile boundary

The specification's VM-array table assigns compression value 1 to ZIP. Its
separate PSD Image Data table instead assigns 1 to PackBits, 2 to ZIP without
prediction, and 3 to ZIP with prediction. These contexts are not interchangeable.
The implemented `adobe-vm-zip1` interpretation uses a bounded zlib-wrapped DEFLATE
stream, with an explicit diagnostic that ABR producer encoding remains unverified.
It never tries PackBits after ZIP fails. Values 2/3 and 1/32-bit channel depths
remain undecoded pending context-specific evidence; no predictor is invented.
The [zlib manual](https://zlib.net/manual.html) defines the stream/error/allocation
interface. Original stored-block fixtures exercise the decoder mechanics only.

An openly licensed serialized-corpus candidate is the author's
[Ende 1.0 release](https://kmalexander.com/2020/11/17/ende-a-17th-18th-century-littoral-edger-for-your-fantasy-maps/),
which identifies ten Photoshop brushes and expressly supplies them under CC0.
It may establish serialized resource fields but cannot establish P2711 rendering
parity or coverage of every compression mode. No pack bytes are included here;
qualification must record the acquired archive hash and observed format before
using it as evidence.

## Observed ABR 6.2 sample binding and static native adapter

The `groundzero-abr6.2-static-1` adapter recognizes descriptor-v16 root class
`null`, its `Brsh` list of `brushPreset` objects, and primary `Brsh` objects of
class `sampledBrush`. `sampledData` TEXT must match exactly one sample Pascal ID.
Missing references and duplicate sample IDs fail closed. Each descriptor creates
its own preset, with an ordinal identity; presets sharing a sample share one
immutable native image. Unreferenced samples retain explicit native-tip fallback.
The combined output is bounded to 256 presets/resources.

This structure was independently inspected in three author-distributed ABR6.2
files pinned to corpus commit `940fc6d4cddbe0a906eb6497a60ec2c7d837ca84`:
[fixture provenance](https://github.com/SethRobinson/Patchy/blob/940fc6d4cddbe0a906eb6497a60ec2c7d837ca84/test-fixtures/abr/NOTICE.txt).
The two original Photoshop 2026 exports contain one sample/preset each; the CC0
collection contains 148 samples/presets. All 150 primary references join uniquely.
This is verified serialized-format evidence for 6.2 only. Versions 6.1 and 7–10
remain decodable without automatically receiving this binding adapter. The exact
Photoshop patch number and source stroke renders were not supplied.

Preset names, diameter (`Dmtr/#Pxl`), angle (`Angl/#Ang`), roundness
(`Rndn/#Prc`), spacing (`Spcn/#Prc`), and static sample flips are transferred into
**native** settings. Required types and units are checked; disabled spacing is
unsupported. Native angle, aspect, interpolation and spacing conventions are
not claimed to reproduce Photoshop. Typed descriptor metadata is retained in
`sourceDescriptorTupleV1`, a compact JSON string containing recursive tuples
`[key,type,name,classId,unit,text,number,integer,bool,children]`; integers use
lossless decimal strings and opaque bytes use hexadecimal. Allocation and final
internal native metadata budgets apply before returning records. Internal
`encode`/`decode` save/reopen retains the tuple; `encodePortable` deliberately
removes it because arbitrary source metadata can contain private alias bytes.

Active dynamics are diagnosed without guessing control-enum values. In
particular an observed `dualBrush`/`colorBurn` is retained as source metadata and
explicitly **not applied**; it is never silently converted to Multiply. Source
semantic support and reference verification remain false. Public regression
fixtures use newly generated pixels, names and numbers following the observed
record structure, not redistributed third-party pack pixels.

All three observed `patt` sections are empty. This acquisition does not establish
pattern compression, pattern binding, dynamic equations, or Photoshop 27.11
rendering parity. Those still require appropriately licensed nonempty resources
and controlled source exports/render traces.


### Conversion classification

`conversionKind` is a bounded provenance enum: `tip-only` for unbound ABR sample
extraction, `static-native` for the 6.2 bound adapter and legacy computed static
conversion. `source-candidate` is reserved for strict source-profile candidates;
this ABR adapter never assigns it. The enum does not establish source parity.
Portable exports preserve these recognized values (and `partial-native`, described
below) only, while continuing
to drop raw descriptors and free-form source diagnostics. Consumers should display
`tip-only` explicitly as **Tip extraction only**, including after portable reopen,
and must not describe these records as a full source brush preset import.

### Active-setting audit and remaining dynamic adapter work

`static-native` now requires that every observed preset field is either applied
or belongs to an explicitly disabled recognized group. Active shape, scatter,
transfer, texture, color, pose, brush-group, dual-brush and unknown settings,
including tool options, produce `partial-native` with a bounded omission report.
Orphan dynamics without an explicit disabled switch are also partial. Unknown
primary-tip fields cannot silently qualify as a complete static conversion.
The full typed descriptor stays available internally; portable exports retain
only the safe classification, not arbitrary source field names or alias bytes.

The declared modern ABR dynamic-adapter outcome remains unfinished. Adobe's
[brush dynamics reference](https://helpx.adobe.com/photoshop/desktop/apply-painting-techniques/fill-objects-selections-layers/brush-dynamics-settings.html)
establishes UI-level jitter, minimum-diameter and transfer concepts, but it does
not specify the ABR `bVTy` integer encoding or tie `szVr`, `opVr` and `prVr` to
controlled UI exports. The inspected author fixtures identify general enabled
categories but provide no per-setting before/after UI/export correspondence.
Numeric values and suggestive field names alone do not establish that mapping.
No control-enum interpretation or random distribution is guessed here.

Needed evidence is precise: original paired exports changing only one shape,
scatter or transfer control; recorded UI values and exact producer build; all
control enum choices (Off, Fade and applicable tablet/direction modes); and
nonzero/minimum/jitter combinations. These pairs can establish a labeled native
conversion without claiming Photoshop renderer parity. Reference stroke traces
would additionally be needed to establish source random distributions, sensor
normalization, fade endpoints and per-dab ordering. No source-code reuse,
paid pack acquisition or runtime emulation is necessary to obtain such evidence.

## Additional original static ABR corpus

The [pinned 64-preset author corpus census](raster_brush_abr_corpus.md)
records asset hashes, license scope and complete serialized-field observations
for two additional ABR 6.2 collections. All tips are sampled and all dynamics
are disabled; both pattern sections are empty. This evidence does not resolve
modern computed-tip, pattern-association, dynamic-control or other-version gaps,
and does not establish source rendering parity. No pack assets are included.
