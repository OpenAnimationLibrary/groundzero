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
not establish random-sequence state or source bitmap parity. Runtime and importer
eligibility are deliberately unchanged until the missing sampling evidence is
available.
