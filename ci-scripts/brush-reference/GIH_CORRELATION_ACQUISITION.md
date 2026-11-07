# Independent GIH correlation causal controls

This new acquisition keeps the existing 168-case cadence, 100-stroke statistics
and 100-stroke long-label matrices unchanged. It addresses their observed gaps
without fitting a random-selection law or changing any acceptance threshold.

## Bounded configurations

Every group contains 100 independent strokes with the exact original random GIH
resource plus two original incremental-GIH controls. Source execution is shuffled
across all 102 cases; random cases use one capture each, while the paired
incremental cases retain deterministic independent-repeat checks. Three original
RGBA frames have unique primary colors and the same original 8x8 alpha mask.
All groups retain the same resource byte hashes and fixed transfer/geometry.
The pure contract pins the random resource's SHA-256
`d5c1b2ac401d1021cb214df3c338b61ea7bff5754f370ff83a31fc24fef24913`
and incremental resource's SHA-256
`5cbaff0356727e33710a5569ae8b2dc69b41924f0be78091d1ad464bc970734b`,
matching the unchanged original corpus and historical acquisition artifacts.

| Group suffix | API and controlled change |
| --- | --- |
| line-exact | Node.paintLine, ends exactly at the 32nd nominal center |
| line-tail | Same line plus one document pixel of terminal continuation |
| path-tail | Same line/tail as a two-vertex Node.paintPath |
| dabs-tail | Same path partitioned at each nominal 32-pixel center |
| halves-tail | Same path partitioned at 16-pixel half intervals |
| west-tail | Reversed horizontal path with the same extent |
| north-tail | Vertical reversed path with the same travel distance |
| translated-tail | Original path translated by integer (13,19) pixels |
| corners-tail | Alternating right/down 32-pixel segments |

Group names have the prefix `gih-correlation-`. Each path has 32 nominal
observation centers and 992 pixels of authored travel before the explicit
terminal continuation (zero or one pixel). A nominal observation window is not
a claim about the source's hidden dab count. Missing labels, including the
measured exact-endpoint omission, stay in the output as null values.

The maximum path has 64 authored vertices. Captures are fixed 2048-square
transparent BGRA8 canvases; retained rectangles are at most 1024-square. Any
visible coverage outside the declared rectangle fails acquisition. Color-label
readback rejects mixed/nonoriginal primary-color cells rather than choosing a
nearest label. Complete source PNGs remain separate evidence.

## Independent phase-grid heldouts

Manual `focus: gih-phase-grid` adds 28 independent heldout groups. For each
grid boundary `g` in 2, 4, 8, 16, 32, 64, 128, one axis receives the exact dyadic origin
offset `1/(2g)`. X and Y are controlled separately; each offset is delivered
both as one coarse path and as a path partitioned at its 32-pixel centers.
Every configuration has its own 100 independent random strokes and two
incremental controls, preserving the 102-case job budget. Both variants have
the same nominal centers, travel and one-pixel terminal continuation.

A simple interior fractional translation could keep one cache key for every
dab under every possible grid. These boundary origins instead place recursive
future-position roundoff at a specific candidate rounding boundary: finer
power-of-two grids land at bucket interiors. The exact dyadic partition provides
an independent path control for the same origin. Raw alpha and labels are
retained so cache-key changes and visible transform quantization can be assessed
separately. Observation windows use the floor of authored fractional centers;
vertices remain exact public QPainterPath doubles and are never rounded for
delivery or replay.

Names are `gih-correlation-phase-{x|y}-g{002|004|008|016|032|064|128}-{coarse|dabs}`.
They are focused-only and do not change the original nine causal groups or full
source matrix. `sourceGridEstablished`, `phaseGridEstablished`, model validation
and parity flags remain false. The renderer's 64-bucket V1 representation is an
explicit experimental assumption until these independent source captures
identify the parameter. No heldout is executed during implementation.

## New files

- `gih_correlation_contract.py`: pure generation/delivery/control/readback bounds,
  complete randomized-execution validation and bounded diagnostic notices.
- `gih_correlation_fixtures.py`: original KPP/resource bundles, with 100 random
  strokes and two deterministic controls per configuration.
- `gih_correlation_test.py`: CI-only completeness, resource identity, path,
  malformed-delivery, capture and execution-evidence regressions.

The fixture helper copies the pure contract into the isolated source plugin.
No source application imports occur in its module; caches can load it with
`runpy.run_path` without initializing the Krita plugin package.

## Integrated acquisition and comparison

1. `prepare.py` dispatches the explicit `gih-correlation-` groups to the fixture
   helper and finalizes the input contract. The reference workflow exposes all
   original nine groups only through manual `focus: gih-correlation` and the
   28 phase-heldout groups through `focus: gih-phase-grid`; the default/full
   matrix and its existing GIH groups are unchanged. The new pure helper test is
   wired into CI validation. The parser fixture matrix discovers focused-only
   groups from the workflow and validates all 37 before source acquisition.
2. The source plugin loads the copied pure contract for these groups, validates
   the complete manifest and shuffles independent stroke execution. This shuffle
   orders cases only; no source RNG seed is set or inferred.
3. Before the generic painting branches, handle `gihCorrelationProbe` using
   `authored = gih_correlation_contract.validate_case(case)`. For Node.paintLine,
   call `node.paintLine(QPoint(*case['start']), QPoint(*case['end']), 1, 1,
   'ForegroundColor')` and return mode, the exact line-start/line-end event
   dictionaries (position and pressure1), and `physicalTabletClaimed=False`.
   For Node.paintPath, construct one QPainterPath from `authored['vertices']`,
   call `node.paintPath(path, 'ForegroundColor', 'None')`, and return mode,
   exact vertices, `singleContinuousStroke=True`, `physicalTabletClaimed=False`.
   These are the actual public API call arguments, not presumed internal dabs.
4. Reuse existing `resource_probe.resolve` before paint. After complete raw pixel
   readback, call `capture(initial, raw, w, case, effective_fields, delivery,
   resource_resolution)`; call `emit(case,evidence)`, retain its JSON diagnostic,
   and put the evidence into both `gihCorrelationEvidence` and `resourceReadback`.
   Bypass the old resource readback's fixed-east-center assumptions for these
   cases. The existing source resource summary then sees exactly 100 random
   strokes; incremental controls are excluded from its RNG population.
5. After source records are sorted into canonical fixture order, call
   `validate_execution(fixtures, records)`. It verifies all 102 identities,
   actual shuffled ordinals, delivered paths, original resource identity,
   labels against bounded decoded retained pixels, effective controls, and the
   explicit absence of parity/model-verification claims. Set `resourceResolution`
   from the actual `resolve()` result; the contract checks it independently.
   The generic report's planned/actual execution-order check remains enabled.
6. In cache validation, use the same pure manifest/execution contract through
   `runpy.run_path` and retain all existing image/repeat/hash checks. Include the
   new pure runtime helper in acquisition identity. `validate_evidence(case,
   evidence)` checks the compressed raster, hash, labels and controls without
   application imports. Also call `controls(case, parsed_effective_xml_fields,
   row['resourceResolution'])` and compare its returned evidence fields with
   recorded evidence, preserving the binding to the separately retained actual
   effective-preset XML. Only generator/test code may
   receive the same explicit exclusions as comparable existing files.
7. The native comparator validates all 102 identities before acquisition,
   validates actual execution/API delivery, and replays every authored API
   vertex in order in one continuous native stroke. It reads labels from
   `correlationCenters` and checks source PNG labels against captured evidence.
   Stochastic replicates retain distinct native seeds. Native time is explicit:
   endpoint times zero/one second for paintLine, successive vertices at 1/240s
   for paintPath; fixed dynamics disable time response, and no hidden source dab
   times are inferred. Candidate labels and bounded alpha rasters are retained.
   Existing deterministic bitmap pixel gates remain enabled for incremental
   controls, including measured endpoint differences. Random GIH uses its
   distinct experimental proposal/geometry-cache source profile, and all new
   source parity/model/phase-grid qualification flags stay false.
   No configuration distributions are pooled and no random law is fitted.

## Evidence and completion boundary

The helper records original inputs, effective controls, resource checksum,
actual API delivery, pixel hash, labels and missing slots. Both
`sourceSelectionModelVerified` and `pixelParityClaimed` stay false. Obtaining
these observations does not close random GIH source admission. A generalized
mechanism and independent heldout distributions must satisfy the unchanged RCP1
statistical/fidelity requirements before that acceptance row can close.
