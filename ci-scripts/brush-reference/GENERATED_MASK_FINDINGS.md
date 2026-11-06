# Original generated Soft/Gaussian mask findings

The independent models below match every retained alpha pixel in 124 new
`generatedcurves` cases and 24 older Soft/Gaussian `generatedtips` cases within
one encoded alpha byte. The comparison covers 1,363,968 pixels, including
constant, linear, nonlinear, asymmetric-fade and fractional-origin controls.
No pixel differs by more than one byte; worst per-case alpha MAE is
0.0735677084 byte over the complete 96-square capture. This is numerical source
research, not completion of source admission, full-image fidelity, broader
geometry, dynamics, stroke interactions, or artist review.

Production integration uses the explicit experimental profile
`krita-5.3.4.1-generated-mask-candidate-1`. Original serialized metadata remains
separate from the active measured candidate state and native tip semantics.
No foreign renderer implementation or source brush pixels were copied into
production. No fixture identity or captured image is used to render a stroke.

## Original evidence and reproduction

- New controls: [run 42263154661](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42263154661),
  commit `bd06d01b962578fe2d00444c95751144d66ca7c5`, artifact
  `krita-reference-generatedcurves-bd06d01b962578fe2d00444c95751144d66ca7c5`,
  artifact digest `608e8d6d355f07017d6ac614c36939cbd3ed0ae8c7fc030327e52887384bdd4f`.
- Earlier controls and nonlinear/asymmetric holdouts:
  [run 42250057820](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42250057820),
  commit `703b77b82a9dae9895268d088a891f4836c6b3fe`, artifact
  `krita-reference-generatedtips-703b77b82a9dae9895268d088a891f4836c6b3fe`.
- Source application: pinned Krita 5.3.4.1 `bd6e8d0`, transparent sRGB RGBA8,
  fixed precision, explicit paintbrush engine, opacity/flow/pressure one,
  brush spacing one, angle zero, ratio one, antialiasEdges one, spikes two.
  Exact effective mask XML, delivered endpoints/vertices and source repeats
  remain in the original artifacts.

`generated_mask_analysis.py` reads the artifact's original BGRA capture notices,
effective controls and source records. If the process's final stdout suffix was
not flushed, it explicitly reads the corresponding complete original RGBA8 PNG
and reports that file's hash. The new acquisition uses this fallback for cases
122 and 123; the earlier acquisition uses it for case 031. The bounded PNG reader
checks CRCs, format, size and complete compressed data. It does not register,
translate, alter, or substitute any source image.

```text
python ci-scripts/brush-reference/generated_mask_analysis.py <artifact-directory>
```

The read-only JSON records per-case maximum error, MAE, original BGRA digest,
model alpha digest, input-report hashes and explicit research-only flags.
Eight earlier Default-generator cases are identified and skipped because they
belong to the existing independently measured Default profiles. All 24 earlier
Soft/Gaussian cases and all 124 new cases are measured. The helper has no source
admission or pass/fail gate, and the original comparator thresholds stay intact.
Its separate validation script contains actual source-pixel anchors and strict
artifact-input cases. No local builds or tests were run during this work; only
original captured-pixel numerical analysis was executed.

## Coordinates and shared curve semantics

Let `i,j` be integer source pixel indices, and `sx,sy` the authored tip origin.
The observed mask coordinates for these primitive captures are:

```text
x = abs(i - sx)
y = abs(j + 0.5 - sy)
r = diameter / 2
```

The apparent horizontal half-pixel difference is already represented by the
source placement adapter: physical pixel center `(i+.5,j+.5)` is measured from
mask center `(sx+.5,sy)`. Do not apply that adapter twice.

`C(t)` is the declared softness curve, evaluated as a natural cubic through its
normalized knots and clamped to [0,1] after interpolation. Constant curves and
noncanonical endpoint Y values must be preserved. The old nonlinear curves
were held out while constant and linear algebra was discriminated; they then
validated the resulting complementary-axis and squared-radius rules.

Final byte rounding is nearest with positive halves upward. Boundary formulas
use an upward byte quantization before the outer envelope. This matters for
constant three-quarter curves and partial edge coverage.

## Soft circle

The measurements distinguish a squared radial basis, a finite curve-resolution
factor and an outer antialias envelope. Let:

```text
q = (x*x + y*y) / (r*r)
a = (r-1)*(r-1) / (r*r)
N = 4 * diameter
```

For `q >= 1`, alpha is zero. For `q <= a`, the byte is
`round(255 * C(q*(N-1)/N))`. In the remaining outer ring:

```text
boundary = ceil(255 * C(a*(N-1)/N))
alpha = round(boundary * (1-q)/(1-a))
```

This matches all new circle constants, linear curves and origins, plus earlier
nonlinear controls and the inactive asymmetric Soft fade case. The `N` rule is
one geometric resolution law consistent with the observed diameters; it is not
a table of fixture results or proof for all unacquired diameters. A direct hard
cutoff or linear-distance AA would fail the captured outer ring substantially.

## Soft rectangle

For one axis, the response is the complementary product:

```text
F(u) = C(u) * (1 - C(1-u))
base = 255 * F(x/r) * F(y/r)
```

Identity curves produce squared axis products. Descending linear curves produce
squared inward distances. Constant `c` produces `255*c*c*(1-c)*(1-c)`, explaining
source bytes 9/16/9 for constants .25/.5/.75, with both zero and one empty.
Applying `C(u*u)` or squaring `C(u)` alone fails the nonlinear and constant
controls. Soft fade-axis attributes are preserved even though the two acquired
asymmetric Soft controls have identical masks to their symmetric counterparts.

The rectangle uses the measured block-dependent axis AA below. It does not use
the circular squared-distance envelope.

## Gaussian rectangle

Define a normalized integral of a symmetric Gaussian box:

```text
B(t; m,s) = [erf((m+t)/(s*sqrt(2))) + erf((m-t)/(s*sqrt(2)))]
           / [2*erf(m/(s*sqrt(2)))]
```

For each axis with fade `f`:

```text
m = r * (0.5 + 0.25*f)
s = r * (0.2 - 0.1*f)
base = 255 * B(x; mx,sx) * B(y; my,sy)
```

The horizontal and vertical parameters are independent. The five new symmetric
fade values 0/.1/.5/.9/1, asymmetric .1/.9 pairs, and older .25/.75 and asymmetric
controls all agree within one byte over the full masks. Axis AA and byte
quantization follow the same measured rectangle rule as Soft.

## Rectangle edge dispatch

The retained source output has a specific eight-column block behavior:

```text
left = round(sx) - r
block = left + floor((i-left)/8)*8
```

If any of the eight pixel columns in that block has `abs(column-sx) > r-1`, the
block uses axis envelopes for every lane:

```text
Ex = clamp(r-x, 0, 1)
Ey = clamp(r-y, 0, 1)
alpha = round(ceil(base) * Ex * Ey)
```

Other blocks use `round(base)`. Coordinates outside `x<=r` and `y<=r` are empty.
The inclusive bound matters: the fractional vertical control can retain a
boundary row in blocks that did not enable AA. This rule describes the pinned
source capture dispatch; it does not assert that every source CPU path uses it.

At an integer origin, the left block contains the exact negative boundary and
therefore enables AA, whereas the right full block may not. Fractional X
controls and the two diameters discriminate the anchor and block width. Using
`ceil(sx-r)` as the anchor fails the positive one-eighth origin cases. A global
axis envelope or a radial approximation cannot reproduce these masks.

## Gaussian circle

The circle uses the mean fade, a radial normalized Gaussian-box response and a
linear-distance outer envelope:

```text
f = (horizontalFade + verticalFade) / 2
s = 0.27 * r * (1-f)
m = r - 2.5*s
d = sqrt(x*x + y*y)
```

When `s=0`, use the limiting interior response one. Otherwise use `B(d;m,s)`
from the Gaussian rectangle definition. Alpha is zero for `d>=r`. For
`d<=r-1`, use `round(255*B(d;m,s))`. In the outer ring:

```text
boundary = ceil(255 * B(r-1;m,s))
alpha = round(boundary * (r-d))
```

The single coefficient 0.27 was independently estimated from original profiles.
It is not a claim that the foreign implementation contains that exact constant.
The family and this global coefficient predict all acquired fades, both sizes,
and the older .25/.75/asymmetric holdouts within one byte; the fade-one controls
are exact. A speculative native Gaussian shape is not substituted or relabeled.
The candidate remains experimental until the required full-image gates and
remaining semantic scope are satisfied. Further geometry/curve/dynamics data
must discriminate behavior outside this measured domain before admission.

## Report identities

| Evidence | New controls SHA-256 | Earlier controls SHA-256 |
| --- | --- | --- |
| fixtures.json | 2e03b30b98c1c0e2640626c11b75bb2f52b508e1c118c2abc465ec96280a1a3f | afc222adc28049351ae2e07843283d0d0dc0a372e1382c817f69d4df51263f96 |
| oracle-status.json | c64844a89888d58d67a9b62bf67065c2638a5c64cc2084ee90a20748bbf71b8e | 3cf1f8c5603f2f6d47709c9189b5968fe013ba6ceb7d08e46823917193d20739 |
| runtime.log | e73e97d83d0fa497a1124b68b00f98ae8a446ed79757d50e5e867d5efac51c4a | 8a03a4669b0a38a2562691763286a730c36376ebf6185d0bd169fcbefaa32186 |

The acquired domain is diameter 16/64, unit aspect, zero angle, AA enabled,
static controls and the documented paths/origins. Other sizes, anisotropy,
rotation, AA disabled, dynamic size/ratio/rotation, other engines and cross-control
combinations remain open. Typed metadata preservation does not establish those
rendering semantics, and this bounded implementation does not fulfill all of
RCP1 by itself.
