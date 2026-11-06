# Smear placement investigation

The c7a7d71cb source run exposed a New Smear failure at the 10,000-dab checkpoint:
white-composite DeltaE00 p95 was 6.478077625, above the unchanged limit of 6.
The actual candidate dab count was exactly 10,000. RGB/alpha mean errors and the
other listed drift limits passed. This is not qualification of the profile.

Evidence: source reference run 42248393484, wetdrift job 127996739309, artifact
`krita-reference-wetdrift-c7a7d71cb9d06da96243e3c78391c011906388c8`.
Full source, repeated source, candidate and diff PNGs plus comparison.json are
retained by the job. Inspection used those recorded pixels; no local builds or
tests ran, and no foreign renderer implementation was consulted.

## Observed geometry

The original drift background has no green channel, while the foreground is
green. Nonzero-green pixel support therefore exposes deposited mark extents.
Within the 16px cell whose authored center is (24,328), both Legacy Smear and
New Smear source marks extend y=-5..2 relative to the center. The candidate
extends y=-4..3. At the same row, columns near x=1032 have unshifted source
support. At the next reverse row, the sampled near-left cells agree in y.

Across the inspected cells, both Smear source models share the same varying
one-pixel vertical shifts. Dry count controls and both Dulling source models do
not show these shifts in the inspected cells. This identifies a Smear placement
boundary; a fixed directional offset or a New Smear reservoir-only correction
would not explain the observed pattern.

Source and candidate center pixels usually differ by only one encoded level.
The larger errors are concentrated around discrepant mark edges. The full
fidelity gate remains at the original coordinates, without image alignment,
region trimming, added epsilon or relaxed perceptual thresholds.

Weighted endpoint interpolation followed by discrete placement is a hypothesis:
roundoff below an integer coordinate could explain a one-pixel raster shift.
Simple weighted interpolation at the nominal/future fractions did not reproduce
all observed shift patterns. It is not an established source equation and has
not been introduced into the renderer.

## Discriminating acquisition

The `wetgeometry` group is a separate original 30-case acquisition. Dry,
Legacy Smear and New Smear controls use the same hard 8px round tip, spacing 2,
opaque-blue initial canvas and opaque-green foreground. Each case declares the
source engine explicitly and validates effective tip, transfer and wet settings.

Ten paths distinguish Y origins 40/328/968, positive/negative one-eighth-pixel
Y displacement at 328, reversed/vertical direction, and a multi-turn path with
coarse, subdivided and transposed authored vertices. Fractional positions use
QPainterPath rather than the integer-only paintLine interface. Each case is a
single source stroke; existing independent-repeat checks remain mandatory.

The readback records bounded document windows every 16 authored path pixels,
nonzero-green bounds/counts and pixel hashes. These are observation windows,
not a claim about the source's hidden dab coordinates or counter. Complete
PNG records remain the source facts. Diagnostic observations do not promote a
candidate, replace the original 10,000-dab test or establish source parity.

A renderer change should follow a generalized rule distinguished by these
original measurements and held-out translations/directions. The original
wetdrift gate must pass on the final implementation without weakened assertions.

## Completed discriminator observations at 9111a8970

Run 42257828455, job 128032799812 completed all 30 original wetgeometry cases
with exact independent-repeat gates. At straight Y328, both Smear models shift
the deposited 8x8 raster upward by one pixel in observation windows 5-8 and
21-24. At Y968 the shifted windows are 22-27. At Y40 the sampled marks do not
show this shift. Reversal retains the same Y328 shift indices; transposition
moves the anomaly to X. The positive/negative one-eighth-pixel Y controls do
not exhibit the integer-boundary anomaly.

The equivalent coarse and subdivided multi-turn paths have identical complete
canonical pixels for the dry control. For both Smear models, 12 authored windows
differ: 20,21,22 and 87 through 95. All those windows have the observed one-pixel
placement discrepancy; the subdivided Smear paths have no anomalous vertical
shift. Their complete canonical source hashes differ as well. This establishes
source Node.paintPath partition-dependent Smear placement for these inputs,
not a general reservoir equation or actual-tablet behavior.

Simple nominal/future weighted interpolation and Bernstein-coordinate hypotheses
still fail to explain every observed pattern. No speculative renderer change is
introduced. The next evidence must distinguish this public API's path behavior
from actual Qt-delivered tablet painting before a universal source-placement
rule is inferred. Original wetdrift acceptance thresholds remain unchanged.

## General raster-tip placement distinguished by existing captures

Inspection of 516 deposited marks from the existing source PNGs (both Smear
models, straight/reversed/vertical/fractional controls and subdivided turns)
finds the same translated binary support in every mark:

```text
00111100
01111110
11111111
11111111
11111111
11111111
01111110
00111100
```

Every support is exactly 8x8 with 52 green pixels. The 128 nonempty marks in
both fractional-Y controls across the two models have top coordinate
`floor(authoredY)-4`: 327.875 gives 323, while 328.125 gives 324. They retain
this fixed mask instead of continuously resampling its coverage with the
fractional input. At the vertical control's authored (328,272) observation,
the source bounds are (324,268,8,8), also a fixed raster mask. Dry controls use
a different lattice: the same fractional-Y marks are 7x8 with 50 green pixels.
Thus any placement correction must remain specific to Smear.

A generalized candidate rule distinguished by these observations places the
rasterized Smear tip at `origin=floor(futurePosition-radius)` independently on
each axis, then samples its fixed mask around `origin+radius`. For the existing
even-size integer horizontal and reversed controls, this agrees with the
previous half-pixel direction convention. It also gives the correct fractional
and vertical mask positions. With an odd diameter, the origin rule retains the
half-integer center of the existing symmetric odd raster. This is an inferred
raster-tip placement model, not a measured hidden dab coordinate or a lookup
of special paths/windows.

At the initial raster-tip checkpoint, the public Node.paintPath integer-axis
anomaly was still a separate problem:
coarse path partitioning sometimes places that fixed raster one pixel below
its nominal integer boundary, while equivalent subdivided paths do not.
Quantizing a stable constant coordinate alone cannot reproduce those selected
windows. No epsilon, offset table, new source run or weakened gate is justified
by this analysis. Original wetdrift and other wet fidelity gates still have to
validate any implementation at the end of the planned work.

## Recursive future geometry distinguished by existing pixels

Further arithmetic reconstruction uses the same downloaded observations; no new
source run or foreign brush-renderer implementation was consulted. A recursive
future-position rule predicts all 518 deposited bounds in each Smear model's ten
geometry paths, including the coarse/subdivided/transposed turns: 1,036 bounds
with zero mismatches. The same rule then predicts all 9,999 deposited mark bounds
in each original c7 10,000-dab Smear PNG: 19,998 held-out bounds with zero
mismatches, including the changing rows and the (24,328) failure region.

For each authored segment, start at its exact first endpoint and track its
original Euclidean length as remaining distance. Advance the running position
toward the fixed segment endpoint using weighted interpolation at the distance
to the next future sample. Subtract that consumed distance from the remaining
scalar; do not recompute length from the rounded current coordinates. Carry the
unconsumed distance-to-next-sample across every authored endpoint. For these 8px,
spacing-2 paths, the first moving future sample is 16.5px from stroke start and
later samples advance 16px each. Raster-tip origins use the separate floor rule
above.

```text
toNext = firstSpacing + 0.5
for each authored segment:
    position = segmentStart
    remaining = originalSegmentLength
    for each event admitted by the unchanged future-availability scheduler:
        t = toNext / remaining
        position = (1-t)*position + t*segmentEnd
        remaining -= toNext
        toNext = currentSpacing
    toNext -= remaining
```

The distinction is measurable. Recursive nominal 16px stepping, with or without
a separate half-pixel interpolation, fails the straight controls. Recomputing
remaining length from rounded positions fails 98 geometry windows per model.
Recovering the next advance by subtracting global cumulative distances instead
of tracking the residual invents a false shift in subdivided window 67. The
tracked residual rule produces the captured selected shifts without an index
table or epsilon, and naturally resets their arithmetic at authored vertices.
Subtracting the consumed distance and multiplying remaining length by (1-t)
were observationally equivalent on these controls; the implementation uses the
explicit consumed-distance bookkeeping above.

The candidate correction is restricted to source Smear geometry. Nominal input
samples, pressure/time interpolation, source dab cadence, source Pixel/native
scheduling, cancellation and pixel-work budgets remain independent. One retained
scalar participates in the existing Impl snapshot-size charge; no additional
history allocation or changed pruning is needed. Weighted coordinate products
are rounded separately to binary64 so compiler FMA contraction does not change
the measured integer-boundary arithmetic. Focused regressions record the source
raster supports, origins and coarse/subdivided placement; they are prepared for
final validation after implementation is complete.

The held-out claim concerns mark bounds. In the low-alpha first drift row,
127 deposited marks have 58 raw-green support pixels; the other 9,872 have 52.
The model does not claim that every drift mark has the opaque-blue diagnostic's
52-pixel binary support, nor does matching geometry establish color/feedback
parity. The unchanged full fidelity gates still determine final qualification,
and actual Qt tablet geometry remains a separately bounded reference question.
