# Kernel convergence diagnostic checkpoint

The c7a7d71cb source run completed the kernel precision and public ManagedColor
sidecars. The original full-image gate still fails 18 admitted kernel cases.
The sidecars preserve source admission and numeric thresholds. The completed
color calibration now informs the bounded adaptive candidate described below;
the general minimum-count/window rule remains unresolved.

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


## Completed single-mark observations

The completed 70-case sidecar retains all 14 exact uniform controls and all
four separate points with both polarities at each of seven geometries. Point
responses are identical within each geometry. The U16 output differences from
the uniform controls are:

| Diameter | Authored radius percent | Absolute U16 difference |
|---|---|---|
| 56 | 100 | 103 |
| 64 | 100 | 83 |
| 32 | 250 | 56 |
| 64 | 150 | 41 |
| 72 | 100 | 68 |
| 88 | 100 | 48 |
| 120 | 100 | 27 |

Combined with the existing quarter masks, these constrain count candidates
80, 99, 146, 202, 121, 173 and 306 respectively. A general size/window equation
has not been established. The production candidate retains the existing
`max(80, floor(.02 * width * max(0, width - 1) + 20))` minimum unchanged.
No size table, fitted denominator, fixture lookup or comparison epsilon is used.
Differences remaining at other diameters must stay visible in the original
full-image gate.

## Independently reproduced source color transform

The named source profile is public standards data:
[sRGB-elle-V2-srgbtrc.icc at the pinned source revision](https://raw.githubusercontent.com/KDE/krita/bd6e8d061d2f0388c5b10278786cf0fe62e3135f/krita/data/profiles/elles-icc-profiles/sRGB-elle-V2-srgbtrc.icc).
Its 9080 bytes have SHA256
`5991c8d8fcb628dad5d052d9341df8a32bd3c7a794c913a8ede8eae4b34b4545`.
Each of its 4096 shared TRC entries is exactly the half-up U16 quantization of
the standard inverse sRGB transfer function at index/4095. Its XYZ matrix
columns, as signed 16.16 integers divided by 65536, are:

| Channel | X | Y | Z |
|---|---|---|---|
| Red | 28576 | 14581 | 912 |
| Green | 25239 | 46983 | 6361 |
| Blue | 9375 | 3972 | 46788 |

The Lab conversion uses color-management D50 (.9642, 1, .8249), not the slightly
quantized ICC white-point tag. Standard CIE Lab equations and full-range LabV4
U16 encoding produce vertices of a 33 by 33 by 33 grid. Fixed-domain U16 TRC
interpolation and generic tetrahedral interpolation reproduce all 544 downloaded
U16-RGB to U16-Lab observations, all three raw channels, exactly. No brush
renderer implementation was inspected or copied. The relevant color-management
math is documented in official LittleCMS
[interpolation](https://github.com/mm2/Little-CMS/blob/lcms2.16/src/cmsintrp.c)
and [optimization](https://github.com/mm2/Little-CMS/blob/lcms2.16/src/cmsopt.c)
sources. The implementation sorts the three fractional axes and accumulates
successive vertex differences, with explicitly defined signed rounding.

All 544 public source-U16 component writes truncate the intended normalized
RGB mean times 65535; half-up input encoding disagrees with 741 individual
channels. Lab output encoding itself is half-up. The public ordered Lab a/b
components have asymmetric normalization; raw LabV4 codes decode to a physical
value `code / 257 - 128`, then positive a/b values acquire an additional
127/128 factor in the pinned public XML serializer. The observed neutral stays
zero. The convergence candidate preserves that measured physical metric.
ICC legacy LabV2 encoding is different; see the primary
[ICC encoding discussion](https://www.color.org/v2issues/) and
[ICC.1:2001-12 Tables 67/68](https://www.color.org/newiccspec.pdf).
This calibration is versioned to the observed profile and source 5.3.4.1;
it does not establish conversion parity for other profiles or source versions.

## Activated bounded adaptive candidate

The private sampling helper now evaluates cumulative premultiplied means, with
straight RGB converted through the calibrated U16 transform. It compares full
physical Lab distance with the previous cumulative checkpoint. The first
comparison is between minimum-minus-16 and minimum attempts; subsequent
checkpoints are 16 attempts apart. Convergence is strictly Delta E76 below 3.
The resulting cumulative RGBA mean, including alpha, remains the pickup value.
The radius-zero single-pixel path and native profiles retain their existing
behavior. New Dulling retains its existing tip rejection and attempted-count
basis; accepted-count behavior for other modern clipped masks remains open.
Opaque source observations establish the color rule; arbitrary partial-alpha
inputs still require their own full-image source qualification.

The transform creates one immutable 215622-byte table lazily, reserves its
complete fixed allocation in a dedicated process-cache memory budget, and
retains no ICC asset or per-fixture data. Initialization is bounded to 33 TRC
vertices and 35937 matrix/Lab vertices. Per-stroke state is allocation-free.
Initial sequence work, additional 16-attempt batches, and each checkpoint
conversion are charged before reads or conversions to the existing input-work
budget. Sampling has an additional one-million-attempt bound; exhausting either
bound fails the dab and returns no usable unconverged partial pickup.

Focused C++ regressions retain independently captured raw Lab triples and all
16 observed gray/chromatic RGB outputs, plus exact-batch/boundary, rejected
sample, invalid-input, conversion-failure and work-cap cases. They have been
written for final CI and have not been executed locally. The 544-color exact
match is numerical analysis of already acquired public observations, not a
claim that the native brush comparison or all checks have passed.


## Exact accumulation at source RGBA8 boundaries

The source candidate initially samples a quantized RGBA8 surface. Its retained
wet field has a different storage contract: the first field contains its
unquantized pickup mean, and later halos can preserve unchanged floating
backdrop values. Repeated floating summation can move a uniform normalized byte value
just below its exact U16 boundary, so flooring a floating cumulative mean alone
is insufficient. An explicit `QuantizedRgba8` accumulation mode recovers rounded
U16 straight RGB and U16 alpha codes from each sampled pixel and sums
`RGBcode * alphaCode` and `alphaCode` in unsigned 64-bit integers. Lab input is
the exact truncating integer quotient `sum(RGBcode * alphaCode) / sum(alphaCode)`.
The returned premultiplied mean uses the same sums divided by accepted count
and 65535 squared, with alpha divided by accepted count and 65535. This is
algebraically the existing premultiplied mean for quantized RGBA8, including
mixed alpha; it adds no alpha-specific convergence criterion or new alpha rule.

At the one-million-attempt cap the largest product sum is
4294836225000000, below 2^52, so all integer sums and their conversions to double
are exact. No epsilon, nextafter adjustment or fixture-specific rounding is
introduced. The renderer explicitly selects quantized storage only for the initial surface
pickup and charges its channel-code recovery work. Every retained-field pickup
uses floating accumulation, preserving an exactly uniform accepted Pixel
without averaging or U16 storage rounding. Its nonuniform sums use compensated
addition. Thus a retained .25 stays .25, and sub-U16 alpha stays nonzero; Lab
checkpoint conversion does not impose a new storage quantization stage. Authored non-neutral uniform regressions repeat
at three different minimum counts and three RGBA8 alpha codes; a repeated
mixed-alpha red/blue case verifies the same premultiplied weighting and exact
Lab input quotient. These regressions are written but have not been executed.
This numerical repair does not expand the previously stated source alpha
qualification scope.


## Cold transform preparation and retained-field regression coverage

The nonallocating transform-readiness query reports a conservative fixed
4608384 operation units while the 33-cube transform is cold: 128 units per
matrix/Lab vertex plus 256 per TRC grid input. Source rendering charges this
cost to the existing per-input operation budget before any sampling can trigger
transform allocation or construction. Memory remains reserved before table
allocation. Warmed conversions retain their separate checkpoint charges.
A racing cold caller may conservatively charge the same construction cost;
readiness is published atomically only after the immutable cache exists.

Authored preparation-state regressions check that readiness inspection neither
builds nor allocates the cache and that initialization changes the reported
preparation cost to zero. Retained-floating regressions cover .25, non-neutral
rational means, alpha below one U16 code, and nonuniform mixtures with small
representable contributions. These are internal numerical/storage invariants,
not new claims of source alpha parity. They have not been executed locally.
