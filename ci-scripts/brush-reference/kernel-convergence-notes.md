# Kernel convergence diagnostic checkpoint

The c7a7d71cb source run completed the kernel precision and public ManagedColor
sidecars. The original full-image gate still fails 18 admitted kernel cases.
These files do not change source admission, sampler equations or thresholds.

## What the new evidence establishes

Uniform-gray U8/U16 controls and exact raw initialization/repeat checks retain
constant preservation. The prefix/complement observations still disagree with
a fixed sample count. Chromatic cases also disagree with a lightness-only
explanation: for the extent-32 first-25-coordinate mask, red/green observations
are (216,39,0) for the normal mask and (43,212,0) for its complement. Treating
marked coordinates as one hit each gives cumulative-count candidates near 163
and 147. The corresponding gray observations give candidates near 147 and 131.
These are hypotheses conditioned on the assumed coordinates, not observations
of a hidden source counter.

Fresh low-contrast U16 diagnostics at diameters 72, 88 and 120 yield gray codes
30823, 30828 and 30827 for original first-30/43/76-coordinate masks with authored
low/high codes 28784/37008. A one-hit-per-point model gives denominators near
121, 173 and 306. The current area formula gives 122, 173 and 305. A global
one-sample adjustment therefore cannot explain these observations. Spatial
geometry/duplicate-hit ambiguity remains open.

The previously acquired public Lab conversions of the cumulative gray means
72/210 and 72/194 help explain the near-threshold ambiguity. Their observed
physical Lab distances are:

| Source storage | Target Lab storage | Delta E76 |
|---|---|---|
| U8 | U16 | 2.9144734144 |
| U8 | F32 | 2.9160194399 |
| U16 | U16 | 2.9983997345 |
| U16 | F32 | 3.0011482239 |

At the same cumulative ratios, chromatic pairs have much larger full Lab
distances but small lightness differences. For example red/green U16-to-U16
has Delta E76 6.9053920422 and lightness difference 0.5783170462. Public color
conversion thus supplies a direct discriminator between full-color convergence,
lightness-only convergence and storage quantization. It does not prove that the
source sampler uses any of these exact routes.

## New bounded sidecar

`kernel_convergence_probe.py` records the missing public conversions for the
original extent-32 first-25/75-coordinate masks and both polarities, four gray/
chromatic endpoint pairs, and hypothetical cumulative counts 99/100 + 16*k
for k from 0 through 16. It repeats the complete observation exactly. The maximum
is 2176 records over source U8/U16 and registered Lab U16/F32 targets.

Every record retains exact intended RGB, actual source components/storage order,
profile/depth, public Lab components and XML, physical Lab values read from that
XML, and measured Delta E76 to the previous count candidate. XML is used instead
of assuming a mapping from normalized Lab storage channels. The report binds the
existing kernel-precision artifact by SHA256 when available. Unsupported Lab
depths remain explicit. These conversions need no renderer-code inspection and
no new bitmap assets.

The result is research-only. Compare its batch distances with actual raw brush
outputs before implementing any stop rule. Minimum-count/window geometry still
needs independent original impulse or low-contrast coordinate evidence. Keep the
existing 230 source cases, exact repeats and full-image numeric limits intact.
The new contract test is intended for CI; it has not been run locally.

## Completed convergence acquisition at 9111a8970

[Kernel diagnostic run 42261285883](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42261285883)
completed source acquisition with all 2176 public-color convergence observations.
The unchanged native sampler still fails its existing full-image comparison.
The convergence artifact SHA256 is `a58fb8d6b1647646874214065b7068dad8a87ac7bf47a0755b68ef4b94cf1923`.

Using cumulative source-RGB means stored at U16, converting each through the
public API to the registered U16 Lab profile, and stopping at the first
16-sample-batch Delta E76 below 3 predicts all 16 observed extent-32 U8
prefix/complement outputs exactly with initial-count candidate 99:

| Marked points | Endpoint pair | Polarity | First count with Delta E76 < 3 | Observed/predicted RGB8 |
|---|---|---|---|---|
| 25 | gray | normal | 147 | 43,43,43 |
| 25 | gray | complement | 131 | 206,206,206 |
| 75 | gray | normal | 227 | 84,84,84 |
| 75 | gray | complement | 211 | 164,164,164 |
| 25 | red-green | normal | 163 | 216,39,0 |
| 25 | red-green | complement | 147 | 43,212,0 |
| 75 | red-green | normal | 307 | 193,62,0 |
| 75 | red-green | complement | 275 | 70,185,0 |
| 25 | blue-yellow | normal | 195 | 33,33,222 |
| 25 | blue-yellow | complement | 163 | 216,216,39 |
| 75 | blue-yellow | normal | 355 | 54,54,201 |
| 75 | blue-yellow | complement | 307 | 193,193,62 |
| 25 | near-lightness | normal | 147 | 165,48,86 |
| 25 | near-lightness | complement | 115 | 67,107,164 |
| 75 | near-lightness | normal | 243 | 143,62,104 |
| 75 | near-lightness | complement | 211 | 89,94,147 |

The equivalent U8-RGB-mean conversion is contradicted by two observations:
75-point gray complement stops at 227 instead of the observed 211, and the
75-point blue/yellow normal track has not stopped by 355 although its observed
output matches that count. Thus quantizing the cumulative mean to RGB8 before
convergence evaluation does not explain the measurements.

U16 means converted to F32 Lab happen to choose the same counts on these 16
tracks. The earlier 72/210 versus 72/194 gray boundary discriminator instead
places the U16-Lab route below 3 and the F32-Lab route above 3, favoring the
quantized U16 Lab hypothesis when these measurements are considered together.
This does not expose the source's internal counter or establish every input,
minimum-count rule, source profile or color transform implementation.

## Single-mark geometry acquisition

`kernel_single_mark_probe.py` adds 70 raw U16 diagnostic cases using the existing
seven original diameter/radius preset combinations. Each uses four distinct
coordinates from the original quarter mask, both 112/144 polarities, and two
uniform controls. Every source result repeats from exact raw initial pixels;
profile/depth/effective-preset identity and full bounded raw crops are retained.
No denominator is written into the fixture. Equal response at the separate
coordinates constrains one-hit normalization; differing responses distinguish
clipping or duplicate sampling from a global count formula. These observations
are required before changing the native minimum-count/window model.
