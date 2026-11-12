# GIH acquisition findings during PR 115 recovery

These are observations from original public-API fixtures, not a source random
selection model or a source-reference verification claim. Existing fidelity and
statistical thresholds remain unchanged.

## Evidence

Implementation c7a7d71cb, full reference run:
https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42248393484

- Cadence job 127996744034 acquired all 168 source cases but stopped during the
  first candidate comparison with `GIH importer did not separate incremental
  from random source behavior`. Its artifact contains only native case000.
- Statistics job 127996743585 acquired 100 strokes and failed the trailing-band
  occupied-tip gate. Other currently measured alpha statistics passed.
- Long-label job 127996743298 acquired 100 strokes and failed all 27
  per-band transition-joint gates; marginal frame frequencies passed.

## Deterministic admission diagnosis

The original resource-000.bundle contains an incremental GIH definition with
scale 1.0 and spacing 2, `PressureSize=false`, and `SizeUseCurve=false`.
`eligibleFixedSource` previously recognized the disabled pressure setting but
classified the additional explicit disabled Size curve flag as unknown. This
selected a native conversion before deterministic source-bitmap admission.

The narrow correction recognizes `SizeUseCurve=false` only when PressureSize is
explicitly false. True, numeric-string and unknown values still fail fixed-size
candidate admission. New bundle-to-import regressions retain those negative
controls and exercise the accepted sequence through portable/render lifecycle.
This correction does not admit random GIH or broaden source geometry.

## Separate source endpoint and random-state gaps

The statistics fixture's line ends exactly at the last authored center. The
source paints 31 of its 32 slots in each of 100 strokes; the native conversion
paints 32. The long-label path similarly paints 3071 of 3072 source slots versus
3072 native slots. Keep this measured endpoint distinction visible; deleting
missing slots or changing denominators would hide it.

The long-label source has equal adjacent labels in 164476 of 302300 interior
pairs (about 0.544), and 1628 of 4700 row-turn pairs (about 0.346). Across the four
short cadence replicates at each setting, random discrete strokes and the
alternating-corner path show approximately independent label changes, while
straight continuous paths have visibly more repeated labels. For example,
spacing-two line has 72 equal adjacent labels of 124 pairs, versus 39 of 124 for
spacing-two alternating corners and 41 of 124 for separate strokes. These are
descriptive original observations; four short replicates cannot establish a
reusable transition law.

Consequently an independent uniform-per-native-dab RNG does not reproduce the
observed source sequence correlations. A fitted single stay probability is also
unsupported: the source observations vary with direction and delivery mode.
Random source admission must remain separate until a generalized mechanism is
supported by independent controls and heldouts.

## Required follow-up

- Preserve the existing cadence, straight-stroke and long-label acquisitions.
- Add independent controls that vary direction changes, path partition and
  delivery cadence without changing the resource or intended geometry.
- Compare equivalent translated paths and additional original frame counts and
  mask/color resource configurations before choosing a generalized state model.
- Keep endpoint behavior and resource selection separate; use explicit terminal
  holdouts in addition to the existing exact-endpoint observations.
- Validate the selected model on 100 independent strokes per held-out
  configuration, all spatial bands and unchanged statistical gates. No generic
  native conversion can satisfy the missing source-profile acceptance row.

## Versioned proposal/cache mechanism

Analysis used only the already retained cadence168, statistics100 and
long-label100 artifacts above, together with the independently recovered wet
future-position recurrence. No foreign renderer implementation was inspected,
no source RNG seed was inferred, and no new acquisition was used to choose a
stay probability.

A deterministic geometry-only hold rule is contradicted by the statistics
matrix: every one of its 30 observed edges repeats in only 43?66 of 100 strokes,
with no edge repeating in all 100. The supported mechanism uses two independent
frame selections: propose a frame for the transformed-mask cache key; on a
cache hit reuse the previous actual frame; on a miss independently choose the
actual frame. Cache hits require the same proposal and effective geometry.
For N original uniform frames this predicts adjacent repetition
`1/N + (1 - 1/N)/N`; the three-frame prediction is 5/9. Geometry changes force a
fresh actual frame and therefore predict 1/3. These probabilities are derived
from the mechanism and are not renderer parameters.

Future positions follow residual recursive weighted interpolation within each
authored segment, resetting to authored vertices. The geometry key retains the
upper fractional phase endpoint: an integer approached from below has phase 64,
while the exact integer has phase 0. Both quantize to the same raster position,
but their keys differ. Applying that rule to all 307000 observed long-path pairs
separates them as follows:

| Independently modeled key transition | Equal source labels / observed pairs | Observed rate | Mechanism prediction |
| --- | --- | --- | --- |
| unchanged | 159060 / 286100 | 0.55596 | 5/9 |
| changed | 7044 / 20900 | 0.33703 | 1/3 |

The predicted total is 165911.11 equal pairs versus 166104 observed. Visible
alpha-mask equality alone misses 115 key changes per stroke: those 11500 pairs
have 3760 equal labels (0.32696), although their raster masks are unchanged.
This is the useful discriminator for hidden transform-key invalidation rather
than a rule fitted to visible direction or pixel shape. Exact-float keys
invalidate too frequently; floor-quantized keys also misclassify changes.

Statistics100 has 1650/3000 equal adjacent labels (0.55), consistent with the
unchanged-key prediction. Cadence corner transitions have 36/120 equal labels
across changed masks, whereas continuous straight modes generally agree with
the unchanged-key prediction. Four-stroke cadence subsets are small: the
spacing-three and Qt half-step subsets show roughly 2.7-standard-deviation
lower repeat counts, which does not justify a separate fitted delivery law.
Observed statistics lag-two and lag-three equal-label rates are 0.40207 and
0.36321, versus mechanism predictions 0.40741 and 0.35802 when geometry stays
unchanged.

`krita-5.3.4.1-random-bitmap-candidate-1` implements this mechanism using the
existing original RGBA frame representation. Its cache is stroke-local and
keys resource identity, proposal, transformed scale/aspect/rotation/mirror and
both fractional phases. It uses a fresh uniform actual selection on misses,
with no fitted probability or direction/index lookup table. Native GIH retains
its existing independent uniform-per-dab rules. Source sampling defers each
moving deposit until its half-pixel future exists, preserving exact-terminal
omission. The implementation retains deterministic native seed regression,
source cache invalidation/seed/reset/endpoint regressions and strict
import/portable resource checks. These tests await final CI execution.

This remains an experimental source profile. Existing whole/half-pixel paths
identify the noncanonical phase endpoint distinction, but do not uniquely
identify 64 versus every other fine phase grid;64 is the explicit V1 key
representation. The new independently authored causal configurations, 28fractional-origin/path
phase-boundary heldouts and unchanged statistical/fidelity gates must validate
the generalized mechanism and identify the phase parameter.
Global source semantic/reference qualification and source seed-parity claims
remain false.
