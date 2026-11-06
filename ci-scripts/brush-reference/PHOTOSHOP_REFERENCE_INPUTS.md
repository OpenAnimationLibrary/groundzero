# Controlled Photoshop 27.11 reference inputs

This is an external operator recipe and a bounded ingestion contract. No
Photoshop execution or original producer export is supplied by implementing it.
The full D target in `doc/raster_brush_compatibility.md` remains required. These
inputs can establish UI-to-serialized-data correspondence; matching source
strokes, device evidence and the unchanged acceptance gates remain separate.

`photoshop_export_contract.py` consumes a flat supplied directory and emits an
`inputs-validated-not-qualified` receipt. It reads files only: no Photoshop
launch, scripting API, image decoding, descriptor enum interpretation, asset
installation, upload or redistribution takes place. Human UI/ownership claims
are declared evidence, not an attestation by this helper.

## Keep input containers separate from exported containers

ABR major/subversion is not a Photoshop application version. An original v1/v2
or 6/1, 7-10/1-2 fixture may be independently authored and then interpreted by
P2711. Photoshop 27.11 need not offer an exporter for every historical format.
A modern 6/2 re-export does not replace the original input's bytes, header or
render association. Never edit just a version header to claim a producer or
source-interpretation result.

Each case has separate `inputAbr` and `abr` (produced export) roles, separate
expected headers, and separate input/export node identities and ordinals.
`inputProvenance` distinguishes an independently authored structural fixture
(pinned generator revision, no invented producer) from an original producer
file (recorded producer label, no invented generator).

The interpretation state is `pending`, `loaded`, or `rejected`. Pending inputs
have no source UI snapshot, export or render claim. A rejected input retains
its UI rejection evidence and cannot claim an export/render. A loaded input
records its actual readback; an optional external render association binds the
original input SHA-256, exact executable SHA-256 and reference-manifest hash.
That association is retained but is not independently validated by this
input-only helper. The receipt reports input presence, export presence, loaded
interpretations, rejections and render associations separately for all 12
required container variants.

## Authorized source setup

1. Use an authorized Photoshop desktop 27.11 installation in a disposable
   reference workspace. Freeze application updates during one capture set and
   keep unrelated extensions/presets out of the reference operation.
2. Confirm the version in About Photoshop: the Photoshop menu on macOS or Help
   on Windows. Record the exact reported numeric patch version and build text.
   Adobe documents the [version menus](https://helpx.adobe.com/photoshop/desktop/get-started/technical-requirements-installation/photoshop-supported-versions.html)
   and identifies [September 2026 version 27.11](https://helpx.adobe.com/photoshop/desktop/whats-new/photoshop-on-desktop-release-notes.html).
3. Inspect Help > System Info privately for product/build identification. Retain
   only the relevant product/build readback, not the full report: it can include
   user paths, hardware identifiers and environment information. Adobe's
   [System Info guide](https://helpx.adobe.com/photoshop/kb/system-info-troubleshoot-photoshop.html)
   describes that UI. Keep this identity evidence operator-private and put only
   its SHA-256 in `identityEvidenceSha256`; do not add the report, screenshot,
   private filename or source path to the supplied inventory.
4. Identify the actual running application's executable and fingerprint that
   regular local file before and after capture. Confirm both fingerprints match
   the recorded producer identity. Do not include the executable, process ID,
   installation path, account details or full system report in the input pack.
5. Use English UI for these acquisition labels. Prepare the reference document
   as 2,048x2,048 RGB 8-bit encoded sRGB, zoom 100%, view rotation 0 and smoothing 0. Keep
   exact selected foreground/background colors in the snapshots. Asset-creation
   documents may be different; document that source asset separately.

The helper's prospective operator commands are:

```text
python ci-scripts/brush-reference/photoshop_export_contract.py describe
python ci-scripts/brush-reference/photoshop_export_contract.py fingerprint-executable <operator-selected-executable>
python ci-scripts/brush-reference/photoshop_export_contract.py ingest <controlled-input-directory>
```

Fingerprinting reads bounded chunks and emits only SHA-256/size. It does not
execute the selected file or prove that it is the process image; the operator
must make that connection. These commands and the authored regression source
remain unexecuted while implementation/final validation is deferred.

## Create original assets, not a downloaded brush corpus

Use original asymmetric grayscale tip images, an original generated ellipse
and an original nonuniform pattern. For example, a small notched mask with
several grayscale levels and a separately drawn unequal-stripe pattern make
flips, resource joins and pattern transforms distinguishable. Keep source PNGs
and their exact hashes. Prefer simple opaque grayscale source masks; retain
bit depth when adding separate 8/16-bit structural/producer cases.

Each asset declares `operatorOriginal`, creation description, and either
CC0-1.0/permitted or an original private-evidence license with redistribution
explicitly forbidden. Do not infer rights from a parser license, a brush-pack
name or a subscription. No paid pack, Kyle asset or third-party brush pixel is
needed. Generated-ellipse assets store the operator's original geometry
parameters rather than borrowing a tip image.

Never supply original private screenshots or a full System Info report.
For control readbacks, prepare separate technical crops that show only the
relevant control/value; exclude accounts, private paths and unrelated work.
The ingestion format accepts only PNG image structure and bounded binary
color/geometry chunks (IHDR, PLTE, IDAT, IEND, tRNS, cHRM, gAMA, sRGB, sBIT,
bKGD, pHYs). Text, EXIF, embedded ICC profiles, animation, unknown critical
chunks and private ancillary chunks reject; profiles may contain private text.
Use an encoded-sRGB source and prepare its metadata before defining the
reference asset. Visible/private image content still needs human review, so
subsequent byte hashes identify the actual supplied source file.

## Capture a baseline and single-setting pairs

Adobe documents [custom brush creation/settings and export](https://helpx.adobe.com/ca/photoshop/desktop/apply-painting-techniques/brushes-presets/create-brush-set-painting-options.html)
and [saving a new preset](https://helpx.adobe.com/sg/photoshop/desktop/apply-painting-techniques/brushes-presets/create-a-new-preset-brush.html).
Use the documented UI, without assuming an undocumented export API:

1. Open the Brush Settings panel and select the original tip. Set a deliberate
   baseline. Inventory every declared control and every additional visible
   control; explicitly record disabled groups and greyed values.
2. Save a new preset with a stable generic name. Record the actual size/color/
   tool-setting save options; these options must remain fixed within a pair.
   Opacity, flow and tool options must not silently disappear from the capture.
3. Export the selected preset(s) from the Brushes panel using its documented
   export command. Keep the original .abr bytes and capture the UI readback.
4. Change exactly one UI setting. Preserve the name, selected resources, other
   presets, export selection and every other observable value. Save/export the
   changed preset, then capture its effective UI state, not just intended input.
5. Link the changed case to its already captured parent. The helper compares
   all selected presets' snapshots and requires exactly one authored value
   difference. Do not rename a preset or change an export option simultaneously.

A group toggle can grey or reveal dependent controls. Declare those actual
`available`/`enabled`/choice-list changes exactly. If a previously hidden Fade
step value becomes visible, record the observed value as a derived availability
change; never invent its earlier/default value. Two observable knob-value
changes reject the pair. A source value that rounds or an export whose bytes do
not change is a retained observation, not a reason to modify the recorded data.

The dotted `CONTROL_SPECS` keys are acquisition labels, not Photoshop scripting
properties or ABR descriptor keys. `available=false` means unobserved/not exposed,
with value null; it must not be read as a semantic zero or an off setting.
`enabled` records UI interactivity; an enabled checkbox with value false still
represents an off feature. Unrepresented controls additionally declare actual
active state, with evidence. If rate/origin/seed input is not exposed, record
that gap rather than inventing a field or API. List choices literally as shown
by the source. Adobe's [dynamics reference](https://helpx.adobe.com/photoshop/desktop/apply-painting-techniques/fill-objects-selections-layers/brush-dynamics-settings.html)
explains the UI concepts; it does not establish ABR integer encodings.

## Required acquisition families

This table is a work ledger, not a smaller release contract or a claim that a
finite set replaces the required categorical combinations/numeric envelope.

| Family | Controlled original captures |
|---|---|
| Primary geometry | Sampled and generated-ellipse roots; size, roundness, hardness, angle, spacing toggle/value and each flip separately. Include envelope/boundary values where the actual UI exposes them; retain quantization/unavailable outcomes. |
| Shape | Size/angle/roundness groups, minima, zero/nonzero/max jitter; every actual Control choice, Fade step boundaries, tilt scale and disabled-group stored values. Use an asymmetric tip. |
| Scatter | One-/two-axis toggle, amount, count, count jitter and their actual control choices, each independently. Do not assume a source RNG seed API. |
| Transfer | Options-bar opacity/flow and each dynamics control independently, including group-off values. Keep tool-setting save choices fixed. |
| Color | Distinct original foreground/background colors, mix, hue/saturation/brightness jitter, per-tip/stroke switch and every exposed related control. |
| Texture | Original nonuniform pattern; scale/depth/invert/contrast/brightness, each-tip state, origin readback if exposed, and Multiply/Subtract modes. Disabled texture state must remain explicit. |
| Dual | Original sampled/generated secondary resources; size/spacing/scatter/count/axes/flip and Multiply/off, without guessing a different mode as Multiply. |
| Blend/airbrush | Normal/Multiply/Screen/Overlay, stationary accumulation enablement and any exposed rate-related UI. No source timing equation follows from an export alone. |
| Exclusions | Record smoothing, wet edges, noise, protect-texture, pose overrides, extra build-up and any additional active UI extension; keep unsupported combinations as explicit negative/undetermined evidence. |
| Resources | Multiple presets sharing one primary/pattern/secondary; different resources with similar names; repeated names; changed export order; generated/sampled pairs and 8/16-bit masks/compression examples. |
| Containers | Independent original v1/v2 and modern 6-10 subversion1/2 inputs, then actual P2711 load/rejection and matching reference association. Re-export format is recorded separately. |

Build combination chains one setting at a time. The D row still requires all
permitted categorical combinations: four color blends, secondary off/Multiply,
and texture off/Multiply/Subtract, with the specified resource/dynamic domains.
Do not substitute pairwise/one-family captures for that complete target.

Selecting a pen-related control in a UI is not pressure/tilt/rotation execution.
Preserve warning icons/readbacks; physical device capabilities and delivered
input must be measured separately. Opaque stochastic behavior still requires
independent stroke distributions and the original statistical gates.

## Manifest and inventory

The input folder contains `manifest.json` plus only the referenced flat files.
An inventory row has exactly:

```json
{"id":"baseline-export","path":"baseline.abr","kind":"abr-export","bytes":1234,"sha256":"<actual-64-character-SHA-256>"}
```

This is a schema fragment with deliberately invalid placeholders, not a supplied
producer export. File IDs are local lowercase fixture labels. Filenames are
flat, bounded ASCII names; paths, links, reparse objects and multiply linked
files reject. No ZIP extraction or arbitrary reference path resolution exists.

| Root field | Meaning |
|---|---|
| schema/profile/collection | Version 1 contract, fixed P2711 target, generic local collection ID |
| producer | Adobe Photoshop/version 27.11, exact reported patch/build, OS/architecture, English UI, executable fingerprint, operator-matched-running-image flag, hash-only operator-private identity-evidence link and actual preset-save checkbox states |
| attestation | Explicit authorized execution, original assets and human privacy review; no personal identity field |
| files | ID/path/kind/byte-size/SHA-256; kinds abr-input, abr-export, ui-png, asset-png and observation-json |
| assets | Original sampled mask/pattern/generated ellipse definitions with rights declaration and exact source file reference |
| snapshots | Interned complete UI inventories, reused by presets without duplicating large declarations |
| cases | Original input/export roles, interpretation state, preset/resource graphs, export order and optional single-setting parent/change |

A UI snapshot has exactly `controls`, `unrepresented` and
`inventoryComplete=true`. Every key returned by the helper's `describe` command
must occur under controls. Each control record has these exact fields:

```json
{"available":true,"enabled":false,"value":17,"unit":"ui-steps","label":"Fade","choices":[],"evidence":["ui-shape"]}
```

The example demonstrates a visible greyed value, not a claimed Photoshop
default. An unavailable entry uses false/false/null with an empty choices list.
Choice controls record their actual choice inventory and selected literal label.
Numeric UI data stays in displayed units; no conversion to an undocumented ABR
scalar or enum is performed. Unknown UI controls belong under unrepresented,
with an explicit active Boolean and the same bounded readback shape.

An authored change stores preset/control/before/after and the exact
`derivedStateChanges` list. The helper computes this comparison; unknown or
missing fields, extra value changes, renamed presets and changed membership
reject. A reorder experiment changes only `export.selectionOrder`; it never
uses resource list position as an association fallback. Separate baselines can
introduce a different tip family or different preset membership.

## Source graphs and observation reports

A case names owned resource roles and every selected preset. Presets refer to
primary, optional texture, and optional secondary local resource IDs plus their
UI snapshot. Available UI-selected resources must appear in that graph.
Repeated names are permitted; names are not source IDs and never resolve an
ambiguous reference. Shared resources remain shared local nodes.

Every resource/preset has separate `ordinals.input/export` and
`identities.input/export`. A missing ABR role has null identity/ordinal. An
unobserved identity has status unresolved, null opaqueHex/evidence, its kind and
a brief reason in locator. Do not manufacture a sample/pattern/preset ID or
join by ordinal because an ID has not been decoded.

An observed identity has status observed, kind, exact lowercase opaqueHex,
structural locator and observation-file ID. A preset or inline generated
record may have no serialized ID field: retain null opaqueHex and its exact
file-bound structural locator rather than manufacturing a GUID. Sample/pattern
resource-reference IDs still require the exact observed bytes; their binding
must never fall back to an ordinal. That report has this shape:

```json
{
  "schema":"groundzero-abr-observations-1",
  "abrSha256":"<exact-original-input-or-export-hash>",
  "observer":{"kind":"bounded-structural-reader","revision":"<exact-reader-revision>"},
  "observations":[
    {"localId":"primary","kind":"sample","opaqueHex":"<exact-observed-bytes>","locator":"<actual-structural-location>"}
  ]
}
```

The ingester verifies the report's ABR hash, local node, kind and exact bytes.
It does not authenticate the observer, decode descriptors or infer semantics
from suggestive field names. Leave these identities unresolved until the built
bounded structural reader can supply them. A Photoshop live Action Descriptor
property is not proof of the serialized ABR property's meaning.

Input/export identity maps may differ after a save. Record both; do not assume
GUID stability, byte stability, resource order or preset order. The existing
`sourceDescriptorTupleV1` representation is raw structural evidence only, not a
UI-to-enum mapping. No guessed meaning is assigned to bVTy or any other integer.

## Interpretation and later render association

A historical input can be independently authored with a recorded generator
revision and no invented producer. Loading it in P2711 is a separate operation:
record loaded/rejected UI evidence against the original input hash. A new
export may be 6/2 even when the input is v1/v2 or another modern variant. Keep
both files. A pending/rejected case has no preset UI snapshot and cannot be a
single-setting export parent/child; first capture a real baseline export for
that purpose.

An optional renderAssociation records referenceSet, external manifest SHA-256,
original input ABR SHA-256 and exact producer executable SHA-256. The input-only
ingester checks those links but does not fetch or qualify the external render
set. A source load is not stroke parity. Future references must preserve the
RCP1 canvas/color/input policy, actual delivered events and source state, raw
pixels, repeated deterministic captures or independent stochastic strokes, and
the unchanged numerical/statistical gates. No unsupported Photoshop stroke,
tablet-injection, seed-setting or export API is proposed here.

## Bounds, receipts and execution status

The manifest is at most 16 MiB, with 1,024 files, 256 cases, 512 interned snapshots
and 4,096 graph nodes. Inputs total at most 512 MiB. An ABR is at most 64 MiB; UI
PNG 8 MiB; original asset PNG 16 MiB; structural observation JSON 1 MiB. PNG
signatures/chunks/checksums/IHDR dimensions are examined without IDAT decoding.
The ABR reader examines only the two header words: the second is a record count
for v1/v2 and a subversion for 6-10, never the Photoshop application version.
All complete ABR structural decoding still belongs to the bounded reader.

Producer identity evidence is hash-only and remains outside the inventory;
the helper does not fetch or authenticate it. The producer build field contains
only technical product/build text. No original private screenshot, full System
Info report, username, account name, installation path or machine identifier
belongs in a manifest, receipt or public fixture. Any permitted control PNG
is a separately privacy-reviewed technical crop, never an original private
image.

The helper retains no executable path or app bytes, never copies the original
input pack, and emits a metadata receipt only. It rejects duplicate JSON keys,
nonfinite/excessive values, traversals, unsafe file kinds and disconnected
inventory nodes. Human review must still establish original ownership, visible
UI state, actual source execution, private-image content and external reference
integrity. The receipt always keeps source-support/reference-verification,
physical-device and descriptor-enum qualification false. Unavailable controls,
unresolved source IDs and every container's evidence role remain visible.

`photoshop_export_contract_test.py` contains authored synthetic protocol,
malformation and linkage regressions. Its tiny images and header-only ABR byte
strings are explicitly not Photoshop producer exports or behavioral evidence.
No helper execution, local tests/builds, Photoshop execution or CI activity was
performed for this input-contract implementation. Final validation is deferred
until the full implementation plan is fulfilled.
