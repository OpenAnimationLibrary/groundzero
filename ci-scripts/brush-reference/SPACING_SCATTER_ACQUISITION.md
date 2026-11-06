# Original automatic-spacing and scatter acquisition

These files prepare source evidence needed by RCP1. They do not implement a
foreign equation, identify the source RNG, admit a source profile, or grant
reference verification. They use only original generated hard-circle tips.
The original numeric/fidelity/statistical contract remains unchanged.

## Metadata and public behavior

Pinned Krita `bd6e8d0` serialization metadata establishes the exact controls:

- [Auto-brush factory serialization](https://github.com/KDE/krita/blob/bd6e8d0/libs/brush/kis_auto_brush_factory.cpp)
  reads/writes `spacing`, `useAutoSpacing`, and `autoSpacingCoeff` separately.
  The fixture deliberately keeps manual spacing at one while changing the
  automatic coefficient, so confusing the two values can be detected.
- [Scatter option data](https://github.com/KDE/krita/blob/bd6e8d0/plugins/paintops/libpaintop/KisScatterOptionData.cpp)
  writes `Scattering/AxisX` and `Scattering/AxisY`, uses option ID `Scatter`, and
  declares the 0 to 5 scalar range. Legacy `Scattering/Amount` is a fallback only
  when `ScatterValue` is absent; the new fixtures use explicit `ScatterValue`.
- [Krita options manual](https://docs.krita.org/en/reference_manual/brushes/brush_settings/options.html)
  describes X scatter along drawing direction and Y scatter across it.
- [Brush-tip manual](https://docs.krita.org/en/reference_manual/brushes/brush_settings/brush_tips.html)
  distinguishes automatic spacing from manual spacing. Its description does
  not establish a complete equation or the precise serialized scalar mapping.

Only metadata is used from those source files. No renderer implementation,
foreign brush pixels or source RNG code is copied.

## Added source contracts

`spacing_scatter_fixtures.prepare(root, resources, preset_writer, group)` accepts:

- `autospacing`: 192 paired cases. Diameter 1/8/64/256; aspect 1/.5; coefficient
  .1/.5/1/2; east/north/diagonal paths; automatic on/off. Manual spacing remains
  one, opacity is .5 and flow one. Fractional opacity exposes overlapping
  deposits. Ordinary independent deterministic repeat checks still apply.
- `scatter-x-east`, `scatter-x-north`, `scatter-y-east`, `scatter-y-north`,
  `scatter-xy-east`, `scatter-xy-north`: each has 100 independent enabled
  strokes with identical authored controls, plus two deterministic disabled
  controls. Diameter eight, manual spacing four and amount one isolate the
  active axis before size/spacing/dynamic combinations are added.

Every case declares `sourceEngine=paintbrush`, explicit unrelated disabled
controls, and an exact original `Node.paintLine` path. The source validator
compares effective option values, brush attributes and generator geometry with
requested controls, verifies delivered endpoints/pressure, preserves bounded
BGRA pixels and rejects coverage outside the capture region. Empty output fails.
No images are aligned to the candidate or cropped to candidate bounds.

`scatter_statistics.py` aligns each raster by its fixed authored direction,
then computes all three spatial bands independently: coverage fraction,
integrated alpha, mean width, occupied authored cells, and normalized texture
energy at scales 1/2/4/8. It reuses RCP1's fixed 10,000-resample bootstrap and
standard-deviation ratio gates from `stochastic_gate.compare`. It rejects fewer
than 100 enabled source/candidate samples, duplicate/missing stroke identities,
missing actual randomized source execution evidence, repeated candidate streams,
pooled configurations, malformed raster streams and oversized regions. Disabled
controls do not count toward the 100 random samples. Passing these statistics
still leaves semantic profile admission and visual/combination review separate.

## Integration contract

`prepare.py` now dispatches these seven groups and applies the existing input
finalizer. `probe.py` validates complete manifests before painting, randomizes
scatter case order, and records intended, planned and actual execution order.
It invokes `spacing_scatter_probe.capture` after actual source readback and stores
`autoSpacingEvidence` or `scatterEvidence`, plus bounded per-preset sidecars and
notices. Only enabled scatter cases omit deterministic pixel repeats; the 100
independent calls replace that unsuitable assertion. Disabled controls and all
automatic-spacing cases retain exact repeat checks. Existing checks are retained.
The `spacing-scatter` workflow focus and full source matrix include all seven
groups, and oracle-cache validation invokes `spacing_scatter_test.py`.

The comparator integration preserves these requirements:

1. Replay the exact delivered path and report source/candidate
   controls and complete pixels. To run scatter statistics, each candidate record
   must retain its `inputCase`, `preset`, a distinct canonical `candidateSeed`, and
   bounded `stochasticAlpha` matching the source rectangle. Source replay itself
   does not establish native support; do not label a native conversion as a source
   profile merely to get a comparison image.
2. Enforce group cardinality/identity guards (192 for automatic spacing, 102 for
   each scatter group). The workflow invokes `scatter_statistics.py REFERENCE_ROOT`
   after successful scatter comparisons. Retain full reference/candidate/diff
   images and analysis reports. No local tests run.

## Remaining scope

This is a first discriminating acquisition, not completion of the K/S contract.
Scatter amount limits and heldouts, pressure/fuzzy dynamics, anisotropic/rotated
resources, interactions with transfer/texture/masks, other declared blend modes,
and all applicable wet profiles remain separate acquisition/implementation work.
Automatic spacing still needs bitmap/soft/Gaussian/resource combinations and
source-cadence/partition holdouts. Runtime admission requires an independent
measured model and unchanged final-head fidelity gates. Never infer a quadratic
formula or a uniform RNG solely from the manual or an option name.
