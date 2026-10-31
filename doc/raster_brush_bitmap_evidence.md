# Fixed bitmap source-profile evidence

The original `bitmaptips` source captures cover 32 combinations of an asymmetric
8×8 grayscale GBR, a PNG used as mask or color, and an incremental GIH: sizes
8/16, view rotations 0/90°, and short/long paths. They are measurements of Krita
5.3.4.1, not proof of the native bitmap conversion's behavioral equivalence.

The initial unrotated GBR stamp occupied 8×7 pixels at size 8 and 16×15 at size
16. PNG mask/color occupied 9×8 and 17×16 respectively. The native sampler's
supports differed. These observations distinguish resource transforms from the
existing generated-tip lattice, but do not uniquely determine interpolation,
finite edge support, phase quantization, or premultiplied color rounding.
The comparison view-rotation correction applies the authored view modifier;
it does not implement source bitmap rendering.

`bitmap_diagnostics.py` appends 35 independently generated CC0 discriminator
cases without changing the original 32 case identities. The seven inputs are:
center impulse, corner impulse, grayscale ramp, odd non-square 5×7 impulse and
solid mask, plus an original RGBA grid used separately as color and mask. Each
has baseline scale 1, quarter-pixel placement, scale 1.5, scale 2, and 37° view
rotation controls. All 35 new controls use a bounded two-vertex public
`Node.paintPath` call, preserving fractional positions; paired integer controls
use the same API. The original 32 keep their `Node.paintLine` delivery. Actual
vertices are recorded and checked by comparison. Packages retain original resource hashes; source acquisition
checks resource resolution and captures a fixed 48×48 full BGRA region. These
are acquisition fixtures, not a fitted runtime oracle or passing fidelity gate.

The impulse/edge cases distinguish filter support and origin; the ramp exposes
byte rounding; odd dimensions distinguish nominal-size scaling from axis-specific
centering. RGBA cases distinguish color sampling/premultiplication from mask
luminance. Their source results must be examined before selecting a reusable
sampling equation. Exact producer repeats and full-region comparisons must then
qualify that equation on held-out patterns and transforms.

A future versioned fixed-bitmap source adapter must admit only resolved resources
and explicitly implemented application modes/settings. It must preserve native
sampling semantics, carry resource identity through portable save/reopen, account
for filtering work and storage, and use qualified source spacing/endpoints.
GIH selection/cadence remains a separate qualification problem; this matrix does
not establish random-sequence state or source bitmap parity. Fractional scaling and arbitrary rotation remain outside source eligibility
until their sampling evidence is resolved.

## Implemented bounded candidate

`krita-5.3.4.1-fixed-bitmap-candidate-1` independently implements clamped-edge
bilinear pyramid construction followed by combined inverse affine sampling.
It does not change native bitmap sampling. The admitted candidate geometry is scale 1–2, fixed orientation, static
size/aspect, Normal
build-up, full opacity/flow, and no sensors, texture, wet state or secondary tip.
GBR grayscale coverage and original GBR/PNG color images are admitted. The
separate `krita-5.3.4.1-fixed-bitmap-mask-candidate-1` profile admits original PNG
mask application while retaining the original RGBA resource. Strict import checks
resource origin before native mask conversion can discard original color.

Color samples interpolate premultiplied channels, quantize final coverage down
to a byte, and retain sampled straight RGB. Grayscale pyramid coverage uses upward byte quantization and final
nearest-byte coverage; non-quarter-turn color transforms use nearest-byte
coverage, while axis-aligned color uses downward coverage quantization. The complete captured regions—including zero pixels—gate maximum
alpha error at one byte, premultiplied RGB error at two bytes and alpha-support
IoU at exactly 1. These tolerances do not imply identical internal equations.
Forty-seven original captured regions cover asymmetric tips, impulses, ramps,
odd dimensions, fractional placement, quarter turns and general RGBA colors.
The reference comparator enforces the same gates for admitted cases; other
views are explicitly measured as native conversions, never passing source cases.

On-demand two-stage sampling charges sixteen image samples per destination
pixel before host writes and creates no transformed-image cache. Original image
storage remains charged by existing shared stroke/resource budgets. Preset and
portable codecs preserve the versioned profile and image payload; unsupported
active settings remain labeled native conversions. Equal-size deterministic
frame arrays can use the runtime's per-stroke dab index, but GIH source admission
and cadence qualification are owned by their separate adapter.

The fractional-scale captures distinguish the stages: first form the clamped
2× pyramid, then combine residual scale, rotation and fractional placement in
one sample. Preserve nominal fractional dimensions (5×7 at 1.5× remains
7.5×10.5); rounding to 8×11 before placement causes large edge errors. The 1.5×
impulse and odd-size controls and 37° unit-scale captures meet the same region
gates under the combined model. Additional held-out scales/angle combinations,
downscaling, scales above 2, dynamic size/orientation and
other source controls remain unfinished. The candidate status is preserved;
this is not completion of the full bitmap contract.

A further 21 prospective held-out controls append after those captures without
renumbering them: all seven original assets at 1.25×/−23°, 1.75×/+61° with
quarter-pixel placement, and 1.01×/0° across the pyramid boundary. They test the
combined affine model rather than adjusting it to those results. Existing
full-region tolerances remain unchanged; acquisition/qualification is pending.

A separate 35-control acquisition extends those same original assets to scales
0.5, 0.75, 2.01, 3 and 4. The 0.75 and 3 controls also vary phase and rotation;
the remaining controls isolate scale. Existing 88 case identities are preserved.
These cases measure reduction and later pyramid levels; they do not expand the
runtime admission range or count as source parity before their results pass the
unchanged full-region gates. All measured regions remain 48×48 at fixed scale.


The PNG-mask model preserves premultiplied color through the clamped pyramid
and final affine sample. Its original premultiplied RGB is rounded to bytes;
after sampling, straight RGB and alpha are floored to bytes, then inverse
[qGray](https://doc.qt.io/qt-6/qcolor.html#qGray) coverage is multiplied by alpha
and rounded to a byte. These are independently evaluated mathematical stages,
not copied rendering code. All nine original mask regions satisfy the unchanged
one-byte alpha and exact-support gates; the RGB gate also checks foreground
color rather than retained tip pigment. Earlier conversion, even immediately
after the pyramid, fails these controls. Prospective heldout controls remain
necessary qualification; these measurements do not establish universal PNG
mask or color-management equivalence.

Strict import retains RGBA only for this explicit source candidate. If any
active setting is unsupported, the rejected candidate alias is released and
the existing native mask conversion runs in place, preserving decoded-memory
limits. Codec and portable-preset tests retain the profile and original pixels;
there is no transformed-raster allocation or altered native sampler.
