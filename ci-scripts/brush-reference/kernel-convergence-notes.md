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
