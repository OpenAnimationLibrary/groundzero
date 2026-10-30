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
