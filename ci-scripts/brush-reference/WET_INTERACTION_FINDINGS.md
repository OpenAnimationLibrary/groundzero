# Wet interaction implementation audit

The original S contract in `doc/raster_brush_compatibility.md` remains normative:
all four models, enabled/disabled smudge length, alpha transport, applicable
dynamics and Multiply/Subtract texture combinations remain required. Native
conversion or a passing structural oracle does not close a source-fidelity row.
This audit uses existing local code and already downloaded source artifacts;
no new source run, remote check, artifact acquisition or local test was used.

## Disabled smudge length

`sourceWetDab` already distinguishes the serialized length from its enable
state. Disabled length selects full transport strength before opacity and the
model-specific New Dulling attenuation. Legacy foreground attenuation still
uses the retained serialized length. `sourceSmearAlpha=false` retains distinct
source-over feedback; New Dulling also orders foreground injection before its
pickup mix. Replacing a disabled length with zero would contradict measured
source output.

The existing c7 `wetopacitycombo` artifact contains half-opacity, length-off
second/third-dab RGBA8 centers:

| Model | Alpha transport | Second | Third |
|---|---|---|---|
| Legacy Smear/Dulling | on | 85,11,159,192 | 148,18,88,162 |
| Legacy Smear/Dulling | off | 85,11,159,192 | 157,17,81,177 |
| New Smear | on | 82,11,163,193 | 143,18,93,164 |
| New Smear | off | 82,11,163,193 | 153,17,85,179 |
| New Dulling | on | 105,11,139,181 | 173,18,64,153 |
| New Dulling | off | 110,5,141,179 | 189,7,60,164 |

All New Dulling toggle cases passed the stored whole-region comparison. The
sole stored failure across 88 cases was New Smear length-off at the third dab:
its maximum alpha error was one encoded byte, represented as
0.0039215686274510775 after subtracting normalized alpha. The comparator's
subsequent integer-code alpha-difference correction addresses that numerical
gate defect; it does not establish a different wet recurrence or change the
allowed one-byte limit.

`testSourceWetLengthToggleMeaning` now retains the measured centers directly,
including opacity, length and alpha combinations for every model. It compares
encoded integer channels to the existing one-byte source tolerance. No runtime
length equation was changed based on this audit. Broader dynamic combinations
remain subject to their original source gates.

## Off-document state

The current runtime separates finite host reads/writes from budgeted transient
`sourceOutside` tiles, snapshots feedback before destination writes and carries
the complete pickup rectangle. Finish/cancel teardown releases that storage.
These mechanisms already address the missing off-document state; deleting it,
clipping the moving field or resetting the reservoir at document entry would
contradict the previously documented outside-layer observations.

The existing structural regression covered a left-edge exit/reentry path.
`testSourceWetBoundaryMatrix` now adds the complete original 96-control shape:
four models, four edges, entry/exit/reentry, color rates zero/0.25, 32px tips and
8px input steps. Each finite-host result is compared with the same continuous
stroke on a sufficiently padded transparent canvas; cadence and storage release
are also retained. This independently checks the clipping/state invariant and
does not claim that the candidate matches a foreign renderer.

No downloaded `wetboundary` source artifact was present in the available local
artifact inventory. Consequently this audit identifies no justified additional
Dulling transport correction or precise remaining boundary mismatch. The
unchanged original full-region/outside-probe source gates must resolve any
remaining source discrepancy after planned implementation is complete.

## New Dulling texture is a concrete missing state rule

The current candidate explicitly rejects New Dulling with brush texture. That
restriction leaves a required S row unfinished; it is not a release-scope
exception. Removing the guard would apply a rule measured only for Legacy
Smear/Dulling and New Smear: the texture-masked displayed result becomes the
next retained field. Existing source documentation records that New Dulling
retained state differs.

The exact missing operation is the texture-aware reduction of the previous
feedback neighborhood before New Dulling's next pickup. In particular,
`pickupAt` currently returns a single center sample immediately at radius zero,
while the recorded New Dulling texture discriminator summary rules out a
center-only explanation. The general nonzero-radius adaptive sampler also does
not specify a texture-aware radius-zero neighborhood. The original pattern
resource, its byte-domain coverage, neighborhood coordinates/extent, acceptance
or weighting rule, and empty-neighborhood fallback must be accounted for
separately from destination deposition. Otherwise a hole at the center can
incorrectly discard surrounding retained paint, or an arbitrary replacement
average can inject paint from outside the source's pickup region.

The repository already defines `texturestate` and `texturepickup` discriminators
for these alternatives: phase changes, center bytes 127/128/129, off-axis points,
radius-two/four holes, asymmetric ramps/diagonals and uniform/nonuniform RGBA
backgrounds. Their raw artifacts were not present locally. The older proposed
coverage-aware neighborhood's two-byte centerline agreement does not uniquely
identify its threshold, filter or fallback, so no such approximate model was
activated. Those existing discriminator records and unchanged whole-region
gates remain necessary to implement and qualify this required row.
