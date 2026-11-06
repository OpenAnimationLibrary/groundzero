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
