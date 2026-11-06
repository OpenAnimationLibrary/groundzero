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
